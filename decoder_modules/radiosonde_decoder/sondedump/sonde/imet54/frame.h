#ifndef imet54_frame_h
#define imet54_frame_h

#include <stdint.h>
#include <stdlib.h>
#include "protocol.h"

/**
 * Turn a raw iMet-54 frame into its 108 byte payload.
 *
 * Undoes, in order: the 8N1 start and stop bits, the 64 bit block interleaver, and
 * the Hamming[8,4] code.
 *
 * @param dst      where to write the IMET54_DATA_LEN byte payload
 * @param raw_bits the frame as handed back by the framer, packed eight bits to the
 *                 byte, most significant bit first, aligned so that the sync word
 *                 starts at bit zero
 *
 * @return how many of the 216 codewords were damaged beyond what Hamming[8,4] can
 *         repair, or -1 if the frame was too short to contain a payload at all.
 *         Zero is a clean frame; a handful is a weak signal; a couple of hundred
 *         means the payload is not being read from the right place. Ruined codewords
 *         become zero nibbles - it is the caller's job to decide, from this count
 *         and from the checksum, whether the result is worth anything.
 */
int imet54_frame_decode(uint8_t *dst, const uint8_t *raw_bits);

/**
 * Check the payload against the CRC32 it carries.
 *
 * @param frame the decoded payload, at least IMET54_POS_CRC32 + 4 bytes long
 * @return 1 if the CRC matches, 0 otherwise
 */
int imet54_frame_crc_ok(const uint8_t *frame);

#endif
