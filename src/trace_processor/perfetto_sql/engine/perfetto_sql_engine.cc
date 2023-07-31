/*
 * Copyright (C) 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h"

#include <memory>
#include <optional>
#include <string>
#include <variant>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/perfetto_sql/engine/created_function.h"
#include "src/trace_processor/perfetto_sql/engine/created_table_function.h"
#include "src/trace_processor/perfetto_sql/engine/function_util.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_parser.h"
#include "src/trace_processor/sqlite/db_sqlite_table.h"
#include "src/trace_processor/sqlite/scoped_db.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/sqlite/sqlite_engine.h"
#include "src/trace_processor/tp_metatrace.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto {
namespace trace_processor {
namespace {

void IncrementCountForStmt(const SqliteEngine::PreparedStatement& p_stmt,
                           PerfettoSqlEngine::ExecutionStats* res) {
  res->statement_count++;

  // If the stmt is already done, it clearly didn't have any output.
  if (p_stmt.IsDone())
    return;

  sqlite3_stmt* stmt = p_stmt.sqlite_stmt();
  if (sqlite3_column_count(stmt) == 1) {
    sqlite3_value* value = sqlite3_column_value(stmt, 0);

    // If the "VOID" pointer associated to the return value is not null,
    // that means this is a function which is forced to return a value
    // (because all functions in SQLite have to) but doesn't actually
    // wait to (i.e. it wants to be treated like CREATE TABLE or similar).
    // Because of this, ignore the return value of this function.
    // See |WrapSqlFunction| for where this is set.
    if (sqlite3_value_pointer(value, "VOID") != nullptr) {
      return;
    }

    // If the statement only has a single column and that column is named
    // "suppress_query_output", treat it as a statement without output for
    // accounting purposes. This allows an escape hatch for cases where the
    // user explicitly wants to ignore functions as having output.
    if (strcmp(sqlite3_column_name(stmt, 0), "suppress_query_output") == 0) {
      return;
    }
  }

  // Otherwise, the statement has output and so increment the count.
  res->statement_count_with_output++;
}

base::Status AddTracebackIfNeeded(base::Status status,
                                  const SqlSource& source) {
  if (status.ok()) {
    return status;
  }
  if (status.GetPayload("perfetto.dev/has_traceback") == "true") {
    return status;
  }
  std::string traceback = source.AsTraceback(std::nullopt);
  status = base::ErrStatus("%s%s", traceback.c_str(), status.c_message());
  status.SetPayload("perfetto.dev/has_traceback", "true");
  return status;
}

}  // namespace

PerfettoSqlEngine::PerfettoSqlEngine(StringPool* pool)
    : query_cache_(new QueryCache()), pool_(pool), engine_(new SqliteEngine()) {
  engine_->RegisterVirtualTableModule<CreatedTableFunction>(
      "created_table_function", this, SqliteTable::TableType::kExplicitCreate,
      false);
}

PerfettoSqlEngine::~PerfettoSqlEngine() {
  // Destroying the sqlite engine should also destroy all the created table
  // functions.
  engine_.reset();
  PERFETTO_CHECK(created_table_function_state_.size() == 0);
}

void PerfettoSqlEngine::RegisterTable(const Table& table,
                                      const std::string& table_name) {
  DbSqliteTable::Context context{
      query_cache_.get(), DbSqliteTable::TableComputation::kStatic, &table,
      /*sql_table=*/nullptr, /*generator=*/nullptr};
  engine_->RegisterVirtualTableModule<DbSqliteTable>(
      table_name, std::move(context), SqliteTable::kEponymousOnly, false);

  // Register virtual tables into an internal 'perfetto_tables' table.
  // This is used for iterating through all the tables during a database
  // export.
  char* insert_sql = sqlite3_mprintf(
      "INSERT INTO perfetto_tables(name) VALUES('%q')", table_name.c_str());
  char* error = nullptr;
  sqlite3_exec(engine_->db(), insert_sql, nullptr, nullptr, &error);
  sqlite3_free(insert_sql);
  if (error) {
    PERFETTO_ELOG("Error adding table to perfetto_tables: %s", error);
    sqlite3_free(error);
  }
}

