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

#define USING_LOG_PREFIX SERVER_OMT
#include "ob_ai_service.h"
#include "observer/ai_service/ob_ai_service_executor.h"
#include "observer/ob_inner_sql_connection.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "sql/privilege_check/ob_ai_model_priv_util.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/engine/ob_exec_context.h"
#include <strings.h>

using namespace oceanbase::share;
using namespace oceanbase::common;
namespace oceanbase
{
namespace omt
{

namespace
{
const int64_t AI_COLUMN_WORKER_INTERVAL_US = 100 * 1000;
const int64_t AI_COLUMN_WORKER_BATCH_SIZE = 128;

int append_ai_sql_string(ObSqlString &sql, const ObString &value)
{
  int ret = sql.append("'");
  for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    ret = value.ptr()[i] == '\'' ? sql.append("''") : sql.append_fmt("%c", value.ptr()[i]);
  }
  if (OB_SUCC(ret)) {
    ret = sql.append("'");
  }
  return ret;
}

int append_ai_sql_identifier(ObSqlString &sql, const ObString &identifier)
{
  int ret = sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < identifier.length(); ++i) {
    ret = identifier.ptr()[i] == '`' ? sql.append("``") : sql.append_fmt("%c", identifier.ptr()[i]);
  }
  if (OB_SUCC(ret)) {
    ret = sql.append("`");
  }
  return ret;
}

int append_ai_hidden_column(ObSqlString &sql, const uint64_t column_id, const char *suffix)
{
  return sql.append_fmt("`__ai_%lu_%s`", static_cast<unsigned long>(column_id), suffix);
}

int copy_ai_string(ObIAllocator &allocator, const ObString &source, ObString &destination)
{
  int ret = OB_SUCCESS;
  char *buf = nullptr;
  if (source.empty()) {
    destination.reset();
  } else if (OB_ISNULL(buf = static_cast<char *>(allocator.alloc(source.length())))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    MEMCPY(buf, source.ptr(), source.length());
    destination.assign_ptr(buf, source.length());
  }
  return ret;
}

ObString get_ai_expression(const ObString &normalized_expr)
{
  static const char *const JSON_PREFIX = "Return exactly one valid JSON value. ";
  static const char *const INTEGER_PREFIX = "Return a base-10 integer. ";
  static const char *const NUMBER_PREFIX = "Return a base-10 number. ";
  const char *prefix = nullptr;
  int64_t prefix_len = 0;
  const char *prefixes[] = {JSON_PREFIX, INTEGER_PREFIX, NUMBER_PREFIX};
  for (int64_t i = 0; nullptr == prefix && i < ARRAYSIZEOF(prefixes); ++i) {
    const int64_t candidate_len = static_cast<int64_t>(strlen(prefixes[i]));
    if (normalized_expr.length() >= candidate_len
        && 0 == strncasecmp(normalized_expr.ptr(), prefixes[i], candidate_len)) {
      prefix = prefixes[i];
      prefix_len = candidate_len;
    }
  }
  return nullptr == prefix ? normalized_expr
                           : ObString(normalized_expr.length() - prefix_len,
                                      normalized_expr.ptr() + prefix_len);
}

int append_ai_expression(ObSqlString &sql, const ObString &normalized_expr)
{
  static const char *const JSON_PREFIX = "Return exactly one valid JSON value. ";
  static const char *const INTEGER_PREFIX = "Return a base-10 integer. ";
  static const char *const NUMBER_PREFIX = "Return a base-10 number. ";
  const char *system_prompt = nullptr;
  int64_t system_prompt_len = 0;
  const char *prefixes[] = {JSON_PREFIX, INTEGER_PREFIX, NUMBER_PREFIX};
  for (int64_t i = 0; nullptr == system_prompt && i < ARRAYSIZEOF(prefixes); ++i) {
    const int64_t candidate_len = static_cast<int64_t>(strlen(prefixes[i]));
    if (normalized_expr.length() >= candidate_len
        && 0 == strncasecmp(normalized_expr.ptr(), prefixes[i], candidate_len)) {
      system_prompt = prefixes[i];
      system_prompt_len = candidate_len;
    }
  }
  if (nullptr == system_prompt) {
    return sql.append_fmt("%.*s", normalized_expr.length(), normalized_expr.ptr());
  }

  const ObString expression(normalized_expr.length() - system_prompt_len,
                            normalized_expr.ptr() + system_prompt_len);
  int64_t func_pos = 0;
  while (func_pos < expression.length() && isspace(expression.ptr()[func_pos])) {
    ++func_pos;
  }
  if (expression.length() - func_pos < 11
      || 0 != strncasecmp(expression.ptr() + func_pos, "AI_COMPLETE", 11)) {
    return sql.append_fmt("%.*s", expression.length(), expression.ptr());
  }
  int64_t open_pos = -1;
  for (int64_t i = 0; i < expression.length(); ++i) {
    if (expression.ptr()[i] == '(') {
      open_pos = i;
      break;
    }
  }
  int64_t close_pos = -1;
  int64_t depth = 0;
  int64_t comma_count = 0;
  int64_t second_comma = -1;
  char quote_char = '\0';
  for (int64_t i = open_pos; open_pos >= 0 && i < expression.length(); ++i) {
    const char c = expression.ptr()[i];
    if (quote_char != '\0') {
      if (c == '\\' && i + 1 < expression.length()) {
        ++i;
      } else if (c == quote_char && i + 1 < expression.length()
                 && expression.ptr()[i + 1] == quote_char) {
        ++i;
      } else if (c == quote_char) {
        quote_char = '\0';
      }
    } else if (c == '\'' || c == '"' || c == '`') {
      quote_char = c;
    } else if (c == '(') {
      ++depth;
    } else if (c == ')') {
      if (--depth == 0) {
        close_pos = i;
        break;
      }
    } else if (depth == 1 && c == ',') {
      if (comma_count++ != 0 && comma_count == 2) {
        second_comma = i;
      }
    }
  }
  if (open_pos < 0 || close_pos < 0 || comma_count < 1 || comma_count > 2) {
    // The expression will be validated by the SQL parser.  Do not silently
    // change malformed or non-call expressions while adding the contract.
    return sql.append_fmt("%.*s", expression.length(), expression.ptr());
  }

  int ret = OB_SUCCESS;
  if (comma_count == 1) {
    if (OB_FAIL(sql.append_fmt("%.*s, JSON_OBJECT('system_prompt', ", close_pos, expression.ptr()))
        || OB_FAIL(append_ai_sql_string(sql, ObString(system_prompt_len, system_prompt)))
        || OB_FAIL(sql.append_fmt(")%.*s", expression.length() - close_pos, expression.ptr() + close_pos))) {
    }
  } else {
    if (OB_FAIL(sql.append_fmt("%.*sJSON_MERGE_PATCH(%.*s, JSON_OBJECT('system_prompt', ",
                               second_comma + 1, expression.ptr(),
                               close_pos - second_comma - 1, expression.ptr() + second_comma + 1))
        || OB_FAIL(append_ai_sql_string(sql, ObString(system_prompt_len, system_prompt)))
        || OB_FAIL(sql.append_fmt(")"))
        || OB_FAIL(sql.append_fmt(")%.*s", expression.length() - close_pos,
                                  expression.ptr() + close_pos))) {
    }
  }
  return ret;
}
}

