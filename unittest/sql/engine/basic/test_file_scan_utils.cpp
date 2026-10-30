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
#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "lib/ob_errno.h"
#include "gtest/gtest.h"
#include "file_sql_parquet_fixtures.h"

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

TEST(TestFileScanUtils, preserves_numeric_boundaries_and_validates_dates)
{
  TemporaryCsv file;
  ASSERT_TRUE(file.write("lo,hi,overflow,day,invalid_day,all_null\n"
                        "-9223372036854775808,9223372036854775807,9223372036854775808,2024-02-29,2026-02-29,\n"));
  CsvOptions options;
  std::vector<CsvColumn> columns;
  ASSERT_EQ(OB_SUCCESS, infer_csv_schema(file.path(), options, columns));
  ASSERT_EQ(6U, columns.size());
  EXPECT_EQ(InferredType::BIGINT, columns[0].type_);
  EXPECT_EQ(InferredType::BIGINT, columns[1].type_);
  EXPECT_EQ(InferredType::VARCHAR, columns[2].type_);
  EXPECT_EQ(InferredType::DATE, columns[3].type_);
  EXPECT_EQ(InferredType::VARCHAR, columns[4].type_);
  EXPECT_EQ(InferredType::NULL_TYPE, columns[5].type_);
}

TEST(TestFileScanUtils, enforces_record_limits_and_utf8)
{
  TemporaryCsv file;
  ASSERT_TRUE(file.write("a;b\n1;\"say \\\"hi\\\"\"\n"));
  CsvOptions options;
  options.delimiter_ = ';';
  options.escape_ = '\\';
  std::vector<CsvColumn> columns;
  ASSERT_EQ(OB_SUCCESS, infer_csv_schema(file.path(), options, columns));
  EXPECT_EQ(2U, columns.size());
  ASSERT_TRUE(file.write("x\n123456789\n"));
  options.max_field_bytes_ = 4;
  EXPECT_EQ(OB_SIZE_OVERFLOW, infer_csv_schema(file.path(), options, columns));
  options = CsvOptions();
  ASSERT_TRUE(file.write(std::string("x\n") + char(0xc0) + char(0xaf) + "\n"));
  EXPECT_EQ(OB_INVALID_DATA, infer_csv_schema(file.path(), options, columns));
}

TEST(TestFileScanUtils, reads_custom_quotes_escape_and_null_text)
{
  TemporaryCsv file;
  ASSERT_TRUE(file.write("id;note\n1;'hello;world'\n2;'say \\'hi\\''\n3;MISSING\n4;'MISSING'\n"));
  CsvOptions options;
  options.delimiter_ = ';';
  options.quote_ = '\'';
  options.escape_ = '\\';
  options.null_string_ = "MISSING";
  CsvReader reader;
  ASSERT_EQ(OB_SUCCESS, reader.open(file.path(), options));
  CsvRecord row;
  bool has_row = false;
  ASSERT_EQ(OB_SUCCESS, reader.next(row, has_row));
  const char *expected[] = {"hello;world", "say 'hi'", "MISSING", "MISSING"};
  for (int i = 0; i < 4; ++i) {
    ASSERT_EQ(OB_SUCCESS, reader.next(row, has_row));
    ASSERT_TRUE(has_row);
    ASSERT_EQ(2U, row.fields_.size());
    EXPECT_EQ(expected[i], row.fields_[1].value_);
    EXPECT_EQ(i == 2, row.fields_[1].is_null_);
  }
}

