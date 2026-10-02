#pragma once

#ifdef _MSC_VER

#if _MSC_VER < 1900
#error "Required Visual Studio 2015 or newer."
#endif

#define CAM_DLL_EXPORT __declspec(dllexport)
#define CAM_DLL_IMPORT __declspec(dllimport)
#define CAM_INLINE __inline
#define CAM_FORCE_INLINE __forceinline
#define CAM_FORCE_NOINLINE __declspec(noinline)
#define CAM_STDCALL __stdcall
#define CAM_THREADLOCAL __declspec(thread)
#define CAM_NORETURN __declspec(noreturn)
#define CAM_PACK_BEGIN() __pragma(pack(push, 1))
#define CAM_PACK_END() ; __pragma(pack(pop))
#define CAM_ALIGN_BEGIN(_align) __declspec(align(_align))
#define CAM_ALIGN_END(_align) /* NOTHING */
#define CAM_OFFSET_OF(X, Y) offsetof(X, Y)
#define CAM_DEPRECATED __declspec(deprecated)

#ifdef CAM_DEBUG
#define CAM_DEBUG_BREAK __debugbreak()
#else
#define CAM_DEBUG_BREAK
#endif

#pragma warning(disable: 4251)

#elif defined(__clang__)

#define CAM_DLL_EXPORT __attribute__ ((visibility ("default")))
#define CAM_DLL_IMPORT /* NOTHING */
#define CAM_INLINE inline
#define CAM_FORCE_INLINE inline
#define CAM_FORCE_NOINLINE __attribute__ ((noinline))
#define CAM_STDCALL __attribute__ ((stdcall))
#define CAM_THREADLOCAL __thread
#define CAM_NORETURN __attribute__ ((noreturn))
#define CAM_PACK_BEGIN() /* NOTHING */
#define CAM_PACK_END() __attribute__ ((__packed__))
#define CAM_ALIGN_BEGIN(_align) /* NOTHING */
#define CAM_ALIGN_END(_align) __attribute__ ((aligned(_align)))
#define CAM_OFFSET_OF(X, Y) __builtin_offsetof(X, Y)
#define CAM_DEPRECATED /* NOTHING */

#pragma clang diagnostic ignored "-Wswitch"
#pragma clang diagnostic ignored "-Wmacro-redefined"
#pragma clang diagnostic ignored "-Waddress-of-packed-member"
#pragma clang diagnostic ignored "-Wnull-dereference"
#pragma clang diagnostic ignored "-Winvalid-noreturn"

#ifdef CAM_DEBUG
#define CAM_DEBUG_BREAK __builtin_debugtrap()
#else
#define CAM_DEBUG_BREAK
#endif

#elif defined(__GNUC__)

#define CAM_DLL_EXPORT __attribute__((visibility("default")))
#define CAM_DLL_IMPORT /* NOTHING */
#define CAM_INLINE inline
#define CAM_FORCE_INLINE inline
#define CAM_FORCE_NOINLINE __attribute__((noinline))
#define CAM_STDCALL __attribute__((stdcall))
#define CAM_THREADLOCAL __thread
#define CAM_NORETURN __attribute__((noreturn))
#define CAM_PACK_BEGIN() /* NOTHING */
#define CAM_PACK_END() __attribute__((__packed__))
#define CAM_ALIGN_BEGIN(_align) /* NOTHING */
#define CAM_ALIGN_END(_align) __attribute__((aligned(_align)))
#define CAM_OFFSET_OF(X, Y) __builtin_offsetof(X, Y)
#define CAM_DEPRECATED __attribute__((deprecated))

#ifdef CAM_DEBUG
#define CAM_DEBUG_BREAK __builtin_trap()
#else
#define CAM_DEBUG_BREAK
#endif

#elif defined(__INTEL_COMPILER)

#define CAM_DLL_EXPORT ??
#define CAM_DLL_IMPORT ??
#define CAM_INLINE inline
#define CAM_FORCE_INLINE inline
#define CAM_FORCE_NOINLINE /* NOTHING */
#define CAM_STDCALL __stdcall
#define CAM_NORETURN ??
#define CAM_PACK_BEGIN() ??
#define CAM_PACK_END() ??
#define CAM_ALIGN_BEGIN(_align) ??
#define CAM_ALIGN_END(_align) ??
#define CAM_OFFSET_OF(X, Y) ??
#define CAM_DEPRECATED ??

#ifdef _WIN32
#define CAM_THREADLOCAL __declspec(thread)
#else
#define CAM_THREADLOCAL __thread
#endif

#ifdef CAM_DEBUG
#define CAM_DEBUG_BREAK ??
#else
#define CAM_DEBUG_BREAK
#endif

#else

#pragma error "Unknown Compiler"

#endif

#define CAM_PACK_STRUCT(__declaration__) CAM_PACK_BEGIN() __declaration__ CAM_PACK_END()

