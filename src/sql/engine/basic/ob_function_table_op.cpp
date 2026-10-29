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

#include "sql/engine/basic/ob_function_table_op.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "common/timezone/ob_time_convert.h"
#include "sql/engine/expr/ob_expr_operator.h"
#include "sql/resolver/ob_resolver_utils.h"
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <limits.h>


namespace oceanbase
{
using namespace common;
namespace sql
{

OB_SERIALIZE_MEMBER((ObFunctionTableSpec, ObOpSpec), value_expr_, column_exprs_, has_correlated_expr_,
                    is_file_table_, file_func_name_, file_path_, file_column_names_,
                    file_csv_header_, file_csv_delimiter_);

namespace
{
int parse_file_int(const std::string &value, int64_t &result)
{
  char *end = nullptr;
  errno = 0;
  result = std::strtoll(value.c_str(), &end, 10);
  return (errno == 0 && end == value.c_str() + value.size()) ? OB_SUCCESS : OB_INVALID_DATA;
}

int parse_file_double(const std::string &value, double &result)
{
  char *end = nullptr;
  errno = 0;
  result = std::strtod(value.c_str(), &end);
  return (errno == 0 && end == value.c_str() + value.size()) ? OB_SUCCESS : OB_INVALID_DATA;
}
}

int ObFunctionTableOp::inner_open()
{
  int ret = OB_SUCCESS;
  node_idx_ = 0;
  already_calc_ = false;
  if (MY_SPEC.is_file_table_) {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_file;
    ret = open_file_reader();
  } else if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ObExtendType == MY_SPEC.value_expr_->datum_meta_.type_) {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_udf;
  } else {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_sys_func;
  }
  return ret;
}

int ObFunctionTableOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else {
    node_idx_ = 0;
    if (MY_SPEC.is_file_table_) {
      if (OB_NOT_NULL(file_reader_)) file_reader_->close();
      if (OB_NOT_NULL(json_reader_)) json_reader_->close();
      if (OB_NOT_NULL(parquet_reader_)) parquet_reader_->close();
      file_reader_ = NULL;
      json_reader_ = NULL;
      parquet_reader_ = NULL;
      file_record_ready_ = false;
      file_has_record_ = false;
      file_header_skipped_ = false;
      next_file_idx_ = 0;
      if (OB_FAIL(open_file_reader())) {
      }
    }
    if (MY_SPEC.has_correlated_expr_) {
      row_count_ = 0;
      col_count_ = 0;
      value_table_ = NULL;
      already_calc_ = false;
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_close()
{
  int ret = OB_SUCCESS;
  node_idx_ = 0;
  row_count_ = 0;
  col_count_ = 0;
  value_table_ = NULL;
  if (OB_NOT_NULL(file_reader_)) file_reader_->close();
  if (OB_NOT_NULL(json_reader_)) json_reader_->close();
  if (OB_NOT_NULL(parquet_reader_)) parquet_reader_->close();
  file_reader_ = NULL;
  json_reader_ = NULL;
  parquet_reader_ = NULL;
  file_paths_.clear();
  return ret;
}

void ObFunctionTableOp::destroy()
{
  ObOperator::destroy();
}


int ObFunctionTableOp::get_current_result(ObObj &result)
{
  int ret = OB_SUCCESS;
  void *data = NULL;
  CK (already_calc_);
  if (node_idx_ < 0 || node_idx_ >= row_count_ * col_count_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("fail to get current result in table function",
              K(node_idx_), K(row_count_), K(col_count_));
  }
  do {
    CK (node_idx_ >= 0);
    if (OB_SUCC(ret) && node_idx_ >= row_count_) {
      ret = OB_ITER_END;
    }
    CK (OB_NOT_NULL(value_table_));
    OX (data = value_table_->get_data());
    CK (OB_NOT_NULL(data));
    OX (result = (static_cast<ObObj*>(data))[node_idx_++]);
  } while (OB_SUCC(ret) && result.get_meta().get_type() == ObMaxType);
  return ret;
}

int ObFunctionTableOp::inner_get_next_row()
{
  return (this->*next_row_func_)();
}

int ObFunctionTableOp::inner_get_next_row_udf()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ObExtendType != MY_SPEC.value_expr_->datum_meta_.type_) {
    ret = OB_ERR_UNEXPECTED;
  } else if (FALSE_IT(plan_ctx->set_autoinc_id_tmp(0))) {
  } else if (OB_FAIL(ctx_.check_status())) {
  } else {
    ObDatum *value = nullptr;
    if (!already_calc_) {
      if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
      } else if (value->is_null()) {
        //do nothing
      } else if (OB_ISNULL(value_table_ 
                 = reinterpret_cast<pl::ObPLCollection*>(value->get_ext()))) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        row_count_ = value_table_->is_inited() ? value_table_->get_count() : 0;
        col_count_ = value_table_->get_column_count();
      }
      already_calc_ = true;
    }
    if (OB_SUCC(ret) && OB_UNLIKELY(node_idx_ >= row_count_)) {
      ret = OB_ITER_END;
    }
    CK (MY_SPEC.column_exprs_.count() >= col_count_);
    ObObj obj_stack[col_count_];
    if (OB_SUCC(ret)) {
      if (nullptr != value_table_ 
          && ObExtendType == value_table_->get_element_type().get_obj_type()) {
        pl::ObPLComposite *composite = NULL;
        pl::ObPLRecord *record = NULL;
        ObObj record_obj;
        OZ (get_current_result(record_obj));
        if (OB_FAIL(ret)) {
        } else if (ObUserDefinedSQLType == record_obj.get_type()) {
          obj_stack[0] = record_obj;   
        } else if (record_obj.is_pl_extend()) {
          CK (OB_NOT_NULL(composite = reinterpret_cast<pl::ObPLComposite*>(record_obj.get_ext())));
          if (OB_SUCC(ret)) {
            if (composite->is_record()) {
              OX (record = static_cast<pl::ObPLRecord*>(composite));
              CK (record->get_count() == col_count_);
              for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
                OZ (record->get_element(i, obj_stack[i]));
              }
            } else if (composite->is_collection()) {
              OX (obj_stack[0] = record_obj);
            } else {
              ret = OB_ERR_UNEXPECTED;
            }
          }
        } else {
          ret = OB_ERR_UNEXPECTED;
          LOG_ERROR("unexpected here", K(ret), K(record_obj), K(record_obj.meta_));
        }
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
          if (OB_FAIL(get_current_result(obj_stack[i]))) {
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
        if (obj_stack[i].is_null()) {
          MY_SPEC.column_exprs_.at(i)->locate_datum_for_write(eval_ctx_).set_null();
        } else {
          const ObObjDatumMapType &datum_map = MY_SPEC.column_exprs_.at(i)->obj_datum_map_;
          ObExpr * const &expr = MY_SPEC.column_exprs_.at(i);
          ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
          if (OB_FAIL(datum.from_obj(obj_stack[i], datum_map))) {
          } else if (is_lob_storage(obj_stack[i].get_type()) &&
                     OB_FAIL(ob_adjust_lob_datum(get_exec_ctx(), obj_stack[i],
                                                 expr->obj_meta_, datum_map,
                                                 get_exec_ctx().get_allocator(), datum))) {
          }
        }
        if (OB_SUCC(ret)) {
          MY_SPEC.column_exprs_.at(i)->set_evaluated_projected(eval_ctx_);
        }
      }
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_get_next_row_sys_func()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  ObDatum *value = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ctx_.check_status())) {
  } else if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
    if (OB_ITER_END != ret) {
    }
  } else {
    MY_SPEC.column_exprs_.at(0)->locate_datum_for_write(eval_ctx_).set_datum(*value);
    MY_SPEC.column_exprs_.at(0)->set_evaluated_projected(eval_ctx_);
  }
  return ret;
}

