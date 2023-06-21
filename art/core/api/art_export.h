#pragma once

#if defined(_WIN32)

#if defined(ART_IMPLEMENTATION)
#define ART_EXPORT __declspec(dllexport)
#else  // !defined(ART_IMPLEMENTATION)
#define ART_EXPORT __declspec(dllimport)
#endif  // !defined(ART_IMPLEMENTATION)

#else  // !defined(_WIN32)
#if defined(ART_IMPLEMENTATION)
#define ART_EXPORT __attribute__((visibility("default")))
#else  // !defined(ART_IMPLEMENTATION)
#define ART_EXPORT
#endif  // !defined(ART_IMPLEMENTATION)
#endif  // !defined(_WIN32)
