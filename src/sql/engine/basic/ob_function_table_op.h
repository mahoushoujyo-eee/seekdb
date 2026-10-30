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

#ifndef OCEANBASE_BASIC_OB_FUNCTION_TABLE_OP_H_
#define OCEANBASE_BASIC_OB_FUNCTION_TABLE_OP_H_

#include "sql/engine/ob_operator.h"
#include "sql/engine/basic/ob_chunk_datum_store.h"
#include "lib/charset/ob_charset.h"
#include "sql/engine/basic/ob_file_scan_utils.h"
#include <vector>

namespace oceanbase
{
namespace sql
{

class ObExpr;
class ObFunctionTableSpec : public ObOpSpec
{
OB_UNIS_VERSION_V(1);
public:
  ObFunctionTableSpec(common::ObIAllocator &alloc, const ObPhyOperatorType type)
    : ObOpSpec(alloc, type), value_expr_(nullptr), column_exprs_(alloc), has_correlated_expr_(false),
      is_file_table_(false), file_func_name_(), file_path_()
      , file_column_names_(alloc), file_csv_header_(true), file_csv_delimiter_(',')
      , file_csv_quote_('"'), file_csv_escape_('"'), file_csv_null_string_("NULL")
  {}
  ObExpr *value_expr_;
  common::ObFixedArray<ObExpr*, common::ObIAllocator> column_exprs_;
  bool has_correlated_expr_;
  bool is_file_table_;
  common::ObString file_func_name_;
  common::ObString file_path_;
  common::ObFixedArray<common::ObString, common::ObIAllocator> file_column_names_;
  bool file_csv_header_;
  char file_csv_delimiter_;
  char file_csv_quote_;
  char file_csv_escape_;
  common::ObString file_csv_null_string_;
};

class ObFunctionTableOp : public ObOperator
{
public:
  ObFunctionTableOp(ObExecContext &exec_ctx, const ObOpSpec &spec, ObOpInput *input)
    : ObOperator(exec_ctx, spec, input),
    node_idx_(0),
    already_calc_(false),
    row_count_(0),
    col_count_(0),
    value_table_(NULL) 
    , file_reader_(NULL)
    , json_reader_(NULL)
    , parquet_reader_(NULL)
    , next_file_idx_(0)
    , file_record_ready_(false)
    , file_has_record_(false)
    , file_header_skipped_(false)
  {}

  virtual int inner_open() override;
  virtual int inner_rescan() override;
  virtual int inner_get_next_row() override;
  //virtual int inner_get_next_batch(int64_t max_row_cnt) override;
  virtual int inner_close() override;
  virtual void destroy() override;
private:
  int inner_get_next_row_udf();
  int inner_get_next_row_sys_func();
  int inner_get_next_row_file();
  int open_file_reader();
  int fill_file_datum(const std::string &value, const bool is_null, ObExpr *expr);
  int get_current_result(common::ObObj &result);
  int64_t node_idx_;
  bool already_calc_;
  int64_t row_count_;
  int64_t col_count_;
  common::ObObj value_;
  pl::ObPLCollection *value_table_;
  int (ObFunctionTableOp::*next_row_func_)();
  file_sql::CsvReader *file_reader_;
  file_sql::JsonlReader *json_reader_;
  file_sql::ParquetReader *parquet_reader_;
  file_sql::CsvReader file_reader_obj_;
  file_sql::JsonlReader json_reader_obj_;
  file_sql::ParquetReader parquet_reader_obj_;
  std::vector<std::string> file_paths_;
  int64_t next_file_idx_;
  file_sql::CsvRecord file_record_;
  file_sql::JsonRecord json_record_;
  file_sql::ParquetRecord parquet_record_;
  bool file_record_ready_;
  bool file_has_record_;
  bool file_header_skipped_;
};

} // end namespace sql
} // end namespace oceanbase

#endif /* OCEANBASE_BASIC_OB_FUNCTION_TABLE_OP_H_ */