ObAiService::ObAiService()
  : is_inited_(false),
    is_processing_(0)
{
}

ObAiService::~ObAiService()
{
  destroy();
}

int ObAiService::server_module_init(ObAiService* &ai_service)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(ai_service)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ai_service->init())) {
  }
  return ret;
}

int ObAiService::init()
{
  int ret = OB_SUCCESS;
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
  } else if (OB_FAIL(timer_.init("AiColumnWorker", ObMemAttr("AiColumnWorker")))) {
  } else {
    is_inited_ = true;
  }
  return ret;
}

int ObAiService::start()
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else if (!timer_.task_exist(*this)
             && OB_FAIL(timer_.schedule(*this, AI_COLUMN_WORKER_INTERVAL_US, true))) {
    LOG_WARN("failed to schedule AI column worker", K(ret));
  } else {
    LOG_INFO("AI column worker started");
  }
  return ret;
}

void ObAiService::stop()
{
  if (timer_.inited() && timer_.task_exist(*this)) {
    int ret = OB_SUCCESS;
    if (OB_FAIL(timer_.cancel_task(*this))) {
      LOG_WARN("failed to cancel AI column worker", K(ret));
    }
  }
}

void ObAiService::wait()
{
  if (timer_.inited()) {
    int ret = OB_SUCCESS;
    if (OB_FAIL(timer_.wait_task(*this))) {
      LOG_WARN("failed to wait AI column worker", K(ret));
    }
  }
}

