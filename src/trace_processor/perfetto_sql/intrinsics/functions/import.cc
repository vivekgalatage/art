/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "src/trace_processor/perfetto_sql/intrinsics/functions/import.h"

#include <numeric>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/sqlite/scoped_db.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/sqlite/sqlite_table.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/tp_metatrace.h"
#include "src/trace_processor/util/sql_modules.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto {
namespace trace_processor {

base::Status Import::Run(Import::Context* ctx,
                         size_t argc,
                         sqlite3_value** argv,
                         SqlValue&,
                         Destructors&) {
  if (argc != 1) {
    return base::ErrStatus(
        "IMPORT: invalid number of args; expected 1, received "
        "%zu",
        argc);
  }
  sqlite3_value* import_val = argv[0];

  // Type check
  {
    base::Status status =
        sqlite_utils::TypeCheckSqliteValue(import_val, SqlValue::Type::kString);
    if (!status.ok()) {
      return base::ErrStatus("IMPORT(%s): %s", sqlite3_value_text(import_val),
                             status.c_message());
    }
  }

  const char* import_key =
      reinterpret_cast<const char*>(sqlite3_value_text(import_val));
  PERFETTO_TP_TRACE(
      metatrace::Category::TOPLEVEL, "Import",
      [import_key](metatrace::Record* r) { r->AddArg("Import", import_key); });

  std::string module_name = sql_modules::GetModuleName(import_key);
  auto module = ctx->modules->Find(module_name);
  if (!module)
    return base::ErrStatus("IMPORT: Unknown module name provided - %s",
                           import_key);

  auto module_file = module->import_key_to_file.Find(import_key);
  if (!module_file) {
    return base::ErrStatus("IMPORT: Unknown filename provided - %s",
                           import_key);
  }
  // IMPORT is noop for already imported files.
  if (module_file->imported) {
    return base::OkStatus();
  }

  auto it = ctx->engine->Execute(
      SqlSource::FromModuleImport(module_file->sql, import_key));
  RETURN_IF_ERROR(it.status());
  if (it->statement_count_with_output > 0)
    return base::ErrStatus("IMPORT: Imported file returning values.");
  module_file->imported = true;
  return base::OkStatus();
}

}  // namespace trace_processor
}  // namespace perfetto
