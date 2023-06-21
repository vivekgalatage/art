#pragma once

#include "art/core/api/analysis/trace_processor.h"
#include "art/core/api/art_impl.h"
#include "art/core/api/art_shared.h"
#include "include/perfetto/base/status.h"
#include "include/perfetto/trace_processor/trace_processor.h"

namespace art::impl {

using TraceProcessor = art::impl::APIInterface<art::analysis::TraceProcessor>;

template <>
class TraceProcessor::Impl
    : public art::impl::SharedImpl<TraceProcessor::Impl> {
 public:
  explicit Impl();
  bool OpenTrace(const std::string& file_path);
  void ExecuteSQLQuery(const std::string& query) const;

 private:
  perfetto::base::Status OpenTraceInternal(const std::string& file_path,
                                           double* size_mb);
  std::unique_ptr<perfetto::trace_processor::TraceProcessor> tp_;
  std::string file_path_;
};

}  // namespace art::impl
