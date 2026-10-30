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

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/ob_file_scan_utils.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cctype>
#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <sstream>
#include <iomanip>
#include <limits.h>
#include <sys/stat.h>
#if !defined(_WIN32)
#include <glob.h>
#endif

#include "lib/ob_errno.h"
#include "share/ob_errno.h"
#include "lib/utility/ob_macro_utils.h"

namespace oceanbase
{
namespace sql
{
namespace file_sql
{
namespace
{
constexpr int64_t DEFAULT_MAX_RECORD_BYTES = 16 * 1024 * 1024;
constexpr int64_t DEFAULT_MAX_FIELD_BYTES = 4 * 1024 * 1024;
constexpr int64_t DEFAULT_MAX_COLUMNS = 4096;
constexpr int64_t MAX_PARQUET_PAGE_BYTES = 16 * 1024 * 1024;
constexpr int64_t MAX_PARQUET_PAGE_VALUES = 262144;
constexpr int64_t MAX_PARQUET_BUFFER_BYTES = 64 * 1024 * 1024;

struct FileIdentity
{
  uint64_t device_;
  uint64_t inode_;
  int64_t size_;
  int64_t modified_sec_;
  int64_t modified_nsec_;
};

int get_file_identity(const std::string &path, FileIdentity &identity)
{
  int ret = OB_SUCCESS;
  struct stat st;
  if (0 != ::stat(path.c_str(), &st)) {
    ret = OB_FILE_NOT_EXIST;
  } else if (!S_ISREG(st.st_mode)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    identity.device_ = static_cast<uint64_t>(st.st_dev);
    identity.inode_ = static_cast<uint64_t>(st.st_ino);
    identity.size_ = static_cast<int64_t>(st.st_size);
#if defined(__APPLE__)
    identity.modified_sec_ = st.st_mtimespec.tv_sec;
    identity.modified_nsec_ = st.st_mtimespec.tv_nsec;
#elif defined(_WIN32)
    identity.modified_sec_ = st.st_mtime;
    identity.modified_nsec_ = 0;
#else
    identity.modified_sec_ = st.st_mtim.tv_sec;
    identity.modified_nsec_ = st.st_mtim.tv_nsec;
#endif
  }
  return ret;
}

bool is_leap_year(int year)
{
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

bool valid_date_parts(const std::string &value, int &year, int &month, int &day)
{
  if (value.size() < 10 || value[4] != '-' || value[7] != '-') {
    return false;
  }
  for (int i = 0; i < 10; ++i) {
    if (i != 4 && i != 7 && (value[i] < '0' || value[i] > '9')) {
      return false;
    }
  }
  year = std::atoi(value.substr(0, 4).c_str());
  month = std::atoi(value.substr(5, 2).c_str());
  day = std::atoi(value.substr(8, 2).c_str());
  if (year < 1 || month < 1 || month > 12 || day < 1) {
    return false;
  }
  static const int days_per_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const int max_day = days_per_month[month - 1] + (month == 2 && is_leap_year(year) ? 1 : 0);
  return day <= max_day;
}

bool is_valid_date(const std::string &value)
{
  int year = 0;
  int month = 0;
  int day = 0;
  return value.size() == 10 && valid_date_parts(value, year, month, day);
}

bool is_valid_datetime(const std::string &value)
{
  int year = 0;
  int month = 0;
  int day = 0;
  if (value.size() != 19 || (value[10] != ' ' && value[10] != 'T')
      || value[13] != ':' || value[16] != ':'
      || !valid_date_parts(value, year, month, day)) {
    return false;
  }
  for (size_t i = 11; i < value.size(); ++i) {
    if (i != 13 && i != 16 && (value[i] < '0' || value[i] > '9')) {
      return false;
    }
  }
  const int hour = std::atoi(value.substr(11, 2).c_str());
  const int minute = std::atoi(value.substr(14, 2).c_str());
  const int second = std::atoi(value.substr(17, 2).c_str());
  return hour < 24 && minute < 60 && second < 60;
}

bool parse_integer(const std::string &value)
{
  if (value.empty()) {
    return false;
  }
  errno = 0;
  char *end = nullptr;
  (void)std::strtoll(value.c_str(), &end, 10);
  return errno != ERANGE && end == value.c_str() + value.size();
}

bool is_integer_lexeme(const std::string &value)
{
  size_t pos = !value.empty() && (value[0] == '-' || value[0] == '+') ? 1 : 0;
  if (pos == value.size()) return false;
  for (; pos < value.size(); ++pos) {
    if (value[pos] < '0' || value[pos] > '9') return false;
  }
  return true;
}

bool parse_double(const std::string &value)
{
  if (value.empty()) {
    return false;
  }
  errno = 0;
  char *end = nullptr;
  const double parsed = std::strtod(value.c_str(), &end);
  return errno != ERANGE && std::isfinite(parsed) && end == value.c_str() + value.size();
}

InferredType infer_value_type(const std::string &value)
{
  InferredType type = InferredType::VARCHAR;
  if (value == "true" || value == "false" || value == "TRUE" || value == "FALSE") {
    type = InferredType::BOOLEAN;
  } else if (parse_integer(value)) {
    type = InferredType::BIGINT;
  } else if (is_integer_lexeme(value)) {
    type = InferredType::VARCHAR;
  } else if (parse_double(value)) {
    type = InferredType::DOUBLE;
  } else if (is_valid_datetime(value)) {
    type = InferredType::DATETIME;
  } else if (is_valid_date(value)) {
    type = InferredType::DATE;
  }
  return type;
}

bool is_json_space(char ch)
{
  return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

bool is_json_digit(char ch)
{
  return ch >= '0' && ch <= '9';
}

bool is_valid_json_number(const std::string &value)
{
  size_t pos = 0;
  if (pos < value.size() && value[pos] == '-') ++pos;
  if (pos >= value.size()) return false;
  if (value[pos] == '0') {
    ++pos;
    if (pos < value.size() && is_json_digit(value[pos])) return false;
  } else if (value[pos] >= '1' && value[pos] <= '9') {
    while (pos < value.size() && is_json_digit(value[pos])) ++pos;
  } else {
    return false;
  }
  if (pos < value.size() && value[pos] == '.') {
    ++pos;
    const size_t fraction_start = pos;
    while (pos < value.size() && is_json_digit(value[pos])) ++pos;
    if (fraction_start == pos) return false;
  }
  if (pos < value.size() && (value[pos] == 'e' || value[pos] == 'E')) {
    ++pos;
    if (pos < value.size() && (value[pos] == '+' || value[pos] == '-')) ++pos;
    const size_t exponent_start = pos;
    while (pos < value.size() && is_json_digit(value[pos])) ++pos;
    if (exponent_start == pos) return false;
  }
  return pos == value.size();
}

bool valid_utf8(const std::string &value)
{
  size_t pos = 0;
  while (pos < value.size()) {
    const unsigned char lead = static_cast<unsigned char>(value[pos]);
    if (lead <= 0x7f) {
      ++pos;
    } else {
      size_t width = 0;
      uint32_t code = 0;
      if (lead >= 0xc2 && lead <= 0xdf) {
        width = 2;
        code = lead & 0x1f;
      } else if (lead >= 0xe0 && lead <= 0xef) {
        width = 3;
        code = lead & 0x0f;
      } else if (lead >= 0xf0 && lead <= 0xf4) {
        width = 4;
        code = lead & 0x07;
      } else {
        return false;
      }
      if (pos + width > value.size()) return false;
      for (size_t i = 1; i < width; ++i) {
        const unsigned char next = static_cast<unsigned char>(value[pos + i]);
        if ((next & 0xc0) != 0x80) return false;
        code = (code << 6) | (next & 0x3f);
      }
      if ((width == 2 && code < 0x80)
          || (width == 3 && code < 0x800)
          || (width == 4 && code < 0x10000)
          || (code >= 0xd800 && code <= 0xdfff)
          || code > 0x10ffff) {
        return false;
      }
      pos += width;
    }
  }
  return true;
}

int parse_json_string(const std::string &line, size_t &pos, std::string &value)
{
  int ret = OB_SUCCESS;
  value.clear();
  if (pos >= line.size() || line[pos] != '"') {
    ret = OB_INVALID_DATA;
  } else {
    ++pos;
    while (OB_SUCC(ret) && pos < line.size()) {
      const char ch = line[pos++];
      if (ch == '"') {
        return ret;
      } else if (static_cast<unsigned char>(ch) < 0x20) {
        ret = OB_INVALID_DATA;
      } else if (ch != '\\') {
        value.push_back(ch);
      } else if (pos >= line.size()) {
        ret = OB_INVALID_DATA;
      } else {
        const char escaped = line[pos++];
        switch (escaped) {
          case '"': value.push_back('"'); break;
          case '\\': value.push_back('\\'); break;
          case '/': value.push_back('/'); break;
          case 'b': value.push_back('\b'); break;
          case 'f': value.push_back('\f'); break;
          case 'n': value.push_back('\n'); break;
          case 'r': value.push_back('\r'); break;
          case 't': value.push_back('\t'); break;
          case 'u': {
            if (pos + 4 > line.size()) {
              ret = OB_INVALID_DATA;
            } else {
              unsigned code = 0;
              for (int i = 0; OB_SUCC(ret) && i < 4; ++i) {
                const char digit = line[pos++];
                code <<= 4;
                if (digit >= '0' && digit <= '9') code += static_cast<unsigned>(digit - '0');
                else if (digit >= 'a' && digit <= 'f') code += static_cast<unsigned>(digit - 'a' + 10);
                else if (digit >= 'A' && digit <= 'F') code += static_cast<unsigned>(digit - 'A' + 10);
                else ret = OB_INVALID_DATA;
              }
              if (OB_SUCC(ret) && code >= 0xd800 && code <= 0xdbff) {
                if (pos + 6 > line.size() || line[pos] != '\\' || line[pos + 1] != 'u') {
                  ret = OB_INVALID_DATA;
                } else {
                  pos += 2;
                  unsigned low = 0;
                  for (int i = 0; OB_SUCC(ret) && i < 4; ++i) {
                    const char digit = line[pos++];
                    low <<= 4;
                    if (digit >= '0' && digit <= '9') low += static_cast<unsigned>(digit - '0');
                    else if (digit >= 'a' && digit <= 'f') low += static_cast<unsigned>(digit - 'a' + 10);
                    else if (digit >= 'A' && digit <= 'F') low += static_cast<unsigned>(digit - 'A' + 10);
                    else ret = OB_INVALID_DATA;
                  }
                  if (OB_SUCC(ret) && (low < 0xdc00 || low > 0xdfff)) ret = OB_INVALID_DATA;
                  if (OB_SUCC(ret)) code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                }
              } else if (OB_SUCC(ret) && code >= 0xdc00 && code <= 0xdfff) {
                ret = OB_INVALID_DATA;
              }
              if (OB_SUCC(ret)) {
                if (code <= 0x7f) {
                  value.push_back(static_cast<char>(code));
                } else if (code <= 0x7ff) {
                  value.push_back(static_cast<char>(0xc0 | (code >> 6)));
                  value.push_back(static_cast<char>(0x80 | (code & 0x3f)));
                } else if (code <= 0xffff) {
                  value.push_back(static_cast<char>(0xe0 | (code >> 12)));
                  value.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
                  value.push_back(static_cast<char>(0x80 | (code & 0x3f)));
                } else {
                  value.push_back(static_cast<char>(0xf0 | (code >> 18)));
                  value.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
                  value.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
                  value.push_back(static_cast<char>(0x80 | (code & 0x3f)));
                }
              }
            }
            break;
          }
          default: ret = OB_INVALID_DATA; break;
        }
      }
    }
    if (OB_SUCC(ret)) {
      ret = OB_INVALID_DATA;
    }
  }
  return ret;
}

int parse_json_scalar(const std::string &line,
                      size_t &pos,
                      std::string &value,
                      InferredType &type,
                      bool &is_null)
{
  int ret = OB_SUCCESS;
  value.clear();
  is_null = false;
  while (pos < line.size() && is_json_space(line[pos])) ++pos;
  if (pos >= line.size()) {
    ret = OB_INVALID_DATA;
  } else if (line[pos] == '"') {
    ret = parse_json_string(line, pos, value);
    type = InferredType::VARCHAR;
  } else if (line[pos] == '{' || line[pos] == '[') {
    ret = OB_NOT_SUPPORTED;
  } else {
    const size_t start = pos;
    while (pos < line.size() && line[pos] != ',' && line[pos] != '}' && !is_json_space(line[pos])) ++pos;
    value.assign(line, start, pos - start);
    if (value == "null") {
      is_null = true;
      type = InferredType::NULL_TYPE;
    } else if (value == "true" || value == "false") {
      type = InferredType::BOOLEAN;
    } else if (!is_valid_json_number(value)) {
      ret = OB_INVALID_DATA;
    } else if (parse_integer(value)) {
      type = InferredType::BIGINT;
    } else if (is_integer_lexeme(value)) {
      type = InferredType::VARCHAR;
    } else if (parse_double(value)) {
      type = InferredType::DOUBLE;
    } else {
      ret = OB_INVALID_DATA;
    }
  }
  while (pos < line.size() && is_json_space(line[pos])) ++pos;
  return ret;
}

int parse_json_object(const std::string &line,
                      JsonRecord &record,
                      const int64_t line_number,
                      const int64_t max_columns)
{
  int ret = OB_SUCCESS;
  record.fields_.clear();
  record.line_number_ = line_number;
  size_t pos = 0;
  while (pos < line.size() && is_json_space(line[pos])) ++pos;
  if (pos >= line.size()) {
    return OB_SUCCESS;
  } else if (line[pos++] != '{') {
    return OB_INVALID_DATA;
  }
  while (OB_SUCC(ret)) {
    while (pos < line.size() && is_json_space(line[pos])) ++pos;
    if (pos < line.size() && line[pos] == '}') {
      ++pos;
      break;
    }
    std::string name;
    if (OB_FAIL(parse_json_string(line, pos, name))) {
    } else if (name.empty()) {
      ret = OB_INVALID_DATA;
    } else {
      for (const JsonField &field : record.fields_) {
        if (field.name_ == name) {
          ret = OB_INVALID_DATA;
          break;
        }
      }
    }
    while (OB_SUCC(ret) && pos < line.size() && is_json_space(line[pos])) ++pos;
    if (OB_SUCC(ret) && (pos >= line.size() || line[pos++] != ':')) ret = OB_INVALID_DATA;
    if (OB_SUCC(ret)) {
      JsonField field;
      field.name_ = std::move(name);
      if (OB_FAIL(parse_json_scalar(line, pos, field.value_, field.type_, field.is_null_))) {
      } else if (static_cast<int64_t>(record.fields_.size()) >= max_columns) {
        ret = OB_SIZE_OVERFLOW;
      } else {
        record.fields_.push_back(std::move(field));
      }
    }
    while (OB_SUCC(ret) && pos < line.size() && is_json_space(line[pos])) ++pos;
    if (OB_SUCC(ret)) {
      if (pos >= line.size()) ret = OB_INVALID_DATA;
      else if (line[pos] == ',') {
        ++pos;
        size_t next_pos = pos;
        while (next_pos < line.size() && is_json_space(line[next_pos])) ++next_pos;
        if (next_pos >= line.size() || line[next_pos] == '}') ret = OB_INVALID_DATA;
      }
      else if (line[pos] == '}') { ++pos; break; }
      else ret = OB_INVALID_DATA;
    }
  }
  while (OB_SUCC(ret) && pos < line.size() && is_json_space(line[pos])) ++pos;
  if (OB_SUCC(ret) && pos != line.size()) ret = OB_INVALID_DATA;
  return ret;
}

InferredType widen_type(InferredType left, InferredType right)
{
  if (left == InferredType::NULL_TYPE) {
    return right;
  } else if (right == InferredType::NULL_TYPE || left == right) {
    return left;
  } else if ((left == InferredType::BIGINT && right == InferredType::DOUBLE)
             || (left == InferredType::DOUBLE && right == InferredType::BIGINT)) {
    return InferredType::DOUBLE;
  } else if ((left == InferredType::DATE && right == InferredType::DATETIME)
             || (left == InferredType::DATETIME && right == InferredType::DATE)) {
    return InferredType::DATETIME;
  }
  return InferredType::VARCHAR;
}

bool name_exists(const std::vector<CsvColumn> &columns, const std::string &name)
{
  for (const CsvColumn &column : columns) {
    if (column.name_.size() == name.size()) {
      bool equal = true;
      for (size_t i = 0; equal && i < name.size(); ++i) {
        equal = std::tolower(static_cast<unsigned char>(column.name_[i]))
                == std::tolower(static_cast<unsigned char>(name[i]));
      }
      if (equal) {
        return true;
      }
    }
  }
  return false;
}

std::string unique_name(const std::vector<CsvColumn> &columns, const std::string &base)
{
  std::string candidate = base.empty() ? "column" : base;
  if (!name_exists(columns, candidate)) {
    return candidate;
  }
  for (int64_t suffix = 2; ; ++suffix) {
    candidate = (base.empty() ? "column" : base) + "__" + std::to_string(suffix);
    if (!name_exists(columns, candidate)) {
      break;
    }
  }
  return candidate;
}

} // namespace

CsvOptions::CsvOptions()
  : delimiter_(',')
  , quote_('"')
  , escape_('"')
  , header_(true)
  , empty_as_null_(true)
  , null_string_("NULL")
  , max_record_bytes_(DEFAULT_MAX_RECORD_BYTES)
  , max_field_bytes_(DEFAULT_MAX_FIELD_BYTES)
  , max_columns_(DEFAULT_MAX_COLUMNS)
{
}

CsvReader::CsvReader()
  : line_number_(1)
  , record_start_line_(1)
  , record_bytes_(0)
  , device_(0)
  , inode_(0)
  , file_size_(0)
  , modified_sec_(0)
  , modified_nsec_(0)
  , inited_(false)
{
}

CsvReader::~CsvReader()
{
  close();
}

int CsvReader::open(const std::string &path, const CsvOptions &options)
{
  int ret = OB_SUCCESS;
  close();
  if (path.empty() || options.delimiter_ == '\0' || options.quote_ == '\0'
      || options.max_record_bytes_ <= 0 || options.max_field_bytes_ <= 0
      || options.max_columns_ <= 0 || options.max_field_bytes_ > options.max_record_bytes_) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    FileIdentity identity;
    if (OB_FAIL(get_file_identity(path, identity))) {
    } else {
      input_.open(path, std::ios::in | std::ios::binary);
      if (!input_.is_open()) {
        ret = OB_FILE_NOT_EXIST;
      } else if (OB_FAIL(get_file_identity(path, identity))) {
        close();
      } else {
        path_ = path;
        options_ = options;
        line_number_ = 1;
        record_start_line_ = 1;
        record_bytes_ = 0;
        device_ = identity.device_;
        inode_ = identity.inode_;
        file_size_ = identity.size_;
        modified_sec_ = identity.modified_sec_;
        modified_nsec_ = identity.modified_nsec_;
        inited_ = true;
      }
    }
  }
  return ret;
}

void CsvReader::close()
{
  if (input_.is_open()) {
    input_.close();
  }
  path_.clear();
  inited_ = false;
}

int CsvReader::check_file_identity() const
{
  int ret = OB_SUCCESS;
  FileIdentity identity;
  if (!inited_) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(get_file_identity(path_, identity))) {
  } else if (identity.device_ != device_ || identity.inode_ != inode_
             || identity.size_ != file_size_ || identity.modified_sec_ != modified_sec_
             || identity.modified_nsec_ != modified_nsec_) {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

int CsvReader::read_char(char &ch, bool &has_char)
{
  int ret = OB_SUCCESS;
  if (!input_.get(ch)) {
    if (input_.eof()) {
      has_char = false;
      input_.clear(input_.rdstate() & ~std::ios::failbit);
    } else {
      ret = OB_IO_ERROR;
    }
  } else {
    has_char = true;
  }
  return ret;
}

int CsvReader::finish_field(CsvRecord &record, bool quoted)
{
  int ret = OB_SUCCESS;
  if (static_cast<int64_t>(record.fields_.size()) >= options_.max_columns_) {
    ret = OB_SIZE_OVERFLOW;
  } else {
    record.fields_.emplace_back();
    record.fields_.back().quoted_ = quoted;
  }
  return ret;
}

int CsvReader::next(CsvRecord &record, bool &has_record)
{
  int ret = OB_SUCCESS;
  has_record = false;
  record.fields_.clear();
  record_bytes_ = 0;
  record_start_line_ = line_number_;
  if (!inited_) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(check_file_identity())) {
  } else {
    bool in_quotes = false;
    bool field_quoted = false;
    bool field_started = false;
    bool closed_quote = false;
    bool saw_anything = false;
    std::string field_value;
    while (OB_SUCC(ret) && !has_record) {
      char ch = '\0';
      bool has_char = false;
      if (OB_FAIL(read_char(ch, has_char))) {
      } else if (!has_char) {
        if (in_quotes) {
          ret = OB_INVALID_DATA;
        } else if (!saw_anything && record.fields_.empty() && field_value.empty()) {
          break;
        } else if (OB_FAIL(finish_field(record, field_quoted))) {
        } else {
          CsvField &last = record.fields_.back();
          last.value_ = std::move(field_value);
          last.is_null_ = !last.quoted_ && (last.value_ == options_.null_string_
                              || (options_.empty_as_null_ && last.value_.empty()));
          has_record = true;
        }
      } else {
        saw_anything = true;
        ++record_bytes_;
        if (record_bytes_ > options_.max_record_bytes_) {
          ret = OB_SIZE_OVERFLOW;
        } else if (in_quotes) {
          if (options_.escape_ == options_.quote_ && ch == options_.quote_) {
            char next_ch = '\0';
            bool has_next = false;
            if (OB_FAIL(read_char(next_ch, has_next))) {
            } else if (has_next && next_ch == options_.quote_) {
              ++record_bytes_;
              field_value.push_back(options_.quote_);
            } else {
              in_quotes = false;
              closed_quote = true;
              if (has_next) {
                input_.putback(next_ch);
              }
            }
          } else if (options_.escape_ != options_.quote_ && ch == options_.escape_) {
            char escaped = '\0';
            bool has_escaped = false;
            if (OB_FAIL(read_char(escaped, has_escaped))) {
            } else if (!has_escaped) {
              ret = OB_INVALID_DATA;
            } else {
              ++record_bytes_;
              switch (escaped) {
                case 'n': field_value.push_back('\n'); break;
                case 'r': field_value.push_back('\r'); break;
                case 't': field_value.push_back('\t'); break;
                case '0': field_value.push_back('\0'); break;
                default: field_value.push_back(escaped); break;
              }
            }
          } else if (ch == options_.quote_) {
            in_quotes = false;
            closed_quote = true;
          } else if (ch == '\r' && input_.peek() == '\n') {
            input_.get();
            ++record_bytes_;
            if (record_bytes_ > options_.max_record_bytes_) {
              ret = OB_SIZE_OVERFLOW;
            } else {
              field_value.push_back('\n');
              ++line_number_;
            }
          } else {
            if (ch == '\n') {
              ++line_number_;
            }
            field_value.push_back(ch);
          }
        } else if (!field_started && field_value.empty() && ch == options_.quote_) {
          in_quotes = true;
          field_quoted = true;
          field_started = true;
        } else if (ch == options_.delimiter_) {
          if (OB_FAIL(finish_field(record, field_quoted))) {
          } else {
            CsvField &last = record.fields_.back();
            last.value_ = std::move(field_value);
            last.is_null_ = !last.quoted_ && (last.value_ == options_.null_string_
                                || (options_.empty_as_null_ && last.value_.empty()));
            field_value.clear();
            field_quoted = false;
            field_started = false;
            closed_quote = false;
          }
        } else if (ch == '\n' || ch == '\r') {
          if (ch == '\r' && input_.peek() == '\n') {
            input_.get();
            ++record_bytes_;
          }
          ++line_number_;
          if (OB_FAIL(finish_field(record, field_quoted))) {
          } else {
            CsvField &last = record.fields_.back();
            last.value_ = std::move(field_value);
            last.is_null_ = !last.quoted_ && (last.value_ == options_.null_string_
                                || (options_.empty_as_null_ && last.value_.empty()));
            has_record = true;
          }
        } else if (closed_quote) {
          ret = OB_INVALID_DATA;
        } else if (ch == options_.quote_) {
          ret = OB_INVALID_DATA;
        } else if (options_.escape_ != options_.quote_ && ch == options_.escape_) {
          char escaped = '\0';
          bool has_escaped = false;
          if (OB_FAIL(read_char(escaped, has_escaped))) {
          } else if (!has_escaped) {
            ret = OB_INVALID_DATA;
          } else {
            ++record_bytes_;
            field_value.push_back(escaped);
            field_started = true;
          }
        } else {
          field_value.push_back(ch);
          field_started = true;
        }
        if (OB_SUCC(ret) && static_cast<int64_t>(field_value.size()) > options_.max_field_bytes_) {
          ret = OB_SIZE_OVERFLOW;
        }
      }
    }
    for (size_t i = 0; OB_SUCC(ret) && has_record && i < record.fields_.size(); ++i) {
      if (!valid_utf8(record.fields_[i].value_)) ret = OB_INVALID_DATA;
    }
    if (OB_SUCC(ret) && has_record) {
      record.line_number_ = record_start_line_;
      ret = check_file_identity();
    }
  }
  return ret;
}

int infer_csv_schema(const std::string &path,
                     const CsvOptions &options,
                     std::vector<CsvColumn> &columns)
{
  int ret = OB_SUCCESS;
  columns.clear();
  CsvReader reader;
  if (OB_FAIL(reader.open(path, options))) {
  } else {
    CsvRecord record;
    bool has_record = false;
    if (OB_FAIL(reader.next(record, has_record))) {
    } else if (has_record && options.header_) {
      for (const CsvField &field : record.fields_) {
        CsvColumn column;
        column.name_ = unique_name(columns, field.value_);
        column.type_ = InferredType::NULL_TYPE;
        column.nullable_ = false;
        columns.push_back(std::move(column));
      }
    } else if (has_record) {
      columns.resize(record.fields_.size());
      for (size_t i = 0; i < columns.size(); ++i) {
        columns[i].name_ = "column" + std::to_string(i + 1);
        columns[i].type_ = InferredType::NULL_TYPE;
        columns[i].nullable_ = false;
      }
    }
    bool process_first_record = has_record && !options.header_;
    while (OB_SUCC(ret)) {
      if (!process_first_record && OB_FAIL(reader.next(record, has_record))) {
      } else if (!process_first_record && !has_record) {
        break;
      } else if (record.fields_.size() != columns.size()) {
        ret = OB_INVALID_DATA;
      } else {
        for (size_t i = 0; OB_SUCC(ret) && i < columns.size(); ++i) {
          CsvColumn &column = columns[i];
          const CsvField &field = record.fields_[i];
          column.nullable_ = column.nullable_ || field.is_null_;
          if (!field.is_null_) {
            column.type_ = widen_type(column.type_, infer_value_type(field.value_));
          }
        }
        process_first_record = false;
      }
    }
    for (CsvColumn &column : columns) {
      column.nullable_ = column.nullable_ || column.type_ == InferredType::NULL_TYPE;
    }
  }
  return ret;
}

JsonOptions::JsonOptions()
  : max_record_bytes_(16 * 1024 * 1024)
  , max_columns_(4096)
{
}

JsonlReader::JsonlReader()
  : line_number_(0)
  , device_(0)
  , inode_(0)
  , file_size_(0)
  , modified_sec_(0)
  , modified_nsec_(0)
  , inited_(false)
{
}

JsonlReader::~JsonlReader()
{
  close();
}

int JsonlReader::open(const std::string &path, const JsonOptions &options)
{
  int ret = OB_SUCCESS;
  close();
  if (path.empty() || options.max_record_bytes_ <= 0 || options.max_columns_ <= 0) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    FileIdentity identity;
    if (OB_FAIL(get_file_identity(path, identity))) {
    } else {
      input_.open(path, std::ios::in | std::ios::binary);
      if (!input_.is_open()) {
        ret = OB_FILE_NOT_EXIST;
      } else {
        path_ = path;
        options_ = options;
        line_number_ = 0;
        device_ = identity.device_;
        inode_ = identity.inode_;
        file_size_ = identity.size_;
        modified_sec_ = identity.modified_sec_;
        modified_nsec_ = identity.modified_nsec_;
        inited_ = true;
      }
    }
  }
  return ret;
}

void JsonlReader::close()
{
  if (input_.is_open()) input_.close();
  path_.clear();
  inited_ = false;
}

int JsonlReader::check_file_identity() const
{
  int ret = OB_SUCCESS;
  FileIdentity identity;
  if (!inited_) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(get_file_identity(path_, identity))) {
  } else if (identity.device_ != device_ || identity.inode_ != inode_
             || identity.size_ != file_size_ || identity.modified_sec_ != modified_sec_
             || identity.modified_nsec_ != modified_nsec_) {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

int JsonlReader::next(JsonRecord &record, bool &has_record)
{
  int ret = OB_SUCCESS;
  has_record = false;
  record.fields_.clear();
  if (!inited_) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(check_file_identity())) {
  } else {
    while (OB_SUCC(ret) && !has_record) {
      std::string line;
      bool has_byte = false;
      bool line_done = false;
      char ch = '\0';
      while (OB_SUCC(ret) && !line_done && input_.get(ch)) {
        has_byte = true;
        if (ch == '\n') {
          line_done = true;
        } else if (static_cast<int64_t>(line.size()) >= options_.max_record_bytes_) {
          ret = OB_SIZE_OVERFLOW;
        } else {
          line.push_back(ch);
        }
      }
      if (OB_SUCC(ret) && input_.bad()) ret = OB_IO_ERROR;
      if (OB_SUCC(ret) && has_byte) {
        ++line_number_;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.find_first_not_of(" \t") == std::string::npos) {
        } else if (!valid_utf8(line)) {
          ret = OB_INVALID_DATA;
          record.line_number_ = line_number_;
        } else if (OB_FAIL(parse_json_object(line, record, line_number_, options_.max_columns_))) {
          record.line_number_ = line_number_;
        } else {
          has_record = true;
        }
      } else if (OB_SUCC(ret) && !has_byte) {
        break;
      }
    }
    if (OB_SUCC(ret)) ret = check_file_identity();
  }
  return ret;
}

int infer_jsonl_schema(const std::string &path,
                       const JsonOptions &options,
                       std::vector<CsvColumn> &columns)
{
  int ret = OB_SUCCESS;
  columns.clear();
  JsonlReader reader;
  if (OB_FAIL(reader.open(path, options))) {
  } else {
    std::map<std::string, size_t> indexes;
    JsonRecord record;
    bool has_record = false;
    int64_t records_seen = 0;
    while (OB_SUCC(ret)) {
      if (OB_FAIL(reader.next(record, has_record))) {
      } else if (!has_record) {
        break;
      } else {
        std::vector<bool> present(columns.size(), false);
        for (const JsonField &field : record.fields_) {
          std::map<std::string, size_t>::iterator iter = indexes.find(field.name_);
          size_t index = 0;
          if (iter == indexes.end()) {
            index = columns.size();
            if (static_cast<int64_t>(index) >= options.max_columns_) {
              ret = OB_SIZE_OVERFLOW;
              break;
            }
            indexes[field.name_] = index;
            CsvColumn column;
            column.name_ = field.name_;
            column.type_ = InferredType::NULL_TYPE;
            column.nullable_ = records_seen > 0 || field.is_null_;
            columns.push_back(std::move(column));
            present.push_back(false);
          } else {
            index = iter->second;
          }
          if (OB_SUCC(ret)) {
            present[index] = true;
            CsvColumn &column = columns[index];
            column.nullable_ = column.nullable_ || field.is_null_;
            if (!field.is_null_) column.type_ = widen_type(column.type_, field.type_);
          }
        }
        if (OB_SUCC(ret)) {
          for (size_t i = 0; i < columns.size(); ++i) {
            columns[i].nullable_ = columns[i].nullable_ || !present[i];
          }
          ++records_seen;
        }
      }
    }
    reader.close();
  }
  return ret;
}

std::string glob_search_root(const std::string &pattern)
{
  const size_t wildcard = pattern.find_first_of("*?[");
  const size_t slash = pattern.rfind('/', wildcard == std::string::npos ? pattern.size() : wildcard);
  return slash == std::string::npos ? "." : slash == 0 ? "/" : pattern.substr(0, slash);
}

int check_secure_file_path(const std::string &directory, const std::string &canonical_path)
{
  int ret = OB_SUCCESS;
  char resolved[PATH_MAX] = {0};
  struct stat st;
  // File SQL deliberately does not inherit ordinary OUTFILE's server log allowlist.
  if (directory.empty() || directory.find('\0') != std::string::npos
      || canonical_path.find('\0') != std::string::npos) {
    ret = OB_ERR_NO_PRIVILEGE;
#if defined(_WIN32)
  } else if (::_fullpath(resolved, directory.c_str(), sizeof(resolved)) == nullptr) {
#else
  } else if (::realpath(directory.c_str(), resolved) == nullptr) {
#endif
    ret = OB_ERR_NO_PRIVILEGE;
  } else if (::stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode)) {
    ret = OB_ERR_NO_PRIVILEGE;
  } else {
    const std::string root(resolved);
    if (canonical_path.compare(0, root.size(), root) != 0
        || (root != "/" && canonical_path.size() > root.size() && canonical_path[root.size()] != '/')) {
      ret = OB_ERR_NO_PRIVILEGE;
    }
  }
  return ret;
}

int discover_files(const std::string &pattern, std::vector<std::string> &files)
{
  int ret = OB_SUCCESS;
  files.clear();
  if (pattern.empty()) {
    ret = OB_INVALID_ARGUMENT;
#if defined(_WIN32)
  } else {
    ret = OB_NOT_SUPPORTED;
#else
  } else {
    glob_t result;
    std::memset(&result, 0, sizeof(result));
    const int glob_ret = ::glob(pattern.c_str(), GLOB_NOSORT, nullptr, &result);
    if (glob_ret == GLOB_NOMATCH) {
      // An empty discovery result is valid and is distinct from a bad pattern.
    } else if (glob_ret != 0) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      for (size_t i = 0; OB_SUCC(ret) && i < result.gl_pathc; ++i) {
        struct stat st;
        if (0 != ::stat(result.gl_pathv[i], &st)) {
          ret = OB_FILE_NOT_EXIST;
        } else if (S_ISREG(st.st_mode)) {
          if (files.size() >= 4096) ret = OB_SIZE_OVERFLOW;
          else files.push_back(result.gl_pathv[i]);
        }
      }
      std::sort(files.begin(), files.end());
    }
    ::globfree(&result);
#endif
  }
  return ret;
}