void ObAiService::destroy()
{
  stop();
  wait();
  if (timer_.inited()) {
    timer_.destroy();
  }
  is_inited_ = false;
}

void ObAiService::runTimerTask()
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else if (!ATOMIC_BCAS(&is_processing_, 0, 1)) {
    return;
  } else if (OB_FAIL(process_tasks_())) {
    LOG_WARN("failed to process AI column tasks", K(ret));
  }
  ATOMIC_STORE(&is_processing_, 0);
}

int ObAiService::process_tasks_()
{
  int ret = OB_SUCCESS;
  common::sqlclient::ObISQLConnectionGuard conn_guard;
  observer::ObInnerSQLConnection *conn = nullptr;
  ObArenaAllocator allocator("AiColumnWorker");
  ObSEArray<AiColumnTaskInfo, 128> tasks;
  ObSqlString select_sql;
  common::ObISQLClient::ReadResult result;
  common::sqlclient::ObMySQLResult *mysql_result = nullptr;
  if (OB_FAIL(observer::ObInnerSQLConnection::create_connection_with_owned_session(false, 0, conn_guard))) {
  } else if (OB_ISNULL(conn = static_cast<observer::ObInnerSQLConnection *>(conn_guard.get_ptr()))) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(select_sql.append_fmt(
      "select task.task_id, task.table_id, task.column_id, task.request_id, task.rowkey_predicate, "
      "task.retry_count, ai.max_retries, d.database_name, t.table_name, ai.column_name, "
      "ai.normalized_expr, ai.state_column_id, ai.request_column_id "
      "from oceanbase.__all_ai_column_task task "
      "join oceanbase.__all_ai_column ai on ai.table_id = task.table_id and ai.column_id = task.column_id "
      "join oceanbase.__all_table t on t.table_id = task.table_id "
      "join oceanbase.__all_database d on d.database_id = t.database_id "
      "where task.eligible_ts <= now(6) and task.state in (0, 3) "
      "order by task.table_id, task.request_id, task.task_id limit %ld",
      static_cast<long>(AI_COLUMN_WORKER_BATCH_SIZE)))) {
  } else if (OB_FAIL(conn->execute_read(select_sql.string(), result))) {
  } else if (OB_ISNULL(mysql_result = result.get_result())) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    int row_ret = OB_SUCCESS;
    while (OB_SUCC(ret) && OB_SUCCESS == (row_ret = mysql_result->next())) {
      AiColumnTaskInfo task = {};
      ObString request_id;
      ObString predicate;
      ObString database_name;
      ObString table_name;
      ObString column_name;
      ObString expression;
      int64_t task_id = 0;
      int64_t table_id = 0;
      int64_t column_id = 0;
      int64_t state_column_id = 0;
      int64_t request_column_id = 0;
      if (OB_FAIL(mysql_result->get_int(static_cast<int64_t>(0), task_id))) {
      } else if (OB_FAIL(mysql_result->get_int(1, table_id))) {
      } else if (OB_FAIL(mysql_result->get_int(2, column_id))) {
      } else if (OB_FAIL(mysql_result->get_varchar(3, request_id))) {
      } else if (OB_FAIL(mysql_result->get_varchar(4, predicate))) {
      } else if (OB_FAIL(mysql_result->get_int(5, task.retry_count_))) {
      } else if (OB_FAIL(mysql_result->get_int(6, task.max_retries_))) {
      } else if (OB_FAIL(mysql_result->get_varchar(7, database_name))) {
      } else if (OB_FAIL(mysql_result->get_varchar(8, table_name))) {
      } else if (OB_FAIL(mysql_result->get_varchar(9, column_name))) {
      } else if (OB_FAIL(mysql_result->get_varchar(10, expression))) {
      } else if (OB_FAIL(mysql_result->get_int(11, state_column_id))) {
      } else if (OB_FAIL(mysql_result->get_int(12, request_column_id))) {
      } else if (task_id < 0 || table_id < 0 || column_id < 0
                 || state_column_id < 0 || request_column_id < 0) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        task.task_id_ = static_cast<uint64_t>(task_id);
        task.table_id_ = static_cast<uint64_t>(table_id);
        task.column_id_ = static_cast<uint64_t>(column_id);
        task.state_column_id_ = static_cast<uint64_t>(state_column_id);
        task.request_column_id_ = static_cast<uint64_t>(request_column_id);
      }
      if (OB_SUCC(ret)) {
        if (OB_FAIL(copy_ai_string(allocator, request_id, task.request_id_))) {
        }
      }
      if (OB_SUCC(ret)) {
        if (OB_FAIL(copy_ai_string(allocator, predicate, task.rowkey_predicate_))) {
        }
      }
      if (OB_SUCC(ret)) {
        if (OB_FAIL(copy_ai_string(allocator, database_name, task.database_name_))) {
        }
      }
      if (OB_SUCC(ret)) {
        if (OB_FAIL(copy_ai_string(allocator, table_name, task.table_name_))) {
        }
      }
      if (OB_SUCC(ret)) {
        if (OB_FAIL(copy_ai_string(allocator, column_name, task.column_name_))) {
        }
      }
      if (OB_SUCC(ret)) {
        if (OB_FAIL(copy_ai_string(allocator, expression, task.expression_))) {
        }
      }
      if (OB_SUCC(ret)) {
        if (OB_FAIL(tasks.push_back(task))) {
        }
      }
    }
    if (OB_ITER_END == row_ret) {
      row_ret = OB_SUCCESS;
    }
    if (OB_SUCC(ret) && OB_FAIL(row_ret)) {
      ret = row_ret;
    }
  }
  if (OB_NOT_NULL(mysql_result)) {
    const int close_ret = result.close();
    if (OB_SUCCESS == ret && OB_SUCCESS != close_ret) {
      ret = close_ret;
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < tasks.count();) {
    int64_t end_idx = i + 1;
    while (end_idx < tasks.count()
           && tasks.at(i).table_id_ == tasks.at(end_idx).table_id_
           && tasks.at(i).request_id_.compare(tasks.at(end_idx).request_id_) == 0) {
      ++end_idx;
    }
    if (OB_FAIL(process_task_group_(tasks, i, end_idx))) {
    }
    i = end_idx;
  }
  return ret;
}