void PerfettoSqlEngine::RegisterTableFunction(
    std::unique_ptr<TableFunction> fn) {
  std::string table_name = fn->TableName();
  DbSqliteTable::Context context{
      query_cache_.get(), DbSqliteTable::TableComputation::kTableFunction,
      /*static_table=*/nullptr, /*sql_table=*/nullptr, std::move(fn)};
  engine_->RegisterVirtualTableModule<DbSqliteTable>(
      table_name, std::move(context), SqliteTable::kEponymousOnly, false);
}

base::StatusOr<PerfettoSqlEngine::ExecutionStats> PerfettoSqlEngine::Execute(
    SqlSource sql) {
  auto res = ExecuteUntilLastStatement(std::move(sql));
  RETURN_IF_ERROR(res.status());
  if (res->stmt.IsDone()) {
    return res->stats;
  }
  while (res->stmt.Step()) {
  }
  RETURN_IF_ERROR(res->stmt.status());
  return res->stats;
}

base::StatusOr<PerfettoSqlEngine::ExecutionResult>
PerfettoSqlEngine::ExecuteUntilLastStatement(SqlSource sql_source) {
  // A SQL string can contain several statements. Some of them might be comment
  // only, e.g. "SELECT 1; /* comment */; SELECT 2;". Some statements can also
  // be PerfettoSQL statements which we need to transpile before execution or
  // execute without delegating to SQLite.
  //
  // The logic here is the following:
  //  - We parse the statement as a PerfettoSQL statement.
  //  - If the statement is something we can execute, execute it instantly and
  //    prepare a dummy SQLite statement so the rest of the code continues to
  //    work correctly.
  //  - If the statement is actually an SQLite statement, we invoke PrepareStmt.
  //  - We step once to make sure side effects take effect (e.g. for CREATE
  //    TABLE statements, tables are created).
  //  - If we encounter a valid statement afterwards, we step internally through
  //    all rows of the previous one. This ensures that any further side effects
  //    take hold *before* we step into the next statement.
  //  - Once no further statements are encountered, we return the prepared
  //    statement for the last valid statement.
  std::optional<SqliteEngine::PreparedStatement> res;
  ExecutionStats stats;
  PerfettoSqlParser parser(std::move(sql_source));
  while (parser.Next()) {
    std::optional<SqlSource> source;
    if (auto* cf = std::get_if<PerfettoSqlParser::CreateFunction>(
            &parser.statement())) {
      auto source_or = ExecuteCreateFunction(*cf);
      RETURN_IF_ERROR(AddTracebackIfNeeded(source_or.status(), cf->sql));
      source = std::move(source_or.value());
    } else if (auto* cst = std::get_if<PerfettoSqlParser::CreateTable>(
                   &parser.statement())) {
      RETURN_IF_ERROR(AddTracebackIfNeeded(
          RegisterSqlTable(cst->name, cst->sql), cst->sql));
      // Since the rest of the code requires a statement, just use a no-value
      // dummy statement.
      source = cst->sql.FullRewrite(
          SqlSource::FromTraceProcessorImplementation("SELECT 0 WHERE 0"));
    } else {
      // If none of the above matched, this must just be an SQL statement
      // directly executable by SQLite.
      auto* sql =
          std::get_if<PerfettoSqlParser::SqliteSql>(&parser.statement());
      PERFETTO_CHECK(sql);
      source = std::move(sql->sql);
    }

    // Try to get SQLite to prepare the statement.
    std::optional<SqliteEngine::PreparedStatement> cur_stmt;
    {
      PERFETTO_TP_TRACE(metatrace::Category::QUERY, "QUERY_PREPARE");
      auto stmt = engine_->PrepareStatement(std::move(*source));
      RETURN_IF_ERROR(stmt.status());
      cur_stmt = std::move(stmt);
    }

    // The only situation where we'd have an ok status but also no prepared
    // statement is if the SQL was a pure comment. However, the PerfettoSQL
    // parser should filter out such statements so this should never happen.
    PERFETTO_DCHECK(cur_stmt->sqlite_stmt());

    // Before stepping into |cur_stmt|, we need to finish iterating through
    // the previous statement so we don't have two clashing statements (e.g.
    // SELECT * FROM v and DROP VIEW v) partially stepped into.
    if (res && !res->IsDone()) {
      PERFETTO_TP_TRACE(metatrace::Category::QUERY, "STMT_STEP_UNTIL_DONE",
                        [&res](metatrace::Record* record) {
                          record->AddArg("SQL", res->expanded_sql());
                        });
      while (res->Step()) {
      }
      RETURN_IF_ERROR(res->status());
    }

    // Propogate the current statement to the next iteration.
    res = std::move(cur_stmt);

    // Step the newly prepared statement once. This is considered to be
    // "executing" the statement.
    {
      PERFETTO_TP_TRACE(metatrace::Category::TOPLEVEL, "STMT_FIRST_STEP",
                        [&res](metatrace::Record* record) {
                          record->AddArg("SQL", res->expanded_sql());
                        });
      PERFETTO_DLOG("Executing statement: %s", res->sql());
      res->Step();
      RETURN_IF_ERROR(res->status());
    }

    // Increment the neecessary counts for the statement.
    IncrementCountForStmt(*res, &stats);
  }
  RETURN_IF_ERROR(parser.status());

  // If we didn't manage to prepare a single statement, that means everything
  // in the SQL was treated as a comment.
  if (!res)
    return base::ErrStatus("No valid SQL to run");

  // Update the output statement and column count.
  stats.column_count =
      static_cast<uint32_t>(sqlite3_column_count(res->sqlite_stmt()));
  return ExecutionResult{std::move(*res), stats};
}

