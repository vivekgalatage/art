#include "art/core/analysis/trace_processor_impl.h"

#include <optional>
#include <sstream>

#include "include/perfetto/base/time.h"
#include "src/profiling/deobfuscator.h"
#include "src/profiling/symbolizer/local_symbolizer.h"
#include "src/profiling/symbolizer/symbolize_database.h"
#include "src/profiling/symbolizer/symbolizer.h"
#include "src/trace_processor/read_trace_internal.h"

using namespace perfetto;
using namespace perfetto::trace_processor;
namespace art::impl {

TraceProcessor::Impl::Impl() {}

perfetto::base::Status TraceProcessor::Impl::OpenTraceInternal(
    const std::string& file_path,
    double* size_mb) {
  base::Status read_status = ReadTraceUnfinalized(
      tp_.get(), file_path.c_str(), [&size_mb](size_t parsed_size) {
        *size_mb = static_cast<double>(parsed_size) / 1E6;
        fprintf(stderr, "\rLoading trace: %.2f MB\r", *size_mb);
      });
  tp_->Flush();
  if (!read_status.ok()) {
    return base::ErrStatus("Could not read trace file (path: %s): %s",
                           file_path.c_str(), read_status.c_message());
  }

  std::unique_ptr<profiling::Symbolizer> symbolizer =
      profiling::LocalSymbolizerOrDie(profiling::GetPerfettoBinaryPath(),
                                      getenv("PERFETTO_SYMBOLIZER_MODE"));

  if (symbolizer) {
    profiling::SymbolizeDatabase(
        tp_.get(), symbolizer.get(), [this](const std::string& trace_proto) {
          std::unique_ptr<uint8_t[]> buf(new uint8_t[trace_proto.size()]);
          memcpy(buf.get(), trace_proto.data(), trace_proto.size());
          auto status = tp_->Parse(std::move(buf), trace_proto.size());
          if (!status.ok()) {
            PERFETTO_DFATAL_OR_ELOG("Failed to parse: %s",
                                    status.message().c_str());
            return;
          }
        });
    tp_->Flush();
  }

  auto maybe_map = profiling::GetPerfettoProguardMapPath();
  if (!maybe_map.empty()) {
    profiling::ReadProguardMapsToDeobfuscationPackets(
        maybe_map, [this](const std::string& trace_proto) {
          std::unique_ptr<uint8_t[]> buf(new uint8_t[trace_proto.size()]);
          memcpy(buf.get(), trace_proto.data(), trace_proto.size());
          auto status = tp_->Parse(std::move(buf), trace_proto.size());
          if (!status.ok()) {
            PERFETTO_DFATAL_OR_ELOG("Failed to parse: %s",
                                    status.message().c_str());
            return;
          }
        });
  }
  tp_->NotifyEndOfFile();
  return base::OkStatus();
}

bool TraceProcessor::Impl::OpenTrace(const std::string& file_path) {
  perfetto::trace_processor::Config config;
  tp_ = perfetto::trace_processor::TraceProcessor::CreateInstance(config);
  base::TimeNanos t_load{};
  base::TimeNanos t_load_start = base::GetWallTimeNs();
  double size_mb = 0;
  OpenTraceInternal(file_path, &size_mb);

  t_load = base::GetWallTimeNs() - t_load_start;

  double t_load_s = static_cast<double>(t_load.count()) / 1E9;
  PERFETTO_ILOG("Trace loaded: %.2f MB in %.2fs (%.1f MB/s)", size_mb, t_load_s,
                size_mb / t_load_s);
  return true;
}

void TraceProcessor::Impl::ExecuteSQLQuery(const std::string& query) const {
  auto it = tp_->ExecuteQuery(query);

  bool first = true;
  while (it.Next()) {
    if (first) {
      fprintf(stderr, "Error stats for this trace:\n");

      for (uint32_t i = 0; i < it.ColumnCount(); i++)
        fprintf(stderr, "%40s ", it.GetColumnName(i).c_str());
      fprintf(stderr, "\n");

      for (uint32_t i = 0; i < it.ColumnCount(); i++)
        fprintf(stderr, "%40s ", "----------------------------------------");
      fprintf(stderr, "\n");

      first = false;
    }

    for (uint32_t c = 0; c < it.ColumnCount(); c++) {
      auto value = it.Get(c);
      switch (value.type) {
        case SqlValue::Type::kNull:
          fprintf(stderr, "%-40.40s", "[NULL]");
          break;
        case SqlValue::Type::kDouble:
          fprintf(stderr, "%40f", value.double_value);
          break;
        case SqlValue::Type::kLong:
          // fprintf(stderr, "%40" PRIi64, value.long_value);
          break;
        case SqlValue::Type::kString:
          fprintf(stderr, "%s", value.string_value);
          break;
        case SqlValue::Type::kBytes:
          printf("%-40.40s", "<raw bytes>");
          break;
      }
      fprintf(stderr, " ");
    }
    fprintf(stderr, "\n");
  }

  base::Status status = it.Status();
  if (!status.ok()) {
    return;
  }
}

}  // namespace art::impl
