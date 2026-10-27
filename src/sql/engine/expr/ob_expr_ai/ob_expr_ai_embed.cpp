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
#include "ob_expr_ai_embed.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{
ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                        T_FUN_SYS_AI_EMBED, 
                        N_AI_EMBED, 
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL, 
                        NOT_ROW_DIMENSION) 
{
}

ObExprAIEmbed::~ObExprAIEmbed() 
{
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const 
{
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
    types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (param_num == 3) {
      if (ob_is_integer_type(types_stack[DIM_IDX].get_type())) {
        types_stack[DIM_IDX].set_calc_type(ObIntType);
        types_stack[DIM_IDX].set_precision(10);
        types_stack[DIM_IDX].set_scale(0);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    type.set_varchar();
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_COERCIBLE);
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_content = nullptr;
  ObDatum *arg_dim = nullptr;
  if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content, arg_dim))
                         : OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content))) {
  } else if (arg_model_id->is_null() || arg_content->is_null()) {
    // A null model id or a null content has no embedding, the null is
    // propagated to the result.
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString content = arg_content->get_string();
    if (model_id.empty() || content.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
      res.set_null();
    }
    int64_t dim = 0;
    ObJsonInt *dim_json = nullptr;
    ObJsonObject *config = nullptr;
    if (OB_FAIL(ret)) {
    } else if (OB_NOT_NULL(arg_dim)) {
      dim = arg_dim->get_int();
      if (dim <= 0) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
        res.set_null();
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, dim, dim_json))) {
      } else if (OB_FAIL(config->add("dimensions", dim_json))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } else {
      ObAIFuncModel model(temp_allocator, *info, *endpoint_info);
      ObString result;
      if (OB_FAIL(model.call_dense_embedding(content, config, result))) {
      } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, result))) {
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed_batch(const ObExpr &expr, ObEvalCtx &ctx,
                                       const ObBitVector &skip, const int64_t size)
{
  INIT_SUCC(ret);
  ObExpr *model_expr = expr.args_[MODEL_IDX];
  ObExpr *content_expr = expr.args_[CONTENT_IDX];
  ObExpr *dim_expr = expr.arg_cnt_ > DIM_IDX ? expr.args_[DIM_IDX] : nullptr;
  if (OB_FAIL(model_expr->eval_batch(ctx, skip, size))
      || OB_FAIL(content_expr->eval_batch(ctx, skip, size))
      || (OB_NOT_NULL(dim_expr) && OB_FAIL(dim_expr->eval_batch(ctx, skip, size)))) {
  } else {
    ObEvalCtx::BatchInfoScopeGuard batch_info_guard(ctx);
    batch_info_guard.set_batch_size(size);
    ObDatumVector res_datum = expr.locate_expr_datumvector(ctx);
    ObBitVector &eval_flags = expr.get_evaluated_flags(ctx);
    ObDatum *model_datums = model_expr->is_batch_result() ? model_expr->locate_batch_datums(ctx) : nullptr;
    ObDatum *content_datums = content_expr->is_batch_result() ? content_expr->locate_batch_datums(ctx) : nullptr;
    ObDatum *dim_datums = (OB_NOT_NULL(dim_expr) && dim_expr->is_batch_result()) ? dim_expr->locate_batch_datums(ctx) : nullptr;
    const ObDatum *const_model = model_expr->is_batch_result() ? nullptr : &model_expr->locate_expr_datum(ctx);
    const ObDatum *const_content = content_expr->is_batch_result() ? nullptr : &content_expr->locate_expr_datum(ctx);
    const ObDatum *const_dim = (OB_NOT_NULL(dim_expr) && !dim_expr->is_batch_result())
                              ? &dim_expr->locate_expr_datum(ctx) : nullptr;
    // Collect the rows that need an embedding, one input per row.
    ObArray<int64_t> row_indexes;
    ObArray<ObString> contents;
    ObString model_id;
    bool has_model_id = false;
    int64_t dimension = 0;
    bool has_dimension = false;
    bool need_row_by_row = false;
    for (int64_t i = 0; OB_SUCC(ret) && i < size; i++) {
      if (skip.at(i)) {
        eval_flags.set(i);
        continue;
      }
      const ObDatum *model_datum = OB_NOTNULL(model_datums) ? &model_datums[i] : const_model;
      const ObDatum *content_datum = OB_NOTNULL(content_datums) ? &content_datums[i] : const_content;
      const ObDatum *dim_datum = OB_NOTNULL(dim_datums) ? &dim_datums[i] : const_dim;
      if (OB_ISNULL(model_datum) || OB_ISNULL(content_datum)
          || model_datum->is_null() || content_datum->is_null()) {
        // A null model id or a null content has no embedding, the null is
        // propagated to the result.
        res_datum.at(i)->set_null();
        eval_flags.set(i);
        expr.get_eval_info(ctx).notnull_ = false;
        continue;
      }
      ObString row_model_id = model_datum->get_string();
      ObString row_content = content_datum->get_string();
      if (row_model_id.empty() || row_content.empty()) {
        res_datum.at(i)->set_null();
        eval_flags.set(i);
        expr.get_eval_info(ctx).notnull_ = false;
        continue;
      }
      if (!has_model_id) {
        model_id = row_model_id;
        has_model_id = true;
      } else if (model_id != row_model_id) {
        // The model is not the same for every row, one request per row it is.
        need_row_by_row = true;
        break;
      }
      if (OB_NOTNULL(dim_datum)) {
        if (dim_datum->is_null() || dim_datum->get_int() <= 0) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
          break;
        }
        if (!has_dimension) {
          dimension = dim_datum->get_int();
          has_dimension = true;
        } else if (dimension != dim_datum->get_int()) {
          need_row_by_row = true;
          break;
        }
      }
      if (OB_FAIL(row_indexes.push_back(i)) || OB_FAIL(contents.push_back(row_content))) {
      }
    }

    if (OB_FAIL(ret)) {
    } else if (need_row_by_row) {
      ret = expr_default_eval_batch_func(expr, ctx, skip, size);
    } else if (row_indexes.count() == 0) {
      // Every row was skipped or null, there is nothing to ask the model for.
    } else {
      ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
      MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
      lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
      ObAIFuncExprInfo *info = nullptr;
      share::ObAiModelEndpointInfo resolved_endpoint;
      const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
      query::ObIAiEndpointResolver *endpoint_resolver =
          ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
      ObJsonObject *config = nullptr;
      ObArray<ObString> results;
      if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
      } else if (OB_ISNULL(endpoint_resolver)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                     model_id, temp_allocator, resolved_endpoint))) {
      } else if (has_dimension) {
        ObJsonInt *dim_json = nullptr;
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
        } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, dimension, dim_json))) {
        } else if (OB_FAIL(config->add("dimensions", dim_json))) {
        }
      }
      if (OB_SUCC(ret)) {
        ObAIFuncModel model(temp_allocator, *info, *endpoint_info);
        if (OB_FAIL(model.call_dense_embedding_vector_v2(contents, config, results))) {
        } else if (results.count() != row_indexes.count()) {
          ret = OB_ERR_UNEXPECTED;
          LOG_USER_ERROR(OB_ERR_UNEXPECTED, "ai_embed, the model returned a different number of embeddings than requested");
        } else {
          // Give every row the embedding that belongs to its own input.
          for (int64_t i = 0; OB_SUCC(ret) && i < row_indexes.count(); i++) {
            if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, *res_datum.at(row_indexes[i]), results[i]))) {
            } else {
              eval_flags.set(row_indexes[i]);
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                           const ObRawExpr &raw_expr,
                           ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   ObObj const_data;
  //   bool got_data = false;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //     LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //   } else {
  //     rt_expr.extra_info_ = info;
  //   }
  // }

  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
    rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_ai_embed_batch;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