base::Status PerfettoSqlEngine::RegisterSqlFunction(bool replace,
                                                    std::string prototype_str,
                                                    std::string return_type_str,
                                                    SqlSource sql) {
  // Parse all the arguments into a more friendly form.
  Prototype prototype;
  base::Status status =
      ParsePrototype(base::StringView(prototype_str), prototype);
  if (!status.ok()) {
    return base::ErrStatus("CREATE PERFETTO FUNCTION[prototype=%s]: %s",
                           prototype_str.c_str(), status.c_message());
  }

  // Parse the return type into a enum format.
  auto opt_return_type =
      sql_argument::ParseType(base::StringView(return_type_str));
  if (!opt_return_type) {
    return base::ErrStatus(
        "CREATE PERFETTO FUNCTION[prototype=%s, return=%s]: unknown return "
        "type specified",
        prototype_str.c_str(), return_type_str.c_str());
  }

  int created_argc = static_cast<int>(prototype.arguments.size());
  auto* ctx = static_cast<CreatedFunction::Context*>(
      sqlite_engine()->GetFunctionContext(prototype.function_name,
                                          created_argc));
  if (!ctx) {
    // We register the function with SQLite before we prepare the statement so
    // the statement can reference the function itself, enabling recursive
    // calls.
    std::unique_ptr<CreatedFunction::Context> created_fn_ctx =
        CreatedFunction::MakeContext(this);
    ctx = created_fn_ctx.get();
    RETURN_IF_ERROR(RegisterCppFunction<CreatedFunction>(
        prototype.function_name.c_str(), created_argc,
        std::move(created_fn_ctx)));
  }
  return CreatedFunction::ValidateOrPrepare(
      ctx, replace, std::move(prototype), std::move(prototype_str),
      std::move(*opt_return_type), std::move(return_type_str), std::move(sql));
}

