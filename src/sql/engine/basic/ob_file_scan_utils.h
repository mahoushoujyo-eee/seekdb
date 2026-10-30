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

#pragma once

#include <fstream>
#include <cstdint>
#include <string>
#include <vector>

namespace oceanbase
{
namespace sql
{
namespace file_sql
{

enum class InferredType : uint8_t
{
  NULL_TYPE,
  BIGINT,
  DOUBLE,
  BOOLEAN,
  VARCHAR,
  DATE,
  DATETIME,
};

struct CsvOptions
{
  CsvOptions();
  char delimiter_;
  char quote_;
  char escape_;
  bool header_;
  bool empty_as_null_;
  std::string null_string_;
  int64_t max_record_bytes_;
  int64_t max_field_bytes_;
  int64_t max_columns_;
};

struct CsvField
{
  CsvField() : is_null_(false), quoted_(false) {}
  std::string value_;
  bool is_null_;
  bool quoted_;
};

struct CsvRecord
{
  std::vector<CsvField> fields_;
  int64_t line_number_;
};

struct CsvColumn
{
  std::string name_;
  InferredType type_;
  bool nullable_;
};

struct JsonOptions
{
  JsonOptions();
  int64_t max_record_bytes_;
  int64_t max_columns_;
};

struct JsonField
{
  JsonField() : is_null_(false), type_(InferredType::NULL_TYPE) {}
  std::string name_;
  std::string value_;
  bool is_null_;
  InferredType type_;
};

struct JsonRecord
{
  std::vector<JsonField> fields_;
  int64_t line_number_;
};

class CsvReader
{
public:
  CsvReader();
  ~CsvReader();

  int open(const std::string &path, const CsvOptions &options);
  int next(CsvRecord &record, bool &has_record);
  void close();
  int64_t line_number() const { return line_number_; }

private:
  int check_file_identity() const;
  int read_char(char &ch, bool &has_char);
  int finish_field(CsvRecord &record, bool quoted);

  std::string path_;
  CsvOptions options_;
  std::ifstream input_;
  int64_t line_number_;
  int64_t record_start_line_;
  int64_t record_bytes_;
  uint64_t device_;
  uint64_t inode_;
  int64_t file_size_;
  int64_t modified_sec_;
  int64_t modified_nsec_;
  bool inited_;
};

int infer_csv_schema(const std::string &path,
                     const CsvOptions &options,
                     std::vector<CsvColumn> &columns);

class JsonlReader
{
public:
  JsonlReader();
  ~JsonlReader();

  int open(const std::string &path, const JsonOptions &options);
  int next(JsonRecord &record, bool &has_record);
  void close();
  int64_t line_number() const { return line_number_; }

private:
  int check_file_identity() const;

  std::string path_;
  JsonOptions options_;
  std::ifstream input_;
  int64_t line_number_;
  uint64_t device_;
  uint64_t inode_;
  int64_t file_size_;
  int64_t modified_sec_;
  int64_t modified_nsec_;
  bool inited_;
};

int infer_jsonl_schema(const std::string &path,
                       const JsonOptions &options,
                       std::vector<CsvColumn> &columns);

struct ParquetField
{
  ParquetField() : is_null_(false) {}
  std::string value_;
  bool is_null_;
};

struct ParquetRecord
{
  std::vector<ParquetField> fields_;
  int64_t row_number_;
};

class ParquetReader
{
public:
  ParquetReader();
  ~ParquetReader();

  int open(const std::string &path, const std::vector<std::string> &projected_columns);
  int next(ParquetRecord &record, bool &has_record);
  void close();

private:
  void *impl_;
};

int infer_parquet_schema(const std::string &path,
                         std::vector<CsvColumn> &columns);

int discover_files(const std::string &pattern, std::vector<std::string> &files);
std::string glob_search_root(const std::string &pattern);
int check_secure_file_path(const std::string &directory, const std::string &canonical_path);
const char *inferred_type_name(InferredType type);

} // namespace file_sql
} // namespace sql
} // namespace oceanbase