TEST(TestFileScanUtils, decodes_snappy_nullable_and_dictionary_pages)
{
  for (int version = 1; version <= 2; ++version) {
    SCOPED_TRACE(version);
    TemporaryCsv file;
    ASSERT_TRUE(file.write(version == 1
      ? std::string(reinterpret_cast<const char *>(PARQUET_V1), sizeof(PARQUET_V1))
      : std::string(reinterpret_cast<const char *>(PARQUET_V2), sizeof(PARQUET_V2))));
    std::vector<CsvColumn> columns;
    ASSERT_EQ(OB_SUCCESS, infer_parquet_schema(file.path(), columns));
    ASSERT_EQ(6U, columns.size());
    EXPECT_EQ(InferredType::BIGINT, columns[0].type_);
    EXPECT_EQ(InferredType::VARCHAR, columns[1].type_);
    EXPECT_EQ(InferredType::BOOLEAN, columns[2].type_);
    EXPECT_EQ(InferredType::DOUBLE, columns[3].type_);
    EXPECT_EQ(InferredType::DATE, columns[4].type_);
    EXPECT_EQ(InferredType::DATETIME, columns[5].type_);
    EXPECT_TRUE(columns[1].nullable_);
    ParquetReader reader;
    ASSERT_EQ(OB_SUCCESS, reader.open(file.path(), {"id", "name", "active", "score", "day", "created"}));
    const char *names[] = {"alpha", "", "alpha", "beta", "", "beta"};
    const char *active[] = {"true", "false", "", "true", "false", "true"};
    const char *scores[] = {"1.5", "", "3.25", "4", "5.5", "6"};
    for (int i = 0; i < 6; ++i) {
      ParquetRecord row;
      bool has_row = false;
      ASSERT_EQ(OB_SUCCESS, reader.next(row, has_row));
      ASSERT_TRUE(has_row);
      ASSERT_EQ(6U, row.fields_.size());
      EXPECT_EQ(std::to_string(i + 1), row.fields_[0].value_);
      EXPECT_EQ(i == 1, row.fields_[1].is_null_);
      EXPECT_EQ(names[i], row.fields_[1].value_);
      EXPECT_EQ(i == 2, row.fields_[2].is_null_);
      EXPECT_EQ(active[i], row.fields_[2].value_);
      EXPECT_EQ(i == 1, row.fields_[3].is_null_);
      EXPECT_EQ(scores[i], row.fields_[3].value_);
      EXPECT_EQ(i % 3 == 1, row.fields_[4].is_null_);
      EXPECT_EQ(i % 3 == 1, row.fields_[5].is_null_);
      if (i % 3 == 0) {
        EXPECT_EQ("2026-08-01", row.fields_[4].value_);
        EXPECT_EQ("2026-08-01 10:11:12.123456", row.fields_[5].value_);
      } else if (i % 3 == 2) {
        EXPECT_EQ("1969-12-31", row.fields_[4].value_);
        EXPECT_EQ("1969-12-31 23:59:59.999999", row.fields_[5].value_);
      }
    }
    ParquetRecord row;
    bool has_row = true;
    ASSERT_EQ(OB_SUCCESS, reader.next(row, has_row));
    EXPECT_FALSE(has_row);
    // Reopening resets the cursor; only requested columns are decoded.
    ASSERT_EQ(OB_SUCCESS, reader.open(file.path(), {"name"}));
    ASSERT_EQ(OB_SUCCESS, reader.next(row, has_row));
    ASSERT_TRUE(has_row);
    ASSERT_EQ(1U, row.fields_.size());
    EXPECT_EQ("alpha", row.fields_[0].value_);
    ASSERT_TRUE(file.write("changed"));
    EXPECT_EQ(OB_ERR_UNEXPECTED, reader.next(row, has_row));
  }
}

TEST(TestFileScanUtils, prunes_unselected_parquet_column_pages)
{
  TemporaryCsv file;
  std::string data(reinterpret_cast<const char *>(PARQUET_V1), sizeof(PARQUET_V1));
  data[PARQUET_V1_NAME_OFFSET] = 0; // Invalid page header in a column we do not read.
  ASSERT_TRUE(file.write(data));
  ParquetReader reader;
  ASSERT_EQ(OB_SUCCESS, reader.open(file.path(), {"id"}));
  for (int i = 1; i <= 6; ++i) {
    ParquetRecord row;
    bool has_row = false;
    ASSERT_EQ(OB_SUCCESS, reader.next(row, has_row));
    ASSERT_TRUE(has_row);
    ASSERT_EQ(1U, row.fields_.size());
    EXPECT_EQ(std::to_string(i), row.fields_[0].value_);
  }
  ASSERT_EQ(OB_SUCCESS, reader.open(file.path(), {"name"}));
  ParquetRecord row;
  bool has_row = false;
  EXPECT_NE(OB_SUCCESS, reader.next(row, has_row));
}

TEST(TestFileScanUtils, streams_parquet_pages_in_one_row_group)
{
  TemporaryCsv file;
  const std::string data(reinterpret_cast<const char *>(PARQUET_STREAMING), sizeof(PARQUET_STREAMING));
  ASSERT_TRUE(file.write(data));
  ParquetReader reader;
  ASSERT_EQ(OB_SUCCESS, reader.open(file.path(), {"id"}));
  ParquetRecord row;
  bool has_row = false;
  for (int i = 0; i < 512; ++i) {
    ASSERT_EQ(OB_SUCCESS, reader.next(row, has_row));
    ASSERT_TRUE(has_row);
    EXPECT_EQ(std::to_string(i), row.fields_[0].value_);
  }
  ASSERT_EQ(OB_SUCCESS, reader.next(row, has_row));
  EXPECT_FALSE(has_row);
}

#if !defined(_WIN32)
TEST(TestFileScanUtils, requires_secure_directory_and_path_boundary)
{
  char cwd[4096] = {0};
  ASSERT_NE(nullptr, ::getcwd(cwd, sizeof(cwd)));
  const std::string root(cwd);
  EXPECT_EQ(OB_SUCCESS, check_secure_file_path(".", root + "/allowed.csv"));
  EXPECT_NE(OB_SUCCESS, check_secure_file_path(".", root + "_sibling/denied.csv"));
  EXPECT_NE(OB_SUCCESS, check_secure_file_path("", root + "/denied.csv"));
  EXPECT_NE(OB_SUCCESS, check_secure_file_path("NULL", root + "/denied.csv"));
  EXPECT_EQ(root, glob_search_root(root + "/*/file?.csv"));
  EXPECT_EQ(".", glob_search_root("*.csv"));
}
#endif

} // namespace file_sql
} // namespace sql
} // namespace oceanbase