base::Status PerfettoSqlEngine::RegisterSqlTable(std::string name,
                                                 SqlSource sql) {
  auto stmt_or = engine_->PrepareStatement(sql);
  RETURN_IF_ERROR(stmt_or.status());
  SqliteEngine::PreparedStatement stmt = std::move(stmt_or);

  uint32_t columns =
      static_cast<uint32_t>(sqlite3_column_count(stmt.sqlite_stmt()));
  std::vector<std::string> column_names;
  for (uint32_t i = 0; i < columns; ++i) {
    column_names.push_back(
        sqlite3_column_name(stmt.sqlite_stmt(), static_cast<int>(i)));

    if (column_names.back().empty()) {
      return base::ErrStatus(
          "CREATE PERFETTO TABLE: column name must not be empty");
    }
  }

  size_t column_count = column_names.size();
  auto table = std::make_unique<RuntimeTable>(pool_, std::move(column_names));
  uint32_t rows = 0;
  int res;
  for (res = sqlite3_step(stmt.sqlite_stmt()); res == SQLITE_ROW;
       ++rows, res = sqlite3_step(stmt.sqlite_stmt())) {
    for (uint32_t i = 0; i < column_count; ++i) {
      int int_i = static_cast<int>(i);
      switch (sqlite3_column_type(stmt.sqlite_stmt(), int_i)) {
        case SQLITE_NULL:
          RETURN_IF_ERROR(table->AddNull(i));
          break;
        case SQLITE_INTEGER:
          RETURN_IF_ERROR(table->AddInteger(
              i, sqlite3_column_int64(stmt.sqlite_stmt(), int_i)));
          break;
        case SQLITE_FLOAT:
          RETURN_IF_ERROR(table->AddFloat(
              i, sqlite3_column_double(stmt.sqlite_stmt(), int_i)));
          break;
        case SQLITE_TEXT: {
          RETURN_IF_ERROR(table->AddText(
              i, reinterpret_cast<const char*>(
                     sqlite3_column_text(stmt.sqlite_stmt(), int_i))));
          break;
        }
        case SQLITE_BLOB:
          return base::ErrStatus(
              "CREATE PERFETTO TABLE on column '%s' in table '%s': bytes "
              "columns are not supported",
              sqlite3_column_name(stmt.sqlite_stmt(), int_i), name.c_str());
      }
    }
  }
  if (res != SQLITE_DONE) {
    return base::ErrStatus("%s: SQLite error while creating table body: %s",
                           name.c_str(), sqlite3_errmsg(engine_->db()));
  }
  RETURN_IF_ERROR(table->AddColumnsAndOverlays(rows));

  DbSqliteTable::Context context{
      query_cache_.get(), DbSqliteTable::TableComputation::kRuntime,
      /*static_table=*/nullptr, std::move(table), /*generator=*/nullptr};
  engine_->RegisterVirtualTableModule<DbSqliteTable>(
      name, std::move(context), SqliteTable::kEponymousOnly, false);
  return base::OkStatus();
}

base::Status PerfettoSqlEngine::EnableSqlFunctionMemoization(
    const std::string& name) {
  constexpr size_t kSupportedArgCount = 1;
  CreatedFunction::Context* ctx = static_cast<CreatedFunction::Context*>(
      sqlite_engine()->GetFunctionContext(name.c_str(), kSupportedArgCount));
  if (!ctx) {
    return base::ErrStatus(
        "EXPERIMENTAL_MEMOIZE: Function %s(INT) does not exist", name.c_str());
  }
  return CreatedFunction::EnableMemoization(ctx);
}

