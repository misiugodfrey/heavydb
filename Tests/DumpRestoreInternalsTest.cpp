/*
 * SPDX-FileCopyrightText: Copyright (c) 2019-2026, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file DumpRestoreInternalsTest.cpp
 * @brief The parts of DumpRestoreTest that need the in-process engine.
 *
 * TableWithUncappedEpoch calls the test-only Catalog::setUncappedTableEpoch(), and
 * S3RestoreTest creates S3 clients from the AWS SDK and credentials of the test process,
 * so they cannot run as thin clients against a separate heavydb process.
 */

#include <algorithm>
#include <limits>
#include <vector>

#include <gtest/gtest.h>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/join.hpp>
#include <boost/process.hpp>
#ifdef HAVE_BOOST_1_86
#include <boost/process/v1/search_path.hpp>
#else
#include <boost/process/search_path.hpp>
#endif
#include <boost/program_options.hpp>
#include <boost/variant.hpp>
#include <boost/variant/get.hpp>

#ifdef HAVE_AWS_S3
#include "AwsHelpers.h"
#include "DataMgr/HeavyDbAwsSdk.h"
#endif
#include "QueryEngine/ResultSet.h"
#include "QueryRunner/QueryRunner.h"
#include "TestHelpers.h"

#ifndef BASE_PATH
#define BASE_PATH "./tmp"
#endif

using namespace TestHelpers;

extern bool g_allow_s3_server_privileges;

const static std::string tar_ball_path = "/tmp/_Orz__";

namespace {
bool g_hoist_literals{true};

using QR = QueryRunner::QueryRunner;

inline void run_ddl_statement(const std::string& create_table_stmt) {
  QR::get()->runDDLStatement(create_table_stmt);
}

std::shared_ptr<ResultSet> run_multiple_agg(const std::string& query_str) {
  return QR::get()->runSQL(query_str, ExecutorDeviceType::CPU, g_hoist_literals);
}

}  // namespace

class DumpAndRestoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    boost::filesystem::remove_all(tar_ball_path);
    run_ddl_statement("DROP TABLE IF EXISTS test_table;");
    run_ddl_statement("DROP TABLE IF EXISTS test_table_2;");
    run_ddl_statement("DROP TABLE IF EXISTS render_groups;");
  }

  void TearDown() override {
    boost::filesystem::remove_all(tar_ball_path);
    run_ddl_statement("DROP TABLE IF EXISTS test_table;");
    run_ddl_statement("DROP TABLE IF EXISTS test_table_2;");
    run_ddl_statement("DROP TABLE IF EXISTS render_groups;");
  }

  void queryAndAssertException(const std::string& query,
                               const std::string& error_message) {
    try {
      run_ddl_statement(query);
      FAIL() << "An exception should have been thrown for this test case";
    } catch (const std::exception& e) {
      ASSERT_STREQ(error_message.c_str(), e.what());
    }
  }

  void sqlAndCompareResult(const std::string& sql,
                           const std::vector<std::string>& expected_result) {
    auto result = run_multiple_agg(sql);
    ASSERT_EQ(expected_result.size(), result->colCount());
    auto row = result->getNextRow(true, true);
    for (size_t i = 0; i < expected_result.size(); i++) {
      auto& target_value = boost::get<ScalarTargetValue>(row[i]);
      auto& nullable_str = boost::get<NullableString>(target_value);
      auto& str = boost::get<std::string>(nullable_str);
      EXPECT_EQ(expected_result[i], str);
    }
  }

  void sqlAndCompareArrayResult(
      const std::string& sql,
      const std::vector<std::vector<std::string>>& expected_result) {
    auto result = run_multiple_agg(sql);
    ASSERT_EQ(expected_result.size(), result->colCount());
    auto row = result->getNextRow(true, true);
    for (size_t i = 0; i < expected_result.size(); i++) {
      auto& expected_array = expected_result[i];
      auto& target_value_vector = boost::get<ArrayTargetValue>(row[i]).get();
      ASSERT_EQ(expected_array.size(), target_value_vector.size());
      size_t j = 0;
      for (const auto& target_value : target_value_vector) {
        auto& nullable_str = boost::get<NullableString>(target_value);
        auto& str = boost::get<std::string>(nullable_str);
        EXPECT_EQ(expected_array[j], str);
        j++;
      }
    }
  }

  void sqlAndCompareResult(const std::string& sql,
                           const std::vector<int64_t>& expected_values) {
    auto result = run_multiple_agg(sql);
    ASSERT_EQ(expected_values.size(), result->rowCount());
    for (auto expected_value : expected_values) {
      auto row = result->getNextRow(true, true);
      auto& target_value = boost::get<ScalarTargetValue>(row[0]);
      EXPECT_EQ(expected_value, boost::get<int64_t>(target_value));
    }
  }
};

