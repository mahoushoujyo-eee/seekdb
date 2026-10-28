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
#include <sys/stat.h>
#if !defined(_WIN32)
#include <glob.h>
#endif

#include "lib/ob_errno.h"

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
          last.is_null_ = !last.quoted_ && (last.value_ == "NULL"
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
            last.is_null_ = !last.quoted_ && (last.value_ == "NULL"
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
            last.is_null_ = !last.quoted_ && (last.value_ == "NULL"
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
          files.push_back(result.gl_pathv[i]);
        }
      }
      std::sort(files.begin(), files.end());
    }
    ::globfree(&result);
#endif
  }
  return ret;
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