namespace
{
enum class CompactKind : uint8_t { STOP, BOOL, INTEGER, REAL, BINARY, LIST, STRUCT };

struct CompactValue
{
  CompactValue() : kind_(CompactKind::STOP), integer_(0), boolean_(false), real_(0) {}
  CompactKind kind_;
  int64_t integer_;
  bool boolean_;
  double real_;
  std::string binary_;
  std::vector<CompactValue> list_;
  std::map<int32_t, CompactValue> fields_;
};

class CompactReader
{
public:
  explicit CompactReader(std::istream &input) : input_(input), consumed_(0) {}
  int64_t consumed() const { return consumed_; }

  int read_struct(CompactValue &value, int depth = 0)
  {
    int ret = OB_SUCCESS;
    value.kind_ = CompactKind::STRUCT;
    int32_t last_field = 0;
    uint8_t header = 0;
    while (OB_SUCC(ret)) {
      if (OB_FAIL(read_byte(header))) {
      } else if ((header & 0x0f) == 0) {
        break;
      } else if (value.fields_.size() >= 65536) {
        ret = OB_SIZE_OVERFLOW;
      } else {
        const uint8_t wire_type = header & 0x0f;
        const uint8_t delta = header >> 4;
        int64_t field_id = 0;
        if (delta == 0) {
          if (OB_FAIL(read_zigzag(field_id))) {
          }
        } else {
          field_id = last_field + delta;
        }
        if (OB_SUCC(ret) && (field_id <= 0 || field_id > INT32_MAX
            || value.fields_.find(static_cast<int32_t>(field_id)) != value.fields_.end())) {
          ret = OB_INVALID_DATA;
        } else if (OB_SUCC(ret)) {
          last_field = static_cast<int32_t>(field_id);
          CompactValue child;
          if (OB_FAIL(read_value(wire_type, child, depth + 1))) {
          } else {
            value.fields_.emplace(last_field, std::move(child));
          }
        }
      }
    }
    return ret;
  }

private:
  int read_byte(uint8_t &value)
  {
    int ret = OB_SUCCESS;
    char byte = 0;
    if (!input_.get(byte)) {
      ret = OB_INVALID_DATA;
    } else {
      value = static_cast<uint8_t>(byte);
      ++consumed_;
    }
    return ret;
  }