int ObFunctionTableOp::open_file_reader()
{
  int ret = OB_SUCCESS;
  file_reader_ = NULL;
  json_reader_ = NULL;
  parquet_reader_ = NULL;
  file_paths_.clear();
  next_file_idx_ = 0;
  file_record_ready_ = false;
  file_has_record_ = false;
  file_header_skipped_ = false;
  const std::string path(MY_SPEC.file_path_.ptr(), MY_SPEC.file_path_.length());
  const std::string func(MY_SPEC.file_func_name_.ptr(), MY_SPEC.file_func_name_.length());
  auto validate_path = [this, &ret](const std::string &input_path, std::string &resolved_path) -> int {
    int path_ret = OB_SUCCESS;
    char real_buf[PATH_MAX] = {0};
    const char *real_file = ::realpath(input_path.c_str(), real_buf);
    ObSQLSessionInfo *session = ctx_.get_my_session();
    ObString secure_file_priv;
    if (OB_ISNULL(real_file)) {
      path_ret = OB_FILE_NOT_EXIST;
      LOG_ERROR("file SQL runtime realpath failed", K(errno));
    } else if (OB_ISNULL(session)) {
      path_ret = OB_ERR_UNEXPECTED;
    } else if (OB_SUCCESS != (path_ret = session->get_secure_file_priv(secure_file_priv))) {
    } else {
      ObString full_path(static_cast<int32_t>(std::strlen(real_file)), real_file);
      if (OB_SUCCESS != (path_ret = ObResolverUtils::check_secure_path(secure_file_priv, full_path))) {
        LOG_ERROR("file SQL runtime secure path rejected", K(path_ret));
      } else {
        resolved_path.assign(real_file, std::strlen(real_file));
      }
    }
    return path_ret;
  };
  if (func == "glob" || func == "GLOB") {
    std::vector<std::string> discovered_files;
    if (OB_FAIL(file_sql::discover_files(path, discovered_files))) {
    } else if (discovered_files.empty()) {
      ret = OB_FILE_NOT_EXIST;
    } else {
      for (const std::string &candidate : discovered_files) {
        std::string resolved_path;
        if (OB_FAIL(validate_path(candidate, resolved_path))) {
          break;
        } else if (file_paths_.size() >= 4096) {
          ret = OB_SIZE_OVERFLOW;
          break;
        } else {
          file_paths_.push_back(std::move(resolved_path));
        }
      }
      if (OB_SUCC(ret) && file_paths_.empty()) ret = OB_FILE_NOT_EXIST;
    }
  } else if (func == "read_csv" || func == "READ_CSV") {
    file_sql::CsvOptions options;
    options.header_ = MY_SPEC.file_csv_header_;
    options.delimiter_ = MY_SPEC.file_csv_delimiter_;
    std::string resolved_path;
    if (OB_FAIL(validate_path(path, resolved_path))) {
    } else {
      file_reader_ = &file_reader_obj_;
      if (OB_FAIL(file_reader_->open(resolved_path, options))) {
        LOG_ERROR("file SQL CSV reader open failed", K(ret), K(errno));
        file_reader_ = NULL;
      }
    }
  } else if (func == "read_json" || func == "READ_JSON"
             || func == "read_ndjson" || func == "READ_NDJSON") {
    file_sql::JsonOptions options;
    std::string resolved_path;
    if (OB_FAIL(validate_path(path, resolved_path))) {
    } else {
      json_reader_ = &json_reader_obj_;
      if (OB_FAIL(json_reader_->open(resolved_path, options))) {
        json_reader_ = NULL;
      }
    }
  } else if (func == "read_parquet" || func == "READ_PARQUET") {
    std::string resolved_path;
    if (OB_FAIL(validate_path(path, resolved_path))) {
    } else {
      std::vector<std::string> projected_columns;
      for (int64_t i = 0; i < MY_SPEC.file_column_names_.count(); ++i) {
        const ObString &column_name = MY_SPEC.file_column_names_.at(i);
        projected_columns.emplace_back(column_name.ptr(), static_cast<size_t>(column_name.length()));
      }
      parquet_reader_ = &parquet_reader_obj_;
      if (OB_FAIL(parquet_reader_->open(resolved_path, projected_columns))) {
        parquet_reader_ = NULL;
      }
    }
  } else {
    ret = OB_NOT_SUPPORTED;
  }
  return ret;
}