TEST_F(DumpAndRestoreTest, TableWithUncappedEpoch) {
  run_ddl_statement("CREATE TABLE test_table (i INTEGER);");
  QR::get()->getCatalog()->setUncappedTableEpoch("test_table");
  run_multiple_agg("INSERT INTO test_table VALUES(1);");
  run_multiple_agg("INSERT INTO test_table VALUES(2);");
  run_multiple_agg("INSERT INTO test_table VALUES(3);");

  sqlAndCompareResult("SELECT * FROM test_table;", {1, 2, 3});
  run_ddl_statement("DUMP TABLE test_table TO '" + tar_ball_path + "';");
  run_ddl_statement("RESTORE TABLE test_table_2 FROM '" + tar_ball_path + "';");
  sqlAndCompareResult("SELECT * FROM test_table_2;", {1, 2, 3});
  auto td = QR::get()->getCatalog()->getMetadataForTable("test_table_2", false);
  ASSERT_TRUE(td != nullptr);
  ASSERT_EQ(DEFAULT_MAX_ROLLBACK_EPOCHS, td->maxRollbackEpochs);
}

#ifdef HAVE_AWS_S3
class S3RestoreTest : public DumpAndRestoreTest {
 protected:
  void SetUp() override {
    g_allow_s3_server_privileges = false;
    run_ddl_statement("DROP TABLE IF EXISTS test_table;");
  }

  void TearDown() override {
    g_allow_s3_server_privileges = false;
    run_ddl_statement("DROP TABLE IF EXISTS test_table;");
  }

  bool insufficientPrivateCredentials() const {
    return !is_valid_aws_key(get_aws_keys_from_env());
  }

  void queryAndAssertPartialException(const std::string& query,
                                      const std::string& error_message) {
    try {
      run_ddl_statement(query);
      FAIL() << "An exception should have been thrown for this test case";
    } catch (const std::exception& e) {
      std::string exception_message{e.what()};
      ASSERT_TRUE(exception_message.find(error_message) != std::string::npos)
          << "Exception message: " << exception_message
          << ", expected partial error message: " << error_message;
    }
  }

  std::string getRestoreQuery(const std::string& object_key,
                              const std::string& region = aws_region_,
                              const std::string& access_key = {},
                              const std::string& secret_key = {},
                              const std::string& session_token = {},
                              const std::string& endpoint = {}) {
    std::string query{"RESTORE TABLE test_table FROM 's3://" + object_key +
                      "' "
                      "WITH (compression = 'gzip'"};
    if (!region.empty()) {
      query += ", s3_region = '" + region + "'";
    }
    if (!access_key.empty()) {
      query += ", s3_access_key = '" + access_key + "'";
    }
    if (!secret_key.empty()) {
      query += ", s3_secret_key = '" + secret_key + "'";
    }
    if (!session_token.empty()) {
      query += ", s3_session_token = '" + session_token + "'";
    }
    if (!endpoint.empty()) {
      query += ", s3_endpoint = '" + endpoint + "'";
    }
    query += ");";
    return query;
  }

  static inline const std::string aws_region_{"us-west-1"};
  static inline const std::string dump_file_name_{"test_table_dump.gz"};
  static inline const std::string public_bucket_{"heavydb-dump-restore-test-public"};
  static inline const std::string public_object_key_{
      "heavydb-dump-restore-test-public/test_table_dump.gz"};
  static inline const std::string private_object_key_{
      "heavydb-dump-restore-test/test_table_dump.gz"};
  static inline const std::string gcs_object_key_{
      "omnisci-importtest-data/dump-restore-test-data/test_table_dump.gz"};
};

// TODO(IAM): re-enable once a CI IAM user with read on
// heavydb-dump-restore-test-public is provisioned. The current CI user
// has no GetObject on that bucket, so this hits AccessDenied.
TEST_F(S3RestoreTest, DISABLED_PublicBucket) {
  run_ddl_statement(getRestoreQuery(public_object_key_));
  sqlAndCompareResult("SELECT * FROM test_table ORDER BY i;", {1, 2, 3});
}