base::StatusOr<SqlSource> PerfettoSqlEngine::ExecuteCreateFunction(
    const PerfettoSqlParser::CreateFunction& cf) {
  if (!cf.is_table) {
    RETURN_IF_ERROR(
        RegisterSqlFunction(cf.replace, cf.prototype, cf.returns, cf.sql));

    // Since the rest of the code requires a statement, just use a no-value
    // dummy statement.
    return cf.sql.FullRewrite(
        SqlSource::FromTraceProcessorImplementation("SELECT 0 WHERE 0"));
  }

  CreatedTableFunction::State state{cf.prototype, cf.sql, {}, {}, std::nullopt};
  base::StringView function_name;
  RETURN_IF_ERROR(
      ParseFunctionName(state.prototype_str.c_str(), function_name));

  // Parse all the arguments into a more friendly form.
  base::Status status =
      ParsePrototype(state.prototype_str.c_str(), state.prototype);
  if (!status.ok()) {
    return base::ErrStatus("CREATE PERFETTO FUNCTION[prototype=%s]: %s",
                           state.prototype_str.c_str(), status.c_message());
  }

  // Parse the return type into a enum format.
  status =
      sql_argument::ParseArgumentDefinitions(cf.returns, state.return_values);
  if (!status.ok()) {
    return base::ErrStatus(
        "CREATE PERFETTO FUNCTION[prototype=%s, return=%s]: unknown return "
        "type specified",
        state.prototype_str.c_str(), cf.returns.c_str());
  }

  // Verify that the provided SQL prepares to a statement correctly.
  auto stmt = sqlite_engine()->PrepareStatement(cf.sql);
  RETURN_IF_ERROR(stmt.status());

  // Verify that every argument name in the function appears in the
  // argument list.
  //
  // We intentionally loop from 1 to |used_param_count| because SQL
  // parameters are 1-indexed *not* 0-indexed.
  int used_param_count = sqlite3_bind_parameter_count(stmt.sqlite_stmt());
  for (int i = 1; i <= used_param_count; ++i) {
    const char* name = sqlite3_bind_parameter_name(stmt.sqlite_stmt(), i);

    if (!name) {
      return base::ErrStatus(
          "%s: \"Nameless\" SQL parameters cannot be used in the SQL "
          "statements of view functions.",
          state.prototype.function_name.c_str());
    }

    if (!base::StringView(name).StartsWith("$")) {
      return base::ErrStatus(
          "%s: invalid parameter name %s used in the SQL definition of "
          "the view function: all parameters must be prefixed with '$' not ':' "
          "or '@'.",
          state.prototype.function_name.c_str(), name);
    }

    auto it = std::find_if(state.prototype.arguments.begin(),
                           state.prototype.arguments.end(),
                           [name](const sql_argument::ArgumentDefinition& arg) {
                             return arg.dollar_name() == name;
                           });
    if (it == state.prototype.arguments.end()) {
      return base::ErrStatus(
          "%s: parameter %s does not appear in the list of arguments in the "
          "prototype of the view function.",
          state.prototype.function_name.c_str(), name);
    }
  }

  // Verify that the prepared statement column count matches the return
  // count.
  uint32_t col_count =
      static_cast<uint32_t>(sqlite3_column_count(stmt.sqlite_stmt()));
  if (col_count != state.return_values.size()) {
    return base::ErrStatus(
        "%s: number of return values %u does not match SQL statement column "
        "count %zu.",
        state.prototype.function_name.c_str(), col_count,
        state.return_values.size());
  }

  // Verify that the return names matches the prepared statment column names.
  for (uint32_t i = 0; i < col_count; ++i) {
    const char* name =
        sqlite3_column_name(stmt.sqlite_stmt(), static_cast<int>(i));
    if (name != state.return_values[i].name()) {
      return base::ErrStatus(
          "%s: column %s at index %u does not match return value name %s.",
          state.prototype.function_name.c_str(), name, i,
          state.return_values[i].name().c_str());
    }
  }
  state.reusable_stmt = std::move(stmt);

  std::string fn_name = state.prototype.function_name;
  std::string lower_name = base::ToLower(state.prototype.function_name);
  if (created_table_function_state_.Find(lower_name)) {
    if (!cf.replace) {
      return base::ErrStatus("Table function named %s already exists",
                             state.prototype.function_name.c_str());
    }
    // This will cause |OnTableFunctionDestroyed| below to be executed.
    base::StackString<1024> drop("DROP TABLE %s",
                                 state.prototype.function_name.c_str());
    auto res = Execute(
        SqlSource::FromTraceProcessorImplementation(drop.ToStdString()));
    RETURN_IF_ERROR(res.status());
  }

  auto it_and_inserted = created_table_function_state_.Insert(
      lower_name,
      std::make_unique<CreatedTableFunction::State>(std::move(state)));
  PERFETTO_CHECK(it_and_inserted.second);

  base::StackString<1024> create(
      "CREATE VIRTUAL TABLE %s USING created_table_function", fn_name.c_str());
  return cf.sql.FullRewrite(
      SqlSource::FromTraceProcessorImplementation(create.ToStdString()));
}

CreatedTableFunction::State* PerfettoSqlEngine::GetTableFunctionState(
    const std::string& name) const {
  auto it = created_table_function_state_.Find(base::ToLower(name));
  PERFETTO_CHECK(it);
  return it->get();
}

void PerfettoSqlEngine::OnTableFunctionDestroyed(const std::string& name) {
  PERFETTO_CHECK(created_table_function_state_.Erase(base::ToLower(name)));
}

}  // namespace trace_processor
}  // namespace perfetto