int ObFunctionTableOp::fill_file_datum(const std::string &value, const bool is_null, ObExpr *expr)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(expr)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
    if (is_null) {
      datum.set_null();
    } else {
      switch (expr->datum_meta_.type_) {
        case ObTinyIntType:
        case ObSmallIntType:
        case ObMediumIntType:
        case ObInt32Type:
        case ObIntType: {
          int64_t value_int = 0;
          if (OB_FAIL(parse_file_int(value, value_int))) {
            if (value == "true" || value == "TRUE") { value_int = 1; ret = OB_SUCCESS; }
            else if (value == "false" || value == "FALSE") { value_int = 0; ret = OB_SUCCESS; }
          }
          if (OB_SUCC(ret)) datum.set_int(value_int);
          break;
        }
        case ObFloatType:
        case ObDoubleType: {
          double value_double = 0;
          if (OB_FAIL(parse_file_double(value, value_double))) {
          } else { datum.set_double(value_double); }
          break;
        }
        case ObDateType: {
          ObDateSqlMode mode;
          int32_t date = 0;
          if (OB_FAIL(ObTimeConverter::str_to_date(ObString(value.length(), value.data()), date, mode))) {
          } else { datum.set_date(date); }
          break;
        }
        case ObDateTimeType: {
          ObTimeConvertCtx cvrt_ctx(NULL, false);
          int64_t datetime = 0;
          if (OB_FAIL(ObTimeConverter::str_to_datetime(ObString(value.length(), value.data()), cvrt_ctx,
                                                       datetime, NULL, cvrt_ctx.date_sql_mode_))) {
          } else { datum.set_datetime(datetime); }
          break;
        }
        default:
          {
            ObExprStrResAlloc res_alloc(*expr, eval_ctx_);
            const int64_t alloc_size = std::max<int64_t>(1, value.length());
            char *buffer = static_cast<char *>(res_alloc.alloc(alloc_size));
            if (OB_ISNULL(buffer)) {
              ret = OB_ALLOCATE_MEMORY_FAILED;
            } else {
              if (!value.empty()) MEMCPY(buffer, value.data(), value.length());
              datum.set_string(ObString(static_cast<int32_t>(value.length()), buffer));
            }
          }
          break;
      }
    }
    if (OB_SUCC(ret)) expr->set_evaluated_projected(eval_ctx_);
  }
  return ret;
}