  int read_exact(char *buffer, size_t length)
  {
    int ret = OB_SUCCESS;
    if (length > 0) {
      input_.read(buffer, static_cast<std::streamsize>(length));
      if (input_.gcount() != static_cast<std::streamsize>(length)) {
        ret = OB_INVALID_DATA;
      } else {
        consumed_ += static_cast<int64_t>(length);
      }
    }
    return ret;
  }

  int read_varint(uint64_t &value)
  {
    int ret = OB_SUCCESS;
    value = 0;
    bool done = false;
    for (int shift = 0; OB_SUCC(ret) && !done; shift += 7) {
      uint8_t byte = 0;
      if (shift >= 64) {
        ret = OB_INVALID_DATA;
      } else if (OB_FAIL(read_byte(byte))) {
      } else if (shift == 63 && (byte & 0xfe) != 0) {
        ret = OB_INVALID_DATA;
      } else {
        value |= static_cast<uint64_t>(byte & 0x7f) << shift;
        done = (byte & 0x80) == 0;
      }
    }
    return ret;
  }

  int read_zigzag(int64_t &value)
  {
    uint64_t encoded = 0;
    int ret = read_varint(encoded);
    if (OB_SUCC(ret)) {
      value = static_cast<int64_t>((encoded >> 1) ^ (0ULL - (encoded & 1)));
    }
    return ret;
  }

