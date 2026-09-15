/*
 * Written for SDR++, in place of the config.h that LAME's configure script generates.
 *
 * Only the encoding library is built - no frontend, no mpglib decoder, no assembly
 * or SSE routines - so all this has to say is that the target is a hosted C
 * environment with the standard headers. That holds for every compiler SDR++ is
 * built with, which is why nothing here is probed.
 */
#ifndef SDRPP_LAME_CONFIG_H
#define SDRPP_LAME_CONFIG_H

#define STDC_HEADERS 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_LIMITS_H 1
#define HAVE_STRCHR 1
#define HAVE_MEMCPY 1
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define PROTOTYPES 1

/* A faster log with less, but enough, precision. LAME's own builds turn it on. */
#define USE_FAST_LOG 1

#define PACKAGE "lame"
#define VERSION "3.100"
#define LAME_LIBRARY_BUILD

typedef long double ieee854_float80_t;
typedef double ieee754_float64_t;
typedef float ieee754_float32_t;

#endif