int ObFunctionTableOp::inner_get_next_row_file()
{
  int ret = OB_SUCCESS;
  clear_evaluated_flag();
  if (OB_FAIL(ctx_.check_status())) {
  } else if (MY_SPEC.file_func_name_.case_compare("glob") == 0) {
    if (next_file_idx_ >= static_cast<int64_t>(file_paths_.size())) {
      ret = OB_ITER_END;
    } else {
      for (int64_t i = 0; i < MY_SPEC.column_exprs_.count(); ++i) {
        OZ(fill_file_datum(i == 0 ? file_paths_[next_file_idx_] : std::string(), i != 0,
                           MY_SPEC.column_exprs_.at(i)));
      }
      ++next_file_idx_;
    }
  } else if (OB_NOT_NULL(file_reader_)) {
    bool has_record = false;
    do {
      OZ(file_reader_->next(file_record_, has_record));
      if (OB_FAIL(ret) || !has_record) break;
      if (MY_SPEC.file_csv_header_ && !file_header_skipped_) {
        file_header_skipped_ = true;
        has_record = false;
        continue;
      }
      for (int64_t i = 0; i < MY_SPEC.column_exprs_.count(); ++i) {
        if (static_cast<size_t>(i) >= file_record_.fields_.size()) {
          ret = OB_INVALID_DATA;
          break;
        }
        const bool missing = i >= static_cast<int64_t>(file_record_.fields_.size());
        const file_sql::CsvField *field = missing ? NULL : &file_record_.fields_[i];
        OZ(fill_file_datum(missing ? std::string() : field->value_,
                           missing || field->is_null_, MY_SPEC.column_exprs_.at(i)));
      }
      if (OB_SUCC(ret)) has_record = true;
    } while (OB_SUCC(ret) && !has_record);
    if (OB_SUCC(ret) && !has_record) ret = OB_ITER_END;
  } else if (OB_NOT_NULL(json_reader_)) {
    bool has_record = false;
    OZ(json_reader_->next(json_record_, has_record));
    if (OB_SUCC(ret) && !has_record) {
      ret = OB_ITER_END;
    } else if (OB_SUCC(ret)) {
      for (int64_t i = 0; i < MY_SPEC.column_exprs_.count(); ++i) {
        const file_sql::JsonField *field = NULL;
        for (const file_sql::JsonField &candidate : json_record_.fields_) {
          if (MY_SPEC.file_column_names_.count() > i
              && MY_SPEC.file_column_names_.at(i).case_compare(candidate.name_.c_str()) == 0) {
            field = &candidate;
            break;
          }
        }
        OZ(fill_file_datum(field == NULL ? std::string() : field->value_,
                           field == NULL || field->is_null_, MY_SPEC.column_exprs_.at(i)));
      }
    }
  } else if (OB_NOT_NULL(parquet_reader_)) {
    bool has_record = false;
    const int parquet_ret = parquet_reader_->next(parquet_record_, has_record);
    if (OB_FAIL(parquet_ret)) {
      ret = parquet_ret;
    }
    if (OB_SUCC(ret) && !has_record) {
      ret = OB_ITER_END;
    } else if (OB_SUCC(ret)) {
      if (parquet_record_.fields_.size() != static_cast<size_t>(MY_SPEC.column_exprs_.count())) {
        ret = OB_INVALID_DATA;
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < MY_SPEC.column_exprs_.count(); ++i) {
          const file_sql::ParquetField &field = parquet_record_.fields_[static_cast<size_t>(i)];
          OZ(fill_file_datum(field.value_, field.is_null_, MY_SPEC.column_exprs_.at(i)));
        }
      }
    }
  } else {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}


} // end namespace sql
} // end namespace oceanbase