  int read_value(const uint8_t wire_type, CompactValue &value, const int depth)
  {
    int ret = OB_SUCCESS;
    if (depth > 64) {
      ret = OB_SIZE_OVERFLOW;
    } else if (wire_type == 1 || wire_type == 2) {
      value.kind_ = CompactKind::BOOL;
      value.boolean_ = wire_type == 1;
    } else if (wire_type == 3) {
      uint8_t byte = 0;
      if (OB_FAIL(read_byte(byte))) {
      } else {
        value.kind_ = CompactKind::INTEGER;
        value.integer_ = static_cast<int8_t>(byte);
      }
    } else if (wire_type >= 4 && wire_type <= 6) {
      value.kind_ = CompactKind::INTEGER;
      ret = read_zigzag(value.integer_);
    } else if (wire_type == 7) {
      uint8_t bytes[8] = {0};
      if (OB_FAIL(read_exact(reinterpret_cast<char *>(bytes), sizeof(bytes)))) {
      } else {
        uint64_t raw = 0;
        for (int i = 7; i >= 0; --i) raw = (raw << 8) | bytes[i];
        value.kind_ = CompactKind::REAL;
        memcpy(&value.real_, &raw, sizeof(raw));
      }
    } else if (wire_type == 8) {
      uint64_t length = 0;
      if (OB_FAIL(read_varint(length))) {
      } else if (length > 64 * 1024 * 1024 || length > static_cast<uint64_t>(INT64_MAX)) {
        ret = OB_SIZE_OVERFLOW;
      } else {
        value.kind_ = CompactKind::BINARY;
        value.binary_.resize(static_cast<size_t>(length));
        ret = read_exact(value.binary_.data(), value.binary_.size());
      }
    } else if (wire_type == 9 || wire_type == 10) {
      uint8_t header = 0;
      if (OB_FAIL(read_byte(header))) {
      } else {
        uint64_t count = header >> 4;
        const uint8_t element_type = header & 0x0f;
        if (count == 15 && OB_FAIL(read_varint(count))) {
        } else if (count > 1000000) {
          ret = OB_SIZE_OVERFLOW;
        } else {
          value.kind_ = CompactKind::LIST;
          value.list_.reserve(static_cast<size_t>(count));
          for (uint64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
            CompactValue child;
            uint8_t item_type = element_type;
            if (element_type == 1 || element_type == 2) {
              uint8_t bool_wire = 0;
              if (OB_FAIL(read_byte(bool_wire))) {
              } else if (bool_wire != 1 && bool_wire != 2) {
                ret = OB_INVALID_DATA;
              } else {
                item_type = bool_wire;
              }
            }
            if (OB_SUCC(ret) && OB_FAIL(read_value(item_type, child, depth + 1))) {
            } else if (OB_SUCC(ret)) {
              value.list_.push_back(std::move(child));
            }
          }
        }
      }
    } else if (wire_type == 11) {
      uint64_t count = 0;
      if (OB_FAIL(read_varint(count))) {
      } else if (count > 1000000) {
        ret = OB_SIZE_OVERFLOW;
      } else {
        value.kind_ = CompactKind::LIST;
        if (count > 0) {
          uint8_t types = 0;
          if (OB_FAIL(read_byte(types))) {
          } else {
            const uint8_t key_type = types >> 4;
            const uint8_t mapped_type = types & 0x0f;
            value.list_.reserve(static_cast<size_t>(count * 2));
            for (uint64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
              CompactValue key;
              CompactValue mapped;
              if (OB_FAIL(read_value(key_type, key, depth + 1))) {
              } else if (OB_FAIL(read_value(mapped_type, mapped, depth + 1))) {
              } else {
                value.list_.push_back(std::move(key));
                value.list_.push_back(std::move(mapped));
              }
            }
          }
        }
      }
    } else if (wire_type == 12) {
      ret = read_struct(value, depth + 1);
    } else {
      ret = OB_INVALID_DATA;
    }
    return ret;
  }

