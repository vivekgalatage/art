#include "api/analysis/trace_processor.h"

#include <iostream>

int main() {
  art::analysis::TraceProcessor tp;
  tp.OpenTrace("<trace_file_path_here>");
  tp.ExecuteSQLQuery("select * from args");
  return 0;
}
