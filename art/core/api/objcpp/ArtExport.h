#if !__OBJC__
#error Only include this file inside Objective C or Objective C++
#endif

#define ART_OBJC_EXPORT __attribute__((visibility("default")))
