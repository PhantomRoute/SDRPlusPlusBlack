/*
 * Written for SDR++, in place of the config.h that FLAC's CMake build generates from
 * config.cmake.h.in.
 *
 * Built plainly: no Ogg, no assembly or intrinsics (FLAC__NO_ASM is set by the build),
 * no threads. The recorder encodes one audio channel or two at 48 kHz, which is nothing
 * for the portable code, and leaving the CPU-specific paths out means the same sources
 * build unchanged on every target SDR++ has, Android included.
 */
#ifndef SDRPP_FLAC_CONFIG_H
#define SDRPP_FLAC_CONFIG_H

#if (defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define CPU_IS_BIG_ENDIAN 1
#else
#define CPU_IS_BIG_ENDIAN 0
#endif
#define WORDS_BIGENDIAN CPU_IS_BIG_ENDIAN

#define ENABLE_64_BIT_WORDS 0

#define OGG_FOUND 0
#define FLAC__HAS_OGG OGG_FOUND

#define FLAC__HAS_X86INTRIN 0
#define FLAC__HAS_NEONINTRIN 0
#define FLAC__HAS_A64NEONINTRIN 0

#define HAVE_LROUND 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1

#if !defined(_WIN32)
#define HAVE_UNISTD_H 1
#define HAVE_FSEEKO 1
#endif

#if defined(__APPLE__)
#define FLAC__SYS_DARWIN
#elif defined(__linux__)
#define FLAC__SYS_LINUX
#endif

#define PACKAGE_VERSION "1.5.0"

/* As FLAC's own config does, so fseeko and friends are declared on glibc and bionic. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#ifndef _LARGEFILE_SOURCE
#define _LARGEFILE_SOURCE
#endif

#endif
