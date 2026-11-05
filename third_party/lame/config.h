/* LAME 3.100 build configuration for The Dojo (shared by GCC, Clang and MSVC).
   Only the encoder (libmp3lame) is built; no decoder (mpglib), no frontend, no SIMD paths. */
#ifndef DOJO_LAME_CONFIG_H
#define DOJO_LAME_CONFIG_H

#define STDC_HEADERS 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_LIMITS_H 1
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STRCHR 1
#define HAVE_MEMCPY 1
#define PROTOTYPES 1
#define USE_FAST_LOG 1
#define TAKEHIRO_IEEE754_HACK 1
#define PACKAGE "lame"
#define LAME_LIBRARY_BUILD 1

typedef long double ieee854_float80_t;
typedef double ieee754_float64_t;
typedef float ieee754_float32_t;

#endif
