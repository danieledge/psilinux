/* stdint.h - for gcc 3.0 (the EPOC SDK's compiler), which has none. Only
 * the runner's build uses this folder (SoftFloat needs it). */
#ifndef PSILINUX_STDINT_H
#define PSILINUX_STDINT_H
typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef short int16_t;
typedef unsigned short uint16_t;
typedef int int32_t;
typedef unsigned int uint32_t;
typedef long long int64_t;
typedef unsigned long long uint64_t;
typedef int8_t int_fast8_t;
typedef uint8_t uint_fast8_t;
typedef int32_t int_fast16_t;
typedef uint32_t uint_fast16_t;
typedef int32_t int_fast32_t;
typedef uint32_t uint_fast32_t;
typedef int64_t int_fast64_t;
typedef uint64_t uint_fast64_t;
typedef int8_t int_least8_t;
typedef uint8_t uint_least8_t;
typedef int16_t int_least16_t;
typedef uint16_t uint_least16_t;
typedef int32_t int_least32_t;
typedef int64_t int_least64_t;
typedef uint64_t uint_least64_t;
typedef uint32_t uint_least32_t;
typedef int intptr_t;
typedef unsigned int uintptr_t;
#define INT32_C(x) x
#define UINT32_C(x) x ## U
#define INT64_C(x) x ## LL
#define UINT64_C(x) x ## ULL
#endif
