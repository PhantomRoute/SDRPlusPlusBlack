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

/* How much of the sync the correlator above matched, in symbols: 0x00 0xAA 0x24 0x24. */
#define IMET54_HEADER_SYMBOLS (IMET54_SYNC_LEN / IMET54_SYMBOL_BITS)    /* 4 */

/* And how much of it is still sitting in the stream afterwards.
 *
 * This is the part that is easy to get wrong and produces a decoder that finds the
 * signal, frames it, and never decodes a single byte. The sync the sonde actually
 * sends is FIVE bytes, 0x24 0x24 0x24 0x24 0x42, of which the correlation window
 * above covers only the first two - so 0x24 0x24 0x42 are still there, ahead of the
 * payload, and have to be stepped over.
 *
 * rs1729 skips 3 bytes here and not 7 because his bit buffer starts *after* the
 * matched header. sondedump's framer keeps the sync word at the front of the frame
 * (see RS41Frame, whose first member is its syncword), so both parts get skipped
 * here: the 4 matched symbols and the 3 that follow. */
#define IMET54_TRAILING_SYNC_SYMBOLS 3
#define IMET54_PREAMBLE_SYMBOLS (IMET54_HEADER_SYMBOLS + IMET54_TRAILING_SYNC_SYMBOLS)  /* 7 */
/* In de-8N1'd bits, which is where the skip is applied. */
#define IMET54_PAYLOAD_OFFSET_BITS (IMET54_PREAMBLE_SYMBOLS * 8)        /* 56 */

/* Seven symbols of sync and preamble, then 216 symbols carrying the coded payload.
 * The framer is asked for all of it because the sync is part of the frame here. */
#define IMET54_SYMBOLS (IMET54_PREAMBLE_SYMBOLS + 217)                  /* 224 */
#define IMET54_FRAME_LEN (IMET54_SYMBOLS * IMET54_SYMBOL_BITS)          /* 2240 bits */

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