TEST_F(S3RestoreTest, PrivateBucket) {
  if (insufficientPrivateCredentials()) {
    GTEST_SKIP() << "Insufficient private credentials to run test";
  }
  const auto [access_key, secret_key] = get_aws_keys_from_env();
  run_ddl_statement(
      getRestoreQuery(private_object_key_, aws_region_, access_key, secret_key));
  sqlAndCompareResult("SELECT * FROM test_table ORDER BY i;", {1, 2, 3});
}

TEST_F(S3RestoreTest, PrivateBucketUsingSessionToken) {
  if (insufficientPrivateCredentials()) {
    GTEST_SKIP() << "Insufficient private credentials to run test";
  }
  const auto aws_keys = get_aws_keys_from_env();
  Aws::Client::ClientConfiguration client_config;
  client_config.region = aws_region_;
  const auto sts_credentials = generate_sts_credentials(aws_keys, client_config);

  run_ddl_statement(getRestoreQuery(private_object_key_,
                                    aws_region_,
                                    sts_credentials.GetAccessKeyId(),
                                    sts_credentials.GetSecretAccessKey(),
                                    sts_credentials.GetSessionToken()));
  sqlAndCompareResult("SELECT * FROM test_table ORDER BY i;", {1, 2, 3});
}

TEST_F(S3RestoreTest, PrivateBucketWithServerPrivileges) {
  if (insufficientPrivateCredentials()) {
    GTEST_SKIP() << "Insufficient private credentials to run test";
  }
  g_allow_s3_server_privileges = true;
  run_ddl_statement(getRestoreQuery(private_object_key_));
  sqlAndCompareResult("SELECT * FROM test_table ORDER BY i;", {1, 2, 3});
}

// TODO(IAM): re-enable once a CI IAM user with read on
// omnisci-importtest-data is provisioned.
TEST_F(S3RestoreTest, DISABLED_CustomS3Endpoint) {
  run_ddl_statement(getRestoreQuery(
      gcs_object_key_, aws_region_, {}, {}, {}, "storage.googleapis.com"));
  sqlAndCompareResult("SELECT * FROM test_table ORDER BY i;", {1, 2, 3});
}

TEST_F(S3RestoreTest, PrivateBucketNoAccess) {
  queryAndAssertPartialException(
      getRestoreQuery(
          private_object_key_, aws_region_, "invalid_access_key", "invalid_secret_key"),
      "failed to get object '" + dump_file_name_ + "' of s3 url 's3://" +
          private_object_key_ + "': InvalidAccessKeyId");
}

TEST_F(S3RestoreTest, WrongOptionType) {
  queryAndAssertException("RESTORE TABLE test_table FROM 's3://" + public_object_key_ +
                              "' "
                              "WITH (compression = 'gzip', s3_region = 10);",
                          "\"s3_region\" option must be a string.");
}

TEST_F(S3RestoreTest, MissingRegion) {
  queryAndAssertException(getRestoreQuery(public_object_key_, {}),
                          "Required parameter \"s3_region\" not set. Please specify the "
                          "\"s3_region\" configuration parameter.");
}

// TODO(IAM): re-enable once a CI IAM user with read on
// heavydb-dump-restore-test-public is provisioned.
TEST_F(S3RestoreTest, DISABLED_S3BucketWithMultipleFiles) {
  queryAndAssertException(
      getRestoreQuery(public_bucket_),
      "S3 URI references multiple files. Only one file can be restored at a time.");
}
#endif

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);

  namespace po = boost::program_options;
  po::options_description desc("Options");

  logger::LogOptions log_options(argv[0]);
  log_options.max_files_ = 0;  // stderr only by default
  log_options.set_base_path(BASE_PATH);
  desc.add(log_options.get_options());

  po::variables_map vm;
  po::store(po::command_line_parser(argc, argv).options(desc).run(), vm);
  po::notify(vm);

  logger::init(log_options);

  QR::init(BASE_PATH);

#ifdef HAVE_AWS_S3
  heavydb_aws_sdk::init_sdk();
#endif

  int err{0};
  try {
    err = RUN_ALL_TESTS();
  } catch (const std::exception& e) {
    LOG(ERROR) << e.what();
  }

#ifdef HAVE_AWS_S3
  heavydb_aws_sdk::shutdown_sdk();
#endif

  QR::reset();
  return err;
}