  std::istream &input_;
  int64_t consumed_;
};

const CompactValue *compact_field(const CompactValue &value, int32_t field_id)
{
  const auto iter = value.fields_.find(field_id);
  return iter == value.fields_.end() ? nullptr : &iter->second;
}

int64_t compact_int(const CompactValue &value, int32_t field_id, int64_t default_value = 0)
{
  const CompactValue *field = compact_field(value, field_id);
  return field != nullptr && field->kind_ == CompactKind::INTEGER ? field->integer_ : default_value;
}

bool read_little_u32(std::istream &input, uint32_t &value)
{
  char bytes[4] = {0};
  input.read(bytes, sizeof(bytes));
  if (input.gcount() != sizeof(bytes)) return false;
  value = static_cast<uint32_t>(static_cast<uint8_t>(bytes[0]))
          | (static_cast<uint32_t>(static_cast<uint8_t>(bytes[1])) << 8)
          | (static_cast<uint32_t>(static_cast<uint8_t>(bytes[2])) << 16)
          | (static_cast<uint32_t>(static_cast<uint8_t>(bytes[3])) << 24);
  return true;
}

bool read_little_u32(const std::string &input, size_t &pos, uint32_t &value)
{
  if (pos > input.size() || input.size() - pos < 4) return false;
  value = static_cast<uint32_t>(static_cast<uint8_t>(input[pos]))
          | (static_cast<uint32_t>(static_cast<uint8_t>(input[pos + 1])) << 8)
          | (static_cast<uint32_t>(static_cast<uint8_t>(input[pos + 2])) << 16)
          | (static_cast<uint32_t>(static_cast<uint8_t>(input[pos + 3])) << 24);
  pos += 4;
  return true;
}

int snappy_uncompress(const std::string &input, std::string &output)
{
  int ret = OB_SUCCESS;
  size_t pos = 0;
  uint64_t expected = 0;
  int shift = 0;
  bool length_done = false;
  while (OB_SUCC(ret) && !length_done && pos < input.size()) {
    const uint8_t byte = static_cast<uint8_t>(input[pos++]);
    if (shift >= 64 || (shift == 63 && (byte & 0xfe) != 0)) ret = OB_INVALID_DATA;
    else {
      expected |= static_cast<uint64_t>(byte & 0x7f) << shift;
      shift += 7;
      length_done = (byte & 0x80) == 0;
    }
  }
  if (OB_SUCC(ret) && (!length_done || expected > MAX_PARQUET_PAGE_BYTES)) ret = OB_SIZE_OVERFLOW;
  output.clear();
  if (OB_SUCC(ret)) output.reserve(static_cast<size_t>(expected));
  while (OB_SUCC(ret) && pos < input.size() && output.size() < expected) {
    const uint8_t tag = static_cast<uint8_t>(input[pos++]);
    const uint8_t kind = tag & 3;
    uint64_t length = 0;
    uint64_t offset = 0;
    if (kind == 0) {
      const uint8_t encoded_length = tag >> 2;
      if (encoded_length < 60) {
        length = encoded_length + 1;
      } else {
        const int length_bytes = encoded_length - 59;
        if (length_bytes > 4 || pos + static_cast<size_t>(length_bytes) > input.size()) {
          ret = OB_INVALID_DATA;
        } else {
          uint64_t encoded = 0;
          for (int i = 0; i < length_bytes; ++i) {
            encoded |= static_cast<uint64_t>(static_cast<uint8_t>(input[pos++])) << (i * 8);
          }
          length = encoded + 1;
        }
      }
      if (OB_SUCC(ret) && (length > input.size() - pos || length > expected - output.size())) {
        ret = OB_INVALID_DATA;
      } else if (OB_SUCC(ret)) {
        output.append(input.data() + pos, static_cast<size_t>(length));
        pos += static_cast<size_t>(length);
      }
    } else {
      length = 1 + (tag >> 2);
      if (kind == 1) {
        length = 4 + ((tag >> 2) & 7);
        if (pos >= input.size()) ret = OB_INVALID_DATA;
        else {
          offset = (static_cast<uint64_t>(tag & 0xe0) << 3)
                   | static_cast<uint8_t>(input[pos++]);
        }
      } else if (kind == 2) {
        if (pos + 2 > input.size()) ret = OB_INVALID_DATA;
        else {
          offset = static_cast<uint8_t>(input[pos])
                   | (static_cast<uint64_t>(static_cast<uint8_t>(input[pos + 1])) << 8);
          pos += 2;
        }
      } else {
        if (pos + 4 > input.size()) ret = OB_INVALID_DATA;
        else {
          offset = static_cast<uint8_t>(input[pos])
                   | (static_cast<uint64_t>(static_cast<uint8_t>(input[pos + 1])) << 8)
                   | (static_cast<uint64_t>(static_cast<uint8_t>(input[pos + 2])) << 16)
                   | (static_cast<uint64_t>(static_cast<uint8_t>(input[pos + 3])) << 24);
          pos += 4;
        }
      }
      if (OB_SUCC(ret) && (offset == 0 || offset > output.size() || length > expected - output.size())) {
        ret = OB_INVALID_DATA;
      } else if (OB_SUCC(ret)) {
        for (uint64_t i = 0; i < length; ++i) output.push_back(output[output.size() - offset]);
      }
    }
  }
  if (OB_SUCC(ret) && (output.size() != expected || pos != input.size())) ret = OB_INVALID_DATA;
  return ret;
}

} // namespace

