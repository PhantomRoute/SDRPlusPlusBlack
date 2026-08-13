#ifndef imet54_protocol_h
#define imet54_protocol_h

#include <stdint.h>
#include "utils.h"

/* iMet-54 (InterMet).
 *
 * Not a variant of the iMet-1/4 in the directory next door: that one is 1200 baud
 * AFSK with a completely different frame. This is 4800 baud GFSK, every byte sent
 * 8N1, with the payload interleaved in 64 bit blocks and protected by Hamming[8,4].
 *
 * The constants here were taken from rs1729's imet54mod.c and cross-checked against
 * an independent CC1101 implementation (sakul7-stack/iMet54); the two agree on the
 * sync word, the baud rate, the frame length, the FEC, the interleaver and every
 * field offset. See ../../../README.md.
 */

#define IMET54_BAUDRATE 4800

/* Preamble 0x00 0xAA then sync 0x24 0x24, each sent 8N1 (start bit, 8 data bits
 * least significant first, stop bit). Written out as the 40 bits that appear on
 * air, which is what the correlator matches against:
 *
 *   0000000001 0101010101 0001001001 0001001001
 *   \__0x00__/ \__0xAA__/ \__0x24__/ \__0x24__/
 *
 * The sonde actually sends ten repetitions of the preamble and four of the sync
 * byte. Correlating against only the last four symbols is deliberate - a shorter
 * window gives the signal more weight against the noise. */
#define IMET54_SYNCWORD 0x0055512449ULL
#define IMET54_SYNC_LEN 40

/* 8N1: ten bits on air per byte */
#define IMET54_SYMBOL_BITS 10
#define IMET54_SYMBOLS 220
#define IMET54_FRAME_LEN (IMET54_SYMBOLS * IMET54_SYMBOL_BITS)      /* 2200 bits */

/* The header symbols are part of the frame the framer hands back, and are dropped
 * once the start and stop bits have been taken out. */
#define IMET54_HEADER_SYMBOLS (IMET54_SYNC_LEN / IMET54_SYMBOL_BITS)    /* 4 */
#define IMET54_HEADER_DATA_BITS (IMET54_HEADER_SYMBOLS * 8)              /* 32 */

/* What is left is 27 blocks of 64 bits, which halve to 108 bytes through the
 * Hamming decode. Both reference implementations arrive at the same numbers. */
#define IMET54_CODED_BITS 1728
#define IMET54_DATA_LEN 108

/* The buffer the framer is given has to hold two frames, not one.
 *
 * framer_read() realigns by demodulating framelen + sync_offset bits into it, and
 * correlate() searches the whole frame for the sync word, so sync_offset can be
 * most of a frame on its own. One frame's worth of bytes is not enough and the
 * overrun lands on whatever follows in the struct. This is why every sonde that
 * ships with sondedump declares its raw frame as an array of two. */
#define IMET54_RAW_FRAME_LEN (2 * (IMET54_FRAME_LEN / 8) + 8)

/* Field offsets within the 108 byte payload. All multi-byte values big endian. */
#define IMET54_POS_SN 0x00       /* uint32  serial number */
#define IMET54_POS_GPSTIME 0x04  /* int32   HHMMSSmmm */
#define IMET54_POS_GPSLAT 0x08   /* int32   DDMM.mmmm * 1e6 */
#define IMET54_POS_GPSLON 0x0C   /* int32   DDMM.mmmm * 1e6 */
#define IMET54_POS_GPSALT 0x10   /* int32   decimetres */
#define IMET54_POS_T 0x1C        /* float32 air temperature, degrees C */
#define IMET54_POS_RH 0x20       /* float32 relative humidity, percent */
#define IMET54_POS_TRH 0x24      /* float32 humidity sensor temperature */
#define IMET54_POS_CRC32 0x34    /* uint32  CRC32/802.3 over the preceding bytes */

/* Written into a field the sonde has no reading for. It is 1e9 as a float, which
 * would otherwise sail through any plausibility check as a real measurement. */
#define IMET54_NO_DATA 0x4E6E6B28

#endif
