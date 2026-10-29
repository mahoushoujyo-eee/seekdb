/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "sql/engine/basic/ob_file_scan_utils.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <vector>

#include "lib/ob_errno.h"
#include "gtest/gtest.h"

namespace oceanbase
{
namespace sql
{
namespace file_sql
{
namespace
{
class TemporaryCsv
{
public:
  TemporaryCsv()
  {
    const int64_t nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = "file_sql_test_" + std::to_string(nonce) + ".csv";
  }
  ~TemporaryCsv() { std::remove(path_.c_str()); }
  bool write(const std::string &contents)
  {
    std::ofstream output(path_, std::ios::binary | std::ios::trunc);
    output << contents;
    return output.good();
  }
  const std::string &path() const { return path_; }
private:
  std::string path_;
};
} // namespace

TEST(TestFileScanUtils, parses_multiline_crlf_and_escaped_quotes)
{
  TemporaryCsv file;
  ASSERT_TRUE(file.write("id,note\r\n1,\"hello,\r\nworld\"\r\n2,\"say \"\"hi\"\"\"\r\n"));
  CsvOptions options;
  CsvReader reader;
  ASSERT_EQ(OB_SUCCESS, reader.open(file.path(), options));

  CsvRecord record;
  bool has_record = false;
  ASSERT_EQ(OB_SUCCESS, reader.next(record, has_record));
  ASSERT_TRUE(has_record);
  ASSERT_EQ(2U, record.fields_.size());
  EXPECT_EQ("id", record.fields_[0].value_);
  EXPECT_EQ("note", record.fields_[1].value_);

  ASSERT_EQ(OB_SUCCESS, reader.next(record, has_record));
  ASSERT_TRUE(has_record);
  EXPECT_EQ("hello,\nworld", record.fields_[1].value_);

  ASSERT_EQ(OB_SUCCESS, reader.next(record, has_record));
  ASSERT_TRUE(has_record);
  EXPECT_EQ("say \"hi\"", record.fields_[1].value_);

  ASSERT_EQ(OB_SUCCESS, reader.next(record, has_record));
  EXPECT_FALSE(has_record);
}

TEST(TestFileScanUtils, infers_full_schema_and_deduplicates_headers)
{
  TemporaryCsv file;
  ASSERT_TRUE(file.write("id,name,score,active,day,created,id\n"
                         "1,Alice,98.5,true,2026-08-01,2026-08-01 10:11:12,101\n"
                         "2,Bob,87,false,,,102\n"));
  CsvOptions options;
  std::vector<CsvColumn> columns;
  ASSERT_EQ(OB_SUCCESS, infer_csv_schema(file.path(), options, columns));
  ASSERT_EQ(7U, columns.size());
  EXPECT_EQ("id", columns[0].name_);
  EXPECT_EQ("id__2", columns[6].name_);
  EXPECT_EQ(InferredType::BIGINT, columns[0].type_);
  EXPECT_EQ(InferredType::VARCHAR, columns[1].type_);
  EXPECT_EQ(InferredType::DOUBLE, columns[2].type_);
  EXPECT_EQ(InferredType::BOOLEAN, columns[3].type_);
  EXPECT_EQ(InferredType::DATE, columns[4].type_);
  EXPECT_EQ(InferredType::DATETIME, columns[5].type_);
  EXPECT_FALSE(columns[0].nullable_);
  EXPECT_TRUE(columns[4].nullable_);
}

TEST(TestFileScanUtils, rejects_bad_rows_and_file_changes)
{
  TemporaryCsv bad_file;
  ASSERT_TRUE(bad_file.write("a,b\n1\n"));
  CsvOptions options;
  std::vector<CsvColumn> columns;
  EXPECT_EQ(OB_INVALID_DATA, infer_csv_schema(bad_file.path(), options, columns));

  TemporaryCsv changed_file;
  ASSERT_TRUE(changed_file.write("a\n1\n2\n"));
  CsvReader reader;
  ASSERT_EQ(OB_SUCCESS, reader.open(changed_file.path(), options));
  CsvRecord record;
  bool has_record = false;
  ASSERT_EQ(OB_SUCCESS, reader.next(record, has_record));
  ASSERT_TRUE(has_record);
  ASSERT_EQ(OB_SUCCESS, reader.next(record, has_record));
  ASSERT_TRUE(has_record);
  ASSERT_TRUE(changed_file.write("a\n1\n2\n3\n"));
  EXPECT_EQ(OB_ERR_UNEXPECTED, reader.next(record, has_record));
}

TEST(TestFileScanUtils, applies_deterministic_widening_and_null_schema)
{
  TemporaryCsv file;
  ASSERT_TRUE(file.write("n,date,all_null\n1,2026-02-28,\n2.5,2024-02-29,\n"));
  CsvOptions options;
  std::vector<CsvColumn> columns;
  ASSERT_EQ(OB_SUCCESS, infer_csv_schema(file.path(), options, columns));
  ASSERT_EQ(3U, columns.size());
  EXPECT_EQ(InferredType::DOUBLE, columns[0].type_);
  EXPECT_EQ(InferredType::DATE, columns[1].type_);
  EXPECT_EQ(InferredType::NULL_TYPE, columns[2].type_);
  EXPECT_TRUE(columns[2].nullable_);
}

TEST(TestFileScanUtils, reads_jsonl_union_and_rejects_nested_values)
{
  TemporaryCsv file;
  ASSERT_TRUE(file.write("{\"id\":1,\"name\":\"Alice\",\"active\":true}\n"
                         "{\"id\":2,\"score\":2.5,\"name\":\"Bob\\u0021\"}\r\n"));
  JsonOptions options;
  JsonlReader reader;
  ASSERT_EQ(OB_SUCCESS, reader.open(file.path(), options));
  JsonRecord record;
  bool has_record = false;
  ASSERT_EQ(OB_SUCCESS, reader.next(record, has_record));
  ASSERT_TRUE(has_record);
  ASSERT_EQ(3U, record.fields_.size());
  EXPECT_EQ("id", record.fields_[0].name_);
  EXPECT_EQ("1", record.fields_[0].value_);
  EXPECT_EQ(InferredType::BOOLEAN, record.fields_[2].type_);
  ASSERT_EQ(OB_SUCCESS, reader.next(record, has_record));
  ASSERT_TRUE(has_record);
  EXPECT_EQ("Bob!", record.fields_[2].value_);
  ASSERT_EQ(OB_SUCCESS, reader.next(record, has_record));
  EXPECT_FALSE(has_record);

  std::vector<CsvColumn> columns;
  ASSERT_EQ(OB_SUCCESS, infer_jsonl_schema(file.path(), options, columns));
  ASSERT_EQ(4U, columns.size());
  EXPECT_EQ("id", columns[0].name_);
  EXPECT_EQ(InferredType::BIGINT, columns[0].type_);
  EXPECT_EQ(InferredType::VARCHAR, columns[1].type_);
  EXPECT_EQ(InferredType::BOOLEAN, columns[2].type_);
  EXPECT_EQ(InferredType::DOUBLE, columns[3].type_);
  EXPECT_TRUE(columns[2].nullable_);
  EXPECT_FALSE(columns[1].nullable_);

  ASSERT_TRUE(file.write("{\"nested\":{\"x\":1}}\n"));
  EXPECT_EQ(OB_NOT_SUPPORTED, infer_jsonl_schema(file.path(), options, columns));
}

TEST(TestFileScanUtils, reports_jsonl_bad_line_and_file_discovery)
{
  TemporaryCsv file;
  ASSERT_TRUE(file.write("{\"a\":1}\n{\"a\":}\n"));
  JsonOptions options;
  JsonlReader reader;
  ASSERT_EQ(OB_SUCCESS, reader.open(file.path(), options));
  JsonRecord record;
  bool has_record = false;
  ASSERT_EQ(OB_SUCCESS, reader.next(record, has_record));
  ASSERT_TRUE(has_record);
  EXPECT_EQ(OB_INVALID_DATA, reader.next(record, has_record));
  EXPECT_EQ(2, record.line_number_);

  TemporaryCsv second;
  ASSERT_TRUE(second.write("{\"b\":2}\n"));
  std::vector<std::string> files;
  const std::string pattern = file.path().substr(0, file.path().find_last_of('_') + 1) + "*.csv";
  ASSERT_EQ(OB_SUCCESS, discover_files(pattern, files));
  EXPECT_GE(files.size(), 2U);
}

} // namespace file_sql
} // namespace sql
} // namespace oceanbase
