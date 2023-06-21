#import "ArtTraceProcessor.h"

#include "art/core/api/analysis/trace_processor.h"

#include <iostream>

@interface ArtTraceProcessor ()
@property(nonatomic, assign) art::analysis::TraceProcessor* tp;
@end

@implementation ArtTraceProcessor
@synthesize tp = _tp;

- (instancetype)init {
  self = [super init];
  if (self) {
    _tp = new art::analysis::TraceProcessor();
  }
  return self;
}

- (void) openTrace:(NSString*)filePath {
  _tp->OpenTrace(std::string([filePath UTF8String]));
}

- (void)executeSQLQuery:(NSString*)query {
  _tp->ExecuteSQLQuery(std::string([query UTF8String]));
}

@end