namespace
{
struct ParquetColumnInfo
{
  std::string name_;
  int32_t physical_type_;
  int32_t type_length_;
  int32_t max_definition_level_;
  int32_t max_repetition_level_;
  int32_t converted_type_;
  InferredType inferred_type_;
};

struct ParquetChunkInfo
{
  int64_t start_offset_;
  int64_t compressed_size_;
  int64_t num_values_;
  int32_t codec_;
  int32_t physical_type_;
  int32_t type_length_;
};

struct ParquetRowGroupInfo
{
  int64_t row_count_;
  std::vector<ParquetChunkInfo> chunks_;
};

struct ParquetMetadata
{
  std::vector<ParquetColumnInfo> columns_;
  std::vector<ParquetRowGroupInfo> row_groups_;
  FileIdentity identity_;
};

InferredType parquet_inferred_type(int32_t physical_type, int32_t converted_type)
{
  InferredType type = InferredType::VARCHAR;
  switch (physical_type) {
    case 0: type = InferredType::BOOLEAN; break;
    case 1: type = converted_type == 6 ? InferredType::DATE : InferredType::BIGINT; break;
    case 2: type = converted_type == 9 || converted_type == 10 || converted_type == 100
                      ? InferredType::DATETIME : InferredType::BIGINT; break;
    case 4:
    case 5: type = InferredType::DOUBLE; break;
    case 6:
    case 7: type = InferredType::VARCHAR; break;
    default: type = InferredType::VARCHAR; break;
  }
  return type;
}

int parse_parquet_metadata(const std::string &path, ParquetMetadata &metadata)
{
  int ret = OB_SUCCESS;
  metadata.columns_.clear();
  metadata.row_groups_.clear();
  std::ifstream input(path, std::ios::in | std::ios::binary);
  if (!input.is_open()) {
    ret = OB_FILE_NOT_EXIST;
  } else if (OB_FAIL(get_file_identity(path, metadata.identity_))) {
  } else if (metadata.identity_.size_ < 12) {
    ret = OB_INVALID_DATA;
  } else {
    char magic[4] = {0};
    input.read(magic, sizeof(magic));
    if (input.gcount() != sizeof(magic) || memcmp(magic, "PAR1", 4) != 0) {
      ret = OB_INVALID_DATA;
    }
    uint32_t footer_length = 0;
    if (OB_SUCC(ret)) {
      input.seekg(metadata.identity_.size_ - 8);
      char footer_tail[8] = {0};
      input.read(footer_tail, sizeof(footer_tail));
      if (input.gcount() != sizeof(footer_tail) || memcmp(footer_tail + 4, "PAR1", 4) != 0) {
        ret = OB_INVALID_DATA;
      } else {
        const std::string tail(footer_tail, sizeof(footer_tail));
        size_t pos = 0;
        if (!read_little_u32(tail, pos, footer_length)
            || footer_length > 64 * 1024 * 1024
            || static_cast<int64_t>(footer_length) > metadata.identity_.size_ - 12) {
          ret = OB_INVALID_DATA;
        }
      }
    }
    std::string footer;
    if (OB_SUCC(ret)) {
      footer.resize(footer_length);
      input.seekg(metadata.identity_.size_ - 8 - footer_length);
      input.read(footer.data(), static_cast<std::streamsize>(footer.size()));
      if (input.gcount() != static_cast<std::streamsize>(footer.size())) ret = OB_INVALID_DATA;
    }
    CompactValue root;
    if (OB_SUCC(ret)) {
      std::istringstream footer_input(footer, std::ios::in | std::ios::binary);
      CompactReader reader(footer_input);
      ret = reader.read_struct(root);
    }
    const CompactValue *schema = OB_SUCC(ret) ? compact_field(root, 2) : nullptr;
    const CompactValue *row_groups = OB_SUCC(ret) ? compact_field(root, 4) : nullptr;
    if (OB_SUCC(ret) && (schema == nullptr || schema->kind_ != CompactKind::LIST
                         || schema->list_.empty() || row_groups == nullptr
                         || row_groups->kind_ != CompactKind::LIST)) {
      ret = OB_INVALID_DATA;
    }
    int64_t declared_columns = 0;
    if (OB_SUCC(ret)) {
      const CompactValue &root_schema = schema->list_[0];
      declared_columns = compact_int(root_schema, 5, -1);
      if (declared_columns < 0 || declared_columns > DEFAULT_MAX_COLUMNS
          || static_cast<size_t>(declared_columns + 1) != schema->list_.size()) {
        ret = OB_NOT_SUPPORTED;
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < declared_columns; ++i) {
      const CompactValue &element = schema->list_[static_cast<size_t>(i + 1)];
      const CompactValue *name = compact_field(element, 4);
      const int64_t physical_type = compact_int(element, 1, -1);
      const int64_t repetition = compact_int(element, 3, 0);
      const int64_t children = compact_int(element, 5, 0);
      int64_t converted = compact_int(element, 6, -1);
      const CompactValue *logical = compact_field(element, 10);
      if (logical != nullptr) {
        if (compact_field(*logical, 6) != nullptr) converted = 6;
        const CompactValue *timestamp = compact_field(*logical, 8);
        const CompactValue *unit = timestamp == nullptr ? nullptr : compact_field(*timestamp, 2);
        if (unit != nullptr) {
          if (compact_field(*unit, 1) != nullptr) converted = 9;
          else if (compact_field(*unit, 2) != nullptr) converted = 10;
          else if (compact_field(*unit, 3) != nullptr) converted = 100;
        }
      }
      const int64_t type_length = compact_int(element, 2, 0);
      if (name == nullptr || name->kind_ != CompactKind::BINARY || name->binary_.empty()
          || physical_type < 0 || physical_type > 7 || physical_type == 3 || children != 0
          || repetition < 0 || repetition > 2 || repetition == 2
          || type_length < 0 || type_length > INT32_MAX) {
        ret = OB_NOT_SUPPORTED;
      } else {
        ParquetColumnInfo column;
        column.name_ = name->binary_;
        column.physical_type_ = static_cast<int32_t>(physical_type);
        column.type_length_ = static_cast<int32_t>(type_length);
        column.max_definition_level_ = repetition == 1 ? 1 : 0;
        column.max_repetition_level_ = 0;
        column.converted_type_ = static_cast<int32_t>(converted);
        column.inferred_type_ = parquet_inferred_type(column.physical_type_, static_cast<int32_t>(converted));
        metadata.columns_.push_back(std::move(column));
      }
    }
    if (OB_SUCC(ret) && row_groups->list_.size() > 1000000) ret = OB_SIZE_OVERFLOW;
    for (size_t group_idx = 0; OB_SUCC(ret) && group_idx < row_groups->list_.size(); ++group_idx) {
      const CompactValue &group_value = row_groups->list_[group_idx];
      const CompactValue *chunks = compact_field(group_value, 1);
      const int64_t row_count = compact_int(group_value, 3, -1);
      if (chunks == nullptr || chunks->kind_ != CompactKind::LIST
          || chunks->list_.size() != metadata.columns_.size()
          || row_count < 0 || row_count > 1000000000LL) {
        ret = OB_INVALID_DATA;
      } else {
        ParquetRowGroupInfo group;
        group.row_count_ = row_count;
        group.chunks_.reserve(chunks->list_.size());
        for (size_t col_idx = 0; OB_SUCC(ret) && col_idx < chunks->list_.size(); ++col_idx) {
          const CompactValue *column_metadata = compact_field(chunks->list_[col_idx], 3);
          if (column_metadata == nullptr || column_metadata->kind_ != CompactKind::STRUCT) {
            ret = OB_INVALID_DATA;
          } else {
            const int64_t physical_type = compact_int(*column_metadata, 1, -1);
            const int64_t codec = compact_int(*column_metadata, 4, -1);
            const int64_t num_values = compact_int(*column_metadata, 5, -1);
            const int64_t compressed_size = compact_int(*column_metadata, 7, -1);
            const int64_t data_offset = compact_int(*column_metadata, 9, -1);
            const int64_t dictionary_offset = compact_int(*column_metadata, 11, -1);
            const int64_t start_offset = dictionary_offset >= 0 ? dictionary_offset : data_offset;
            if (physical_type != metadata.columns_[col_idx].physical_type_
                || codec < 0 || codec > 1 || num_values < row_count
                || num_values > 2000000000LL || compressed_size <= 0
                || compressed_size > 1024LL * 1024 * 1024 || start_offset < 4
                || start_offset > metadata.identity_.size_
                || compressed_size > metadata.identity_.size_ - start_offset) {
              ret = OB_NOT_SUPPORTED;
            } else {
              ParquetChunkInfo chunk;
              chunk.start_offset_ = start_offset;
              chunk.compressed_size_ = compressed_size;
              chunk.num_values_ = num_values;
              chunk.codec_ = static_cast<int32_t>(codec);
              chunk.physical_type_ = static_cast<int32_t>(physical_type);
              chunk.type_length_ = metadata.columns_[col_idx].type_length_;
              group.chunks_.push_back(chunk);
            }
          }
        }
        if (OB_SUCC(ret)) metadata.row_groups_.push_back(std::move(group));
      }
    }
    if (OB_SUCC(ret)) {
      FileIdentity current;
      if (OB_FAIL(get_file_identity(path, current))) {
      } else if (current.device_ != metadata.identity_.device_ || current.inode_ != metadata.identity_.inode_
                 || current.size_ != metadata.identity_.size_ || current.modified_sec_ != metadata.identity_.modified_sec_
                 || current.modified_nsec_ != metadata.identity_.modified_nsec_) {
        ret = OB_ERR_UNEXPECTED;
      }
    }
  }
  return ret;
}

} // namespace

int infer_parquet_schema(const std::string &path, std::vector<CsvColumn> &columns)
{
  int ret = OB_SUCCESS;
  columns.clear();
  ParquetMetadata metadata;
  if (OB_FAIL(parse_parquet_metadata(path, metadata))) {
  } else {
    for (const ParquetColumnInfo &source : metadata.columns_) {
      CsvColumn column;
      column.name_ = source.name_;
      column.type_ = source.inferred_type_;
      column.nullable_ = source.max_definition_level_ > 0;
      columns.push_back(std::move(column));
    }
  }
  return ret;
}

namespace
{
struct ParquetColumnCursor
{
  ParquetColumnCursor() : offset_(0), values_read_(0), buffered_bytes_(0), row_index_(0) {}
  int64_t offset_;
  int64_t values_read_;
  int64_t buffered_bytes_;
  size_t row_index_;
  std::vector<ParquetField> values_;
  std::vector<std::string> dictionary_;
};

struct ParquetImpl
{
  ParquetImpl()
    : row_group_index_(0), row_index_(0), identity_(), row_group_loaded_(false), inited_(false)
  {}
  std::string path_;
  std::ifstream input_;
  ParquetMetadata metadata_;
  std::vector<size_t> selected_columns_;
  std::vector<ParquetColumnCursor> cursors_;
  size_t row_group_index_;
  size_t row_index_;
  FileIdentity identity_;
  bool row_group_loaded_;
  bool inited_;
};

int decode_hybrid(const std::string &input,
                  const int32_t bit_width,
                  const int64_t expected_count,
                  std::vector<int32_t> &values)
{
  int ret = OB_SUCCESS;
  values.clear();
  if (bit_width < 0 || bit_width > 32 || expected_count < 0 || expected_count > MAX_PARQUET_PAGE_VALUES) {
    ret = OB_INVALID_ARGUMENT;
  } else if (bit_width == 0) {
    values.assign(static_cast<size_t>(expected_count), 0);
  } else {
    size_t pos = 0;
    while (OB_SUCC(ret) && values.size() < static_cast<size_t>(expected_count)) {
      uint64_t header = 0;
      int shift = 0;
      bool done = false;
      while (OB_SUCC(ret) && !done) {
        if (pos >= input.size() || shift >= 64) {
          ret = OB_INVALID_DATA;
        } else {
          const uint8_t byte = static_cast<uint8_t>(input[pos++]);
          header |= static_cast<uint64_t>(byte & 0x7f) << shift;
          shift += 7;
          done = (byte & 0x80) == 0;
        }
      }
      if (OB_FAIL(ret)) {
      } else if ((header & 1) == 0) {
        const size_t count = static_cast<size_t>(header >> 1);
        const size_t width = static_cast<size_t>((bit_width + 7) / 8);
        if (count == 0 || count > static_cast<size_t>(expected_count) - values.size()
            || pos + width > input.size()) {
          ret = OB_INVALID_DATA;
        } else {
          uint32_t value = 0;
          for (size_t i = 0; i < width; ++i) value |= static_cast<uint32_t>(static_cast<uint8_t>(input[pos++])) << (8 * i);
          values.insert(values.end(), count, static_cast<int32_t>(value));
        }
      } else {
        const size_t groups = static_cast<size_t>(header >> 1);
        if (groups > (static_cast<size_t>(expected_count) + 7) / 8) {
          ret = OB_INVALID_DATA;
          break;
        }
        const size_t count = groups * 8;
        const size_t bytes = (count * static_cast<size_t>(bit_width) + 7) / 8;
        const size_t remaining = static_cast<size_t>(expected_count) - values.size();
        // Bit-packed groups are eight values wide and may contain padding in
        // the final group when the row count is not a multiple of eight.
        if (groups == 0 || (count > remaining && count - remaining >= 8)
            || pos + bytes > input.size()) {
          ret = OB_INVALID_DATA;
        } else {
          size_t bit_pos = 0;
          for (size_t i = 0; i < count; ++i) {
            uint32_t value = 0;
            for (int32_t bit = 0; bit < bit_width; ++bit) {
              const size_t source = bit_pos + static_cast<size_t>(bit);
              if ((static_cast<uint8_t>(input[pos + source / 8]) >> (source % 8)) & 1) {
                value |= 1U << bit;
              }
            }
            if (i < remaining) values.push_back(static_cast<int32_t>(value));
            bit_pos += static_cast<size_t>(bit_width);
          }
          pos += bytes;
        }
      }
    }
  }
  if (OB_SUCC(ret) && values.size() != static_cast<size_t>(expected_count)) ret = OB_INVALID_DATA;
  return ret;
}

int decode_levels(const std::string &input,
                  size_t &pos,
                  const int32_t bit_width,
                  const int64_t expected_count,
                  const bool has_length_prefix,
                  std::vector<int32_t> &values)
{
  int ret = OB_SUCCESS;
  uint32_t section_length = 0;
  if (has_length_prefix && !read_little_u32(input, pos, section_length)) {
    ret = OB_INVALID_DATA;
  } else if (!has_length_prefix) {
    section_length = static_cast<uint32_t>(input.size() - pos);
  }
  if (OB_SUCC(ret) && section_length > input.size() - pos) ret = OB_INVALID_DATA;
  if (OB_SUCC(ret)) {
    const std::string section(input.data() + pos, section_length);
    pos += section_length;
    ret = decode_hybrid(section, bit_width, expected_count, values);
  }
  return ret;
}

int decode_plain_value(const std::string &data,
                       size_t &pos,
                       const int32_t physical_type,
                       const int32_t type_length,
                       std::string &value)
{
  int ret = OB_SUCCESS;
  value.clear();
  uint32_t length = 0;
  switch (physical_type) {
    case 0:
      if (pos >= data.size()) ret = OB_INVALID_DATA;
      else value = data[pos++] == 0 ? "false" : "true";
      break;
    case 1:
      if (pos + 4 > data.size()) ret = OB_INVALID_DATA;
      else {
        const uint32_t raw = static_cast<uint32_t>(static_cast<uint8_t>(data[pos]))
          | (static_cast<uint32_t>(static_cast<uint8_t>(data[pos + 1])) << 8)
          | (static_cast<uint32_t>(static_cast<uint8_t>(data[pos + 2])) << 16)
          | (static_cast<uint32_t>(static_cast<uint8_t>(data[pos + 3])) << 24);
        value = std::to_string(static_cast<int32_t>(raw));
        pos += 4;
      }
      break;
    case 2:
      if (pos + 8 > data.size()) ret = OB_INVALID_DATA;
      else {
        uint64_t raw = 0;
        for (int i = 7; i >= 0; --i) raw = (raw << 8) | static_cast<uint8_t>(data[pos + i]);
        value = std::to_string(static_cast<int64_t>(raw));
        pos += 8;
      }
      break;
    case 4:
      if (pos + 4 > data.size()) ret = OB_INVALID_DATA;
      else {
        uint32_t raw = static_cast<uint32_t>(static_cast<uint8_t>(data[pos]))
          | (static_cast<uint32_t>(static_cast<uint8_t>(data[pos + 1])) << 8)
          | (static_cast<uint32_t>(static_cast<uint8_t>(data[pos + 2])) << 16)
          | (static_cast<uint32_t>(static_cast<uint8_t>(data[pos + 3])) << 24);
        float parsed = 0;
        memcpy(&parsed, &raw, sizeof(raw));
        std::ostringstream formatted;
        formatted << std::setprecision(std::numeric_limits<float>::max_digits10) << parsed;
        value = formatted.str();
        pos += 4;
      }
      break;
    case 5:
      if (pos + 8 > data.size()) ret = OB_INVALID_DATA;
      else {
        uint64_t raw = 0;
        for (int i = 7; i >= 0; --i) raw = (raw << 8) | static_cast<uint8_t>(data[pos + i]);
        double parsed = 0;
        memcpy(&parsed, &raw, sizeof(raw));
        std::ostringstream formatted;
        formatted << std::setprecision(std::numeric_limits<double>::max_digits10) << parsed;
        value = formatted.str();
        pos += 8;
      }
      break;
    case 6:
      if (!read_little_u32(data, pos, length)) {
        ret = OB_INVALID_DATA;
      } else if (length > data.size() - pos) {
        ret = OB_INVALID_DATA;
      } else {
        value.assign(data.data() + pos, length);
        pos += length;
      }
      break;
    case 7:
      if (type_length <= 0 || static_cast<size_t>(type_length) > data.size() - pos) ret = OB_INVALID_DATA;
      else { value.assign(data.data() + pos, static_cast<size_t>(type_length)); pos += static_cast<size_t>(type_length); }
      break;
    default: ret = OB_NOT_SUPPORTED; break;
  }
  return ret;
}

int decode_data_page(const std::string &payload,
                     const int32_t physical_type,
                     const int32_t type_length,
                     const int32_t max_repetition_level,
                     const int32_t max_definition_level,
                     const int32_t num_values,
                     const int32_t encoding,
                     const std::vector<std::string> &dictionary,
                     std::vector<ParquetField> &values)
{
  int ret = OB_SUCCESS;
  if (num_values < 0 || num_values > MAX_PARQUET_PAGE_VALUES) {
    ret = OB_SIZE_OVERFLOW;
  } else {
    size_t pos = 0;
    std::vector<int32_t> repetition;
    std::vector<int32_t> definition;
    const int32_t rep_width = max_repetition_level == 0 ? 0
      : static_cast<int32_t>(std::ceil(std::log2(static_cast<double>(max_repetition_level + 1))));
    const int32_t def_width = max_definition_level == 0 ? 0
      : static_cast<int32_t>(std::ceil(std::log2(static_cast<double>(max_definition_level + 1))));
    if (max_repetition_level > 0
        && OB_FAIL(decode_levels(payload, pos, rep_width, num_values, true, repetition))) {
    } else if (max_definition_level > 0
               && OB_FAIL(decode_levels(payload, pos, def_width, num_values, true, definition))) {
    } else if (encoding == 2 || encoding == 8) {
      if (pos >= payload.size()) ret = OB_INVALID_DATA;
      else {
        const int32_t index_width = static_cast<int32_t>(static_cast<uint8_t>(payload[pos++]));
        int64_t non_null_count = num_values;
        if (max_definition_level > 0) {
          non_null_count = 0;
          for (int32_t level : definition) non_null_count += level == max_definition_level;
        }
        {
          // Dictionary indices omit the four-byte length used by level streams.
          const std::string encoded(payload.data() + pos, payload.size() - pos);
          std::vector<int32_t> indices;
          if (OB_FAIL(decode_hybrid(encoded, index_width, non_null_count, indices))) {
          } else {
            size_t index = 0;
            for (int32_t i = 0; OB_SUCC(ret) && i < num_values; ++i) {
              const bool is_null = max_definition_level > 0 && definition[static_cast<size_t>(i)] < max_definition_level;
              ParquetField field;
              field.is_null_ = is_null;
              if (!is_null) {
                if (index >= indices.size() || indices[index] < 0
                    || static_cast<size_t>(indices[index]) >= dictionary.size()) ret = OB_INVALID_DATA;
                else field.value_ = dictionary[static_cast<size_t>(indices[index])];
                ++index;
              }
              values.push_back(std::move(field));
            }
          }
        }
      }
    } else if (encoding == 3 && physical_type == 0) {
      int64_t non_null_count = num_values;
      if (max_definition_level > 0) {
        non_null_count = 0;
        for (int32_t level : definition) non_null_count += level == max_definition_level;
      }
      std::vector<int32_t> boolean_values;
      if (OB_FAIL(decode_levels(payload, pos, 1, non_null_count, true, boolean_values))) {
      } else {
        size_t value_index = 0;
        for (int32_t i = 0; OB_SUCC(ret) && i < num_values; ++i) {
          ParquetField field;
          field.is_null_ = max_definition_level > 0 && definition[static_cast<size_t>(i)] < max_definition_level;
          if (!field.is_null_) {
            if (boolean_values[value_index] > 1 || boolean_values[value_index] < 0) ret = OB_INVALID_DATA;
            else field.value_ = boolean_values[value_index++] ? "true" : "false";
          }
          values.push_back(std::move(field));
        }
      }
    } else if (encoding != 0) {
      ret = OB_NOT_SUPPORTED;
    } else if (physical_type == 0) {
      size_t value_bit = 0;
      for (int32_t i = 0; OB_SUCC(ret) && i < num_values; ++i) {
        const bool is_null = max_definition_level > 0 && definition[static_cast<size_t>(i)] < max_definition_level;
        ParquetField field;
        field.is_null_ = is_null;
        if (!is_null) {
          if (value_bit / 8 >= payload.size() - pos) ret = OB_INVALID_DATA;
          else field.value_ = ((static_cast<uint8_t>(payload[pos + value_bit / 8]) >> (value_bit % 8)) & 1)
                              ? "true" : "false";
          ++value_bit;
        }
        values.push_back(std::move(field));
      }
      if (OB_SUCC(ret)) pos += (value_bit + 7) / 8;
    } else {
      for (int32_t i = 0; OB_SUCC(ret) && i < num_values; ++i) {
        const bool is_null = max_definition_level > 0 && definition[static_cast<size_t>(i)] < max_definition_level;
        ParquetField field;
        field.is_null_ = is_null;
        if (!is_null && OB_FAIL(decode_plain_value(payload, pos, physical_type, type_length, field.value_))) {
        }
        values.push_back(std::move(field));
      }
    }
  }
  return ret;
}

int parse_page_header(std::istream &input,
                      int32_t &page_type,
                      int32_t &compressed_size,
                      int32_t &uncompressed_size,
                      CompactValue &data_header,
                      CompactValue &data_header_v2,
                      CompactValue &dictionary_header,
                      int64_t &header_bytes)
{
  int ret = OB_SUCCESS;
  CompactReader reader(input);
  CompactValue header;
  if (OB_FAIL(reader.read_struct(header))) {
  } else {
    page_type = static_cast<int32_t>(compact_int(header, 1, -1));
    uncompressed_size = static_cast<int32_t>(compact_int(header, 2, -1));
    compressed_size = static_cast<int32_t>(compact_int(header, 3, -1));
    const CompactValue *v1 = compact_field(header, 5);
    const CompactValue *v2 = compact_field(header, 8);
    const CompactValue *dict = compact_field(header, 7);
    if (v1 != nullptr) data_header = *v1;
    if (v2 != nullptr) data_header_v2 = *v2;
    if (dict != nullptr) dictionary_header = *dict;
    header_bytes = reader.consumed();
    if (page_type < 0 || compressed_size < 0 || uncompressed_size < 0) ret = OB_INVALID_DATA;
  }
  return ret;
}

int format_parquet_temporal(const ParquetColumnInfo &column, std::string &value)
{
  int ret = OB_SUCCESS;
  if (column.inferred_type_ == InferredType::DATE || column.inferred_type_ == InferredType::DATETIME) {
    char *end = nullptr;
    errno = 0;
    const int64_t raw = std::strtoll(value.c_str(), &end, 10);
    if (errno == ERANGE || end != value.data() + value.size()) {
      ret = OB_INVALID_DATA;
    } else {
      const bool timestamp = column.inferred_type_ == InferredType::DATETIME;
      const int64_t units = column.converted_type_ == 9 ? 1000
        : column.converted_type_ == 100 ? 1000000000 : 1000000;
      const int64_t seconds = timestamp ? raw / units - (raw % units < 0) : 0;
      const int64_t days = timestamp ? seconds / 86400 - (seconds % 86400 < 0) : raw;
      // Civil date from a signed Unix day number, including dates before 1970.
      if (days < -719162 || days > 2932896) {
        ret = OB_INVALID_DATA;
      } else {
        const int64_t z = days + 719468;
        const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
        const unsigned doe = static_cast<unsigned>(z - era * 146097);
        const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        int year = static_cast<int>(yoe + era * 400);
        const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const unsigned mp = (5 * doy + 2) / 153;
        const unsigned day = doy - (153 * mp + 2) / 5 + 1;
        const unsigned month = mp < 10 ? mp + 3 : mp - 9;
        year += month <= 2;
        std::ostringstream formatted;
        formatted << std::setfill('0') << std::setw(4) << year << '-'
                  << std::setw(2) << month << '-' << std::setw(2) << day;
        if (timestamp) {
          const int64_t time = seconds - days * 86400;
          const int64_t fraction = ((raw % units + units) % units) * (1000000000 / units) / 1000;
          formatted << ' ' << std::setw(2) << time / 3600 << ':' << std::setw(2) << time / 60 % 60
                    << ':' << std::setw(2) << time % 60 << '.' << std::setw(6) << fraction;
        }
        value = formatted.str();
      }
    }
  }
  return ret;
}

int read_parquet_page(ParquetImpl &impl,
                        const ParquetChunkInfo &chunk,
                        const ParquetColumnInfo &column,
                        ParquetColumnCursor &cursor)
{
  int ret = OB_SUCCESS;
  std::vector<ParquetField>().swap(cursor.values_);
  cursor.row_index_ = 0;
  std::vector<ParquetField> &values = cursor.values_;
  std::vector<std::string> &dictionary = cursor.dictionary_;
  int64_t &offset = cursor.offset_;
  int64_t &value_count = cursor.values_read_;
  const int64_t chunk_end = chunk.start_offset_ + chunk.compressed_size_;
  // Keep one decoded page per projected column, independent of Row Group size.
  while (OB_SUCC(ret) && values.empty() && value_count < chunk.num_values_) {
    impl.input_.clear();
    impl.input_.seekg(offset);
    if (!impl.input_ || offset >= chunk_end) {
      ret = OB_IO_ERROR;
    } else {
      int32_t page_type = 0;
      int32_t compressed_size = 0;
      int32_t uncompressed_size = 0;
      int64_t header_bytes = 0;
      CompactValue data_header;
      CompactValue data_header_v2;
      CompactValue dictionary_header;
      if (OB_FAIL(parse_page_header(impl.input_, page_type, compressed_size, uncompressed_size,
                                    data_header, data_header_v2, dictionary_header, header_bytes))) {
      } else if (compressed_size > MAX_PARQUET_PAGE_BYTES || uncompressed_size > MAX_PARQUET_PAGE_BYTES) {
        ret = OB_SIZE_OVERFLOW;
      } else if (header_bytes > chunk_end - offset
                 || compressed_size > chunk_end - offset - header_bytes) {
        ret = OB_INVALID_DATA;
      } else {
        std::string compressed(static_cast<size_t>(compressed_size), '\0');
        impl.input_.read(compressed.data(), compressed.size());
        if (impl.input_.gcount() != static_cast<std::streamsize>(compressed.size())) {
          ret = OB_INVALID_DATA;
        } else {
          std::string payload;
          // Parquet DataPageV2 keeps repetition/definition levels uncompressed;
          // only the values suffix is compressed with the page codec.
          const CompactValue *v2_compressed = compact_field(data_header_v2, 7);
          if (page_type == 3 && chunk.codec_ == 1
              && (v2_compressed == nullptr || v2_compressed->boolean_)) {
            const int32_t definition_bytes = static_cast<int32_t>(compact_int(data_header_v2, 5, -1));
            const int32_t repetition_bytes = static_cast<int32_t>(compact_int(data_header_v2, 6, -1));
            const int64_t levels_bytes = static_cast<int64_t>(definition_bytes) + repetition_bytes;
            if (definition_bytes < 0 || repetition_bytes < 0 || levels_bytes > static_cast<int64_t>(compressed.size())) {
              ret = OB_INVALID_DATA;
            } else {
              std::string values;
              const size_t prefix_size = static_cast<size_t>(levels_bytes);
              const std::string compressed_values(compressed.data() + prefix_size,
                                                   compressed.size() - prefix_size);
              if (OB_FAIL(snappy_uncompress(compressed_values, values))) {
              } else {
                payload.assign(compressed.data(), prefix_size);
                payload.append(values);
              }
            }
          } else if (chunk.codec_ == 1 && page_type != 3) {
            ret = snappy_uncompress(compressed, payload);
          } else {
            payload = std::move(compressed);
          }
          if (OB_SUCC(ret) && payload.size() != static_cast<size_t>(uncompressed_size)) ret = OB_INVALID_DATA;
          if (OB_SUCC(ret) && page_type == 2) {
            const int32_t dictionary_values = static_cast<int32_t>(compact_int(dictionary_header, 1, -1));
            size_t value_pos = 0;
            if (dictionary_values < 0 || dictionary_values > MAX_PARQUET_PAGE_VALUES) ret = OB_SIZE_OVERFLOW;
            else if (!dictionary.empty() || compact_int(dictionary_header, 2, -1) != 0) ret = OB_INVALID_DATA;
            else {
              for (int32_t i = 0; OB_SUCC(ret) && i < dictionary_values; ++i) {
                std::string value;
                if (OB_FAIL(decode_plain_value(payload, value_pos, column.physical_type_, column.type_length_, value))) {
                } else {
                  dictionary.push_back(std::move(value));
                }
              }
            }
          } else if (OB_SUCC(ret) && page_type == 0) {
            const int32_t page_values = static_cast<int32_t>(compact_int(data_header, 1, -1));
            const int32_t encoding = static_cast<int32_t>(compact_int(data_header, 2, -1));
            const int32_t level_encoding = static_cast<int32_t>(compact_int(data_header, 3, -1));
            if (page_values <= 0 || page_values > chunk.num_values_ - value_count
                || encoding < 0 || level_encoding != 3) ret = OB_INVALID_DATA;
            else if (OB_FAIL(decode_data_page(payload, column.physical_type_, column.type_length_,
                                              column.max_repetition_level_, column.max_definition_level_,
                                              page_values, encoding,
                                              dictionary, values))) {
            } else {
              value_count += page_values;
            }
          } else if (OB_SUCC(ret) && page_type == 3) {
            const int32_t page_values = static_cast<int32_t>(compact_int(data_header_v2, 1, -1));
            const int32_t null_count = static_cast<int32_t>(compact_int(data_header_v2, 2, -1));
            const int32_t encoding = static_cast<int32_t>(compact_int(data_header_v2, 4, -1));
            const int32_t definition_bytes = static_cast<int32_t>(compact_int(data_header_v2, 5, -1));
            const int32_t repetition_bytes = static_cast<int32_t>(compact_int(data_header_v2, 6, -1));
            if (page_values <= 0 || page_values > chunk.num_values_ - value_count
                || null_count < 0 || null_count > page_values || definition_bytes < 0 || repetition_bytes != 0
                || static_cast<size_t>(definition_bytes) + repetition_bytes > payload.size()) {
              ret = OB_INVALID_DATA;
            } else {
              // Normalize the V2 level section to the V1 layout, then share the
              // plain/dictionary/boolean decoder (V2 lengths exclude prefixes).
              std::string normalized;
              if (column.max_definition_level_ > 0) {
                for (int i = 0; i < 4; ++i) normalized.push_back(static_cast<char>((definition_bytes >> (8 * i)) & 0xff));
                normalized.append(payload.data(), static_cast<size_t>(definition_bytes));
              }
              normalized.append(payload.data() + definition_bytes, payload.size() - definition_bytes);
              if (OB_FAIL(decode_data_page(normalized, column.physical_type_, column.type_length_,
                                            0, column.max_definition_level_, page_values, encoding,
                                            dictionary, values))) {
              } else value_count += page_values;
            }
          } else {
            if (OB_SUCC(ret)) {
              ret = OB_NOT_SUPPORTED;
            }
          }
        }
      }
      offset += header_bytes + compressed_size;
    }
  }
  if (OB_SUCC(ret) && (value_count > chunk.num_values_ || values.empty())) {
    ret = OB_INVALID_DATA;
  }
  for (size_t i = 0; OB_SUCC(ret) && i < values.size(); ++i) {
    if (!values[i].is_null_) ret = format_parquet_temporal(column, values[i].value_);
  }
  cursor.buffered_bytes_ = values.capacity() * sizeof(ParquetField)
                           + dictionary.capacity() * sizeof(std::string);
  for (const ParquetField &field : values) cursor.buffered_bytes_ += field.value_.capacity();
  for (const std::string &value : dictionary) cursor.buffered_bytes_ += value.capacity();
  if (OB_SUCC(ret)) {
    FileIdentity current;
    if (OB_FAIL(get_file_identity(impl.path_, current))) {
    } else if (current.device_ != impl.identity_.device_ || current.inode_ != impl.identity_.inode_
               || current.size_ != impl.identity_.size_ || current.modified_sec_ != impl.identity_.modified_sec_
               || current.modified_nsec_ != impl.identity_.modified_nsec_) {
      ret = OB_ERR_UNEXPECTED;
    }
  }
  return ret;
}

int load_parquet_row_group(ParquetImpl &impl, const ParquetRowGroupInfo &group)
{
  int ret = OB_SUCCESS;
  impl.cursors_.clear();
  if (group.row_count_ < 0 || group.row_count_ > 1000000000LL
      || group.chunks_.size() != impl.metadata_.columns_.size()) {
    ret = OB_INVALID_DATA;
  } else {
    impl.cursors_.resize(impl.selected_columns_.size());
    for (size_t i = 0; OB_SUCC(ret) && i < impl.selected_columns_.size(); ++i) {
      const size_t source_index = impl.selected_columns_[i];
      if (source_index >= group.chunks_.size()) {
        ret = OB_ERR_UNEXPECTED;
      } else if (group.chunks_[source_index].num_values_ != group.row_count_) {
        ret = OB_INVALID_DATA;
      } else {
        impl.cursors_[i].offset_ = group.chunks_[source_index].start_offset_;
      }
    }
  }
  if (OB_SUCC(ret)) {
    impl.row_index_ = 0;
    impl.row_group_loaded_ = true;
  }
  return ret;
}

} // namespace

ParquetReader::ParquetReader() : impl_(nullptr) {}

ParquetReader::~ParquetReader()
{
  close();
}

int ParquetReader::open(const std::string &path, const std::vector<std::string> &projected_columns)
{
  int ret = OB_SUCCESS;
  close();
  ParquetImpl *impl = new (std::nothrow) ParquetImpl();
  if (impl == nullptr) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(parse_parquet_metadata(path, impl->metadata_))) {
    delete impl;
  } else {
    impl->input_.open(path, std::ios::in | std::ios::binary);
    if (!impl->input_.is_open()) {
      ret = OB_FILE_NOT_EXIST;
      delete impl;
    } else if (OB_FAIL(get_file_identity(path, impl->identity_))) {
      impl->input_.close();
      delete impl;
    } else {
      impl->path_ = path;
      impl->row_group_index_ = 0;
      impl->row_index_ = 0;
      impl->row_group_loaded_ = false;
      for (size_t i = 0; i < impl->metadata_.columns_.size(); ++i) {
        bool selected = false;
        for (const std::string &name : projected_columns) {
          if (name.size() == impl->metadata_.columns_[i].name_.size()
              && std::equal(name.begin(), name.end(), impl->metadata_.columns_[i].name_.begin(),
                            [](char left, char right) {
                              return std::tolower(static_cast<unsigned char>(left))
                                     == std::tolower(static_cast<unsigned char>(right));
                            })) {
            selected = true;
            break;
          }
        }
        if (selected) impl->selected_columns_.push_back(i);
      }
      impl->inited_ = true;
      if (OB_SUCC(ret)) impl_ = impl;
    }
  }
  return ret;
}

