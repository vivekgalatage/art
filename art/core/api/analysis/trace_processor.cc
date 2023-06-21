#include "trace_processor.h"

#include "art/core/analysis/trace_processor_impl.h"
#include "art/core/api/art_impl.h"

namespace art::analysis {

TraceProcessor::TraceProcessor()
    : art::impl::APIInterface<art::analysis::TraceProcessor>() {}

TraceProcessor::~TraceProcessor() = default;

bool TraceProcessor::OpenTrace(const std::string& file_path) {
  return impl_->OpenTrace(file_path);
}

void TraceProcessor::ExecuteSQLQuery(const std::string& query) const {
  return impl_->ExecuteSQLQuery(query);
}

}  // namespace art::analysis