int ObAiService::process_task_group_(const common::ObIArray<AiColumnTaskInfo> &tasks,
                                    const int64_t begin_idx,
                                    const int64_t end_idx)
{
  int ret = OB_SUCCESS;
  int ai_ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  bool can_retry = true;
  const AiColumnTaskInfo &first = tasks.at(begin_idx);
  for (int64_t i = begin_idx; i < end_idx; ++i) {
    const AiColumnTaskInfo &task = tasks.at(i);
    const ObString expression = get_ai_expression(task.expression_);
    can_retry = can_retry && task.retry_count_ < task.max_retries_;
    int64_t expr_pos = 0;
    while (expr_pos < expression.length() && isspace(expression.ptr()[expr_pos])) {
      ++expr_pos;
    }
    const int64_t remain = expression.length() - expr_pos;
    if (remain > 16384 || remain < 9
        || (0 != strncasecmp(expression.ptr() + expr_pos, "AI_COMPLETE(", MIN(remain, 12))
            && 0 != strncasecmp(expression.ptr() + expr_pos, "AI_EMBED(", MIN(remain, 9)))) {
      ai_ret = OB_INVALID_ARGUMENT;
      break;
    }
  }
  common::sqlclient::ObISQLConnectionGuard conn_guard;
  observer::ObInnerSQLConnection *conn = nullptr;
  if (OB_FAIL(observer::ObInnerSQLConnection::create_connection_with_owned_session(false, 0, conn_guard))) {
  } else if (OB_ISNULL(conn = static_cast<observer::ObInnerSQLConnection *>(conn_guard.get_ptr()))) {
    ret = OB_ERR_UNEXPECTED;
  }

  ObSqlString update_sql;
  if (OB_SUCC(ret) && OB_SUCCESS == ai_ret) {
    if (OB_FAIL(update_sql.append("update "))
        || OB_FAIL(append_ai_sql_identifier(update_sql, first.database_name_))
        || OB_FAIL(update_sql.append("."))
        || OB_FAIL(append_ai_sql_identifier(update_sql, first.table_name_))
        || OB_FAIL(update_sql.append(" set "))) {
    }
    for (int64_t i = begin_idx; OB_SUCC(ret) && i < end_idx; ++i) {
      const AiColumnTaskInfo &task = tasks.at(i);
      if (i != begin_idx && OB_FAIL(update_sql.append(", "))) {
      } else if (OB_FAIL(append_ai_sql_identifier(update_sql, task.column_name_))) {
      } else if (OB_FAIL(update_sql.append(" = "))
                 || OB_FAIL(append_ai_expression(update_sql, task.expression_))
                 || OB_FAIL(update_sql.append(", "))) {
      } else if (OB_FAIL(append_ai_hidden_column(update_sql, task.column_id_, "state"))) {
      } else if (OB_FAIL(update_sql.append(" = 2, "))) {
      } else if (OB_FAIL(append_ai_hidden_column(update_sql, task.column_id_, "request"))) {
      } else if (OB_FAIL(update_sql.append(" = ")) || OB_FAIL(append_ai_sql_string(update_sql, first.request_id_))) {
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(update_sql.append_fmt(" where (%.*s)", first.rowkey_predicate_.length(), first.rowkey_predicate_.ptr()))) {
      }
    }
    for (int64_t i = begin_idx; OB_SUCC(ret) && i < end_idx; ++i) {
      if (OB_FAIL(update_sql.append(" and "))
          || OB_FAIL(append_ai_hidden_column(update_sql, tasks.at(i).column_id_, "request"))
          || OB_FAIL(update_sql.append(" = "))
          || OB_FAIL(append_ai_sql_string(update_sql, first.request_id_))) {
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(conn->execute_write(update_sql.string(), affected_rows))) {
      ai_ret = ret;
      ret = OB_SUCCESS;
    }
  }

  if (OB_SUCC(ret) && OB_SUCCESS == ai_ret) {
    ObSqlString delete_sql;
    if (OB_FAIL(delete_sql.append_fmt(
            "delete from oceanbase.__all_ai_column_task where table_id = %lu and request_id = ",
            static_cast<unsigned long>(first.table_id_)))
        || OB_FAIL(append_ai_sql_string(delete_sql, first.request_id_))) {
    } else if (OB_FAIL(conn->execute_write(delete_sql.string(), affected_rows))) {
    }
  } else if (OB_SUCC(ret)) {
    const ObString error_text(static_cast<int32_t>(strlen(ob_errpkt_strerror(ai_ret))), ob_errpkt_strerror(ai_ret));
    if (can_retry) {
      ObSqlString retry_sql;
      if (OB_FAIL(retry_sql.append(
              "update oceanbase.__all_ai_column_task set state = 3, retry_count = retry_count + 1, "
              "eligible_ts = date_add(now(6), interval 1 second), last_error = "))
          || OB_FAIL(append_ai_sql_string(retry_sql, error_text))
          || OB_FAIL(retry_sql.append_fmt(" where table_id = %lu and request_id = ",
                                          static_cast<unsigned long>(first.table_id_)))
          || OB_FAIL(append_ai_sql_string(retry_sql, first.request_id_))) {
      } else if (OB_FAIL(conn->execute_write(retry_sql.string(), affected_rows))) {
      }
    } else {
      ObSqlString state_sql;
      if (OB_FAIL(state_sql.append("update "))
          || OB_FAIL(append_ai_sql_identifier(state_sql, first.database_name_))
          || OB_FAIL(state_sql.append("."))
          || OB_FAIL(append_ai_sql_identifier(state_sql, first.table_name_))
          || OB_FAIL(state_sql.append(" set "))) {
      }
      for (int64_t i = begin_idx; OB_SUCC(ret) && i < end_idx; ++i) {
        if (i != begin_idx && OB_FAIL(state_sql.append(", "))) {
        } else if (OB_FAIL(append_ai_hidden_column(state_sql, tasks.at(i).column_id_, "state"))) {
        } else if (OB_FAIL(state_sql.append(" = 3"))) {
        }
      }
      if (OB_SUCC(ret)) {
        if (OB_FAIL(state_sql.append_fmt(" where (%.*s)", first.rowkey_predicate_.length(), first.rowkey_predicate_.ptr()))) {
        }
      }
      for (int64_t i = begin_idx; OB_SUCC(ret) && i < end_idx; ++i) {
        if (OB_FAIL(state_sql.append(" and "))
            || OB_FAIL(append_ai_hidden_column(state_sql, tasks.at(i).column_id_, "request"))
            || OB_FAIL(state_sql.append(" = "))
            || OB_FAIL(append_ai_sql_string(state_sql, first.request_id_))) {
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(conn->execute_write(state_sql.string(), affected_rows))) {
      }
      if (OB_SUCC(ret)) {
        ObSqlString delete_sql;
        if (OB_FAIL(delete_sql.append_fmt(
                "delete from oceanbase.__all_ai_column_task where table_id = %lu and request_id = ",
                static_cast<unsigned long>(first.table_id_)))
            || OB_FAIL(append_ai_sql_string(delete_sql, first.request_id_))) {
        } else if (OB_FAIL(conn->execute_write(delete_sql.string(), affected_rows))) {
        }
      }
    }
  }
  return ret;
}

int ObAiService::get_ai_service_guard(ObAiServiceGuard &ai_service_guard)
{
  // TODO: implement after cache support
  int ret = OB_SUCCESS;
  return ret;
}

ObAiServiceGuard::ObAiServiceGuard()
  : local_allocator_(ObMemAttr("AiServiceGuard", ObCtxIds::DEFAULT_CTX_ID))
{
}

ObAiServiceGuard::~ObAiServiceGuard()
{
}

int ObAiServiceGuard::check_access_privilege()
{
  int ret = OB_SUCCESS;
  bool has_priv = false;

  sql::ObSQLSessionInfo *session = nullptr;
  if (OB_ISNULL(session = THIS_WORKER.get_session())) {
    // system background task thread do not have session, skip check
  } else {
    ObArenaAllocator tmp_allocator;
    share::schema::ObSchemaGetterGuard *schema_guard = session->get_cur_exec_ctx()->get_sql_ctx()->schema_guard_;
    if (OB_ISNULL(schema_guard)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      sql::ObAIServiceEndpointPrivUtil priv_util(*schema_guard);
      share::schema::ObSessionPrivInfo session_priv;
      uint64_t user_id = session->get_priv_user_id();
      if (user_id == OB_INVALID_ID) {
        user_id = OB_SYS_USER_ID;
      }
      if (OB_FAIL(schema_guard->get_session_priv_info(
                                                    user_id,
                                                    session->get_database_name(),
                                                    session_priv))) {
      } else if (OB_FAIL(priv_util.check_access_ai_model_priv(tmp_allocator, session_priv, has_priv))) {
      } else if (!has_priv) {
        ret = OB_ERR_NO_PRIVILEGE;
        LOG_WARN("no privilege for ai model access", K(ret));
        LOG_USER_ERROR(OB_ERR_NO_PRIVILEGE, "access ai model");
      }
    }
  }

  return ret;
}

int ObAiServiceGuard::get_ai_endpoint(const common::ObString &name, const share::ObAiModelEndpointInfo *&endpoint_info)
{
  int ret = OB_SUCCESS;
  ObAiModelEndpointInfo *tmp_endpoint_info = nullptr;

  if (OB_FAIL(check_access_privilege())) {
  } else if (OB_ISNULL(tmp_endpoint_info = OB_NEWx(ObAiModelEndpointInfo, &local_allocator_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(ObAiServiceExecutor::read_ai_endpoint(local_allocator_, name, *tmp_endpoint_info))) {
  } else {
    endpoint_info = tmp_endpoint_info;
  }
  return ret;
}

int ObAiServiceGuard::get_ai_endpoint_by_ai_model_name(const common::ObString &ai_model_name, const share::ObAiModelEndpointInfo *&endpoint_info, bool need_check)
{
  int ret = OB_SUCCESS;
  ObAiModelEndpointInfo *tmp_endpoint_info = nullptr;

  if (need_check && OB_FAIL(check_access_privilege())) {
    LOG_WARN("failed to check access privilege", K(ret));
  } else if (OB_ISNULL(tmp_endpoint_info = OB_NEWx(ObAiModelEndpointInfo, &local_allocator_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(ObAiServiceExecutor::read_ai_endpoint_by_ai_model_name(local_allocator_, ai_model_name, *tmp_endpoint_info))) {
  } else {
    endpoint_info = tmp_endpoint_info;
  }
  return ret;
}

} // namespace omt
} // namespace oceanbase