int ParquetReader::next(ParquetRecord &record, bool &has_record)
{
  int ret = OB_SUCCESS;
  has_record = false;
  record.fields_.clear();
  record.row_number_ = 0;
  ParquetImpl *impl = static_cast<ParquetImpl *>(impl_);
  if (impl == nullptr || !impl->inited_) {
    ret = OB_NOT_INIT;
  } else {
    while (OB_SUCC(ret) && impl->row_group_index_ < impl->metadata_.row_groups_.size()) {
      const ParquetRowGroupInfo &group = impl->metadata_.row_groups_[impl->row_group_index_];
      if (!impl->row_group_loaded_) {
        if (OB_FAIL(load_parquet_row_group(*impl, group))) {
        }
      }
      if (OB_SUCC(ret) && impl->row_index_ < static_cast<size_t>(group.row_count_)) {
        FileIdentity current;
        if (OB_FAIL(get_file_identity(impl->path_, current))) {
        } else if (current.device_ != impl->identity_.device_ || current.inode_ != impl->identity_.inode_
                   || current.size_ != impl->identity_.size_ || current.modified_sec_ != impl->identity_.modified_sec_
                   || current.modified_nsec_ != impl->identity_.modified_nsec_) {
          ret = OB_ERR_UNEXPECTED;
        }
        record.fields_.resize(impl->cursors_.size());
        int64_t buffered_bytes = 0;
        for (size_t i = 0; OB_SUCC(ret) && i < impl->cursors_.size(); ++i) {
          ParquetColumnCursor &cursor = impl->cursors_[i];
          const size_t source_index = impl->selected_columns_[i];
          if (cursor.row_index_ == cursor.values_.size()) {
            ret = read_parquet_page(*impl, group.chunks_[source_index],
                                     impl->metadata_.columns_[source_index], cursor);
          }
          buffered_bytes += cursor.buffered_bytes_;
          if (OB_SUCC(ret) && buffered_bytes > MAX_PARQUET_BUFFER_BYTES) ret = OB_SIZE_OVERFLOW;
          if (OB_SUCC(ret)) record.fields_[i] = cursor.values_[cursor.row_index_++];
        }
        record.row_number_ = impl->row_index_;
        ++impl->row_index_;
        has_record = OB_SUCC(ret);
        break;
      } else if (OB_SUCC(ret)) {
        ++impl->row_group_index_;
        impl->cursors_.clear();
        impl->row_index_ = 0;
        impl->row_group_loaded_ = false;
      }
    }
  }
  return ret;
}

void ParquetReader::close()
{
  ParquetImpl *impl = static_cast<ParquetImpl *>(impl_);
  if (impl != nullptr) {
    if (impl->input_.is_open()) impl->input_.close();
    delete impl;
    impl_ = nullptr;
  }
}

const char *inferred_type_name(InferredType type)
{
  const char *name = "VARCHAR";
  switch (type) {
    case InferredType::NULL_TYPE: name = "NULL"; break;
    case InferredType::BIGINT: name = "BIGINT"; break;
    case InferredType::DOUBLE: name = "DOUBLE"; break;
    case InferredType::BOOLEAN: name = "BOOLEAN"; break;
    case InferredType::VARCHAR: name = "VARCHAR"; break;
    case InferredType::DATE: name = "DATE"; break;
    case InferredType::DATETIME: name = "DATETIME"; break;
    default: break;
  }
  return name;
}

} // namespace file_sql
} // namespace sql
} // namespace oceanbase
