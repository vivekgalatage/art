#import "ArtExport.h"

#import <AppKit/AppKit.h>

ART_OBJC_EXPORT
@interface ArtTraceProcessor : NSObject
- (void)openTrace:(NSString*)filePath;
- (void)executeSQLQuery:(NSString*)query;
@end
