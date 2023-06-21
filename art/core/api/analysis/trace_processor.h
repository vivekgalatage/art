#pragma once

#include <optional>
#include <string>

#include "../art_export.h"
#include "../art_impl.h"

namespace art::analysis {

class ART_EXPORT Vivek {
 public:
  void hello();
};

class ART_EXPORT TraceProcessor
    : public art::impl::APIInterface<art::analysis::TraceProcessor> {
 public:
  explicit TraceProcessor();
  ~TraceProcessor();

  bool OpenTrace(const std::string& file_path);
  void ExecuteSQLQuery(const std::string& query) const;

 private:
  friend class art::impl::APIInterface<art::analysis::TraceProcessor>;
  friend class art::impl::APIInterface<art::analysis::TraceProcessor>::Impl;
};

}  // namespace art::analysis
