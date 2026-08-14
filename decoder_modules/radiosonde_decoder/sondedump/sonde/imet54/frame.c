#include <string.h>
#include "frame.h"

/* Hamming[8,4]. Sixteen valid codewords, one per nibble: a received byte that is
 * not in this table is either uncorrectable or was corrected into it first. */
static const uint8_t HAM_LUT[16] = {
    0x00, 0x87, 0x99, 0x1E, 0xAA, 0x2D, 0x33, 0xB4,
    0x4B, 0xCC, 0xD2, 0x55, 0xE1, 0x66, 0x78, 0xFF
};

/* Parity check matrix, and the syndrome each single bit error produces. */
static const uint8_t HAM_H[4][8] = {
    { 1, 0, 1, 0, 1, 0, 1, 0 },
    { 0, 1, 1, 0, 0, 1, 1, 0 },
    { 0, 0, 0, 1, 1, 1, 1, 0 },
    { 1, 1, 1, 1, 1, 1, 1, 1 }
};
static const uint8_t HAM_HE[8] = { 0x9, 0xA, 0xB, 0xC, 0xD, 0xE, 0xF, 0x8 };

/* One bit per byte from here down. It costs a couple of kilobytes of stack and it
 * makes the interleaver and the code below read the way they are described, which
 * on a bit level transform is worth more than the memory. */

/* Correct a single codeword in place. Returns 0 if it was already clean, 1 if one
 * bit was flipped back, -1 if it cannot be repaired. */
static int
ham_correct(uint8_t *code)
{
    uint8_t syndrome[4];
    int i, j;
    unsigned int synval = 0;

    for (i = 0; i < 4; i++) {
        syndrome[i] = 0;
        for (j = 0; j < 8; j++) {
            syndrome[i] ^= HAM_H[i][j] & code[j];
        }
    }

    /* Syndrome bits are least significant first */
    for (i = 0; i < 4; i++) {
        synval |= (unsigned int)(syndrome[i] & 1) << i;
    }
    if (!synval) { return 0; }

    for (j = 0; j < 8; j++) {
        if (synval == HAM_HE[j]) {
            code[j] ^= 1;
            return 1;
        }
    }
    return -1;
}

int
imet54_frame_decode(uint8_t *dst, const uint8_t *raw_bits)
{
    /* On the stack, not static: this module can be instantiated more than once and
     * each instance decodes on its own thread, so anything shared here would be two
     * threads writing the same buffer. Six kilobytes is nothing on a worker stack. */
    uint8_t bits[IMET54_FRAME_LEN];          /* as received, one bit per byte */
    uint8_t stripped[IMET54_SYMBOLS * 8];    /* start and stop bits removed */
    uint8_t deinter[IMET54_CODED_BITS];      /* interleaver undone */
    int i, j, n;
    int corrected = 0;
    int out = 0;
    uint8_t nibble[IMET54_DATA_LEN * 2];

    /* Unpack the frame the framer produced. It is packed most significant bit
     * first, which is the order the correlator reads it in. */
    for (i = 0; i < IMET54_FRAME_LEN; i++) {
        bits[i] = (raw_bits[i / 8] >> (7 - (i % 8))) & 1;
    }

    /* 8N1: of every ten bits keep the middle eight, dropping the start bit and the
     * stop bit. */
    n = 0;
    for (i = 0; i < IMET54_FRAME_LEN; i++) {
        int pos = i % IMET54_SYMBOL_BITS;
        if (pos > 0 && pos < 9) { stripped[n++] = bits[i]; }
    }

    /* The sync and preamble are data as far as the above is concerned. Step over all
     * of them - both the symbols the correlator matched and the three that follow it
     * - leaving exactly the coded payload. */
    if (n - IMET54_PAYLOAD_OFFSET_BITS < IMET54_CODED_BITS) { return -1; }

    /* Undo the interleaver: each 64 bit block is an 8x8 bit matrix, transposed. */
    for (n = 0; n + 64 <= IMET54_CODED_BITS; n += 64) {
        const uint8_t *in = stripped + IMET54_PAYLOAD_OFFSET_BITS + n;
        for (i = 0; i < 8; i++) {
            for (j = 0; j < 8; j++) {
                deinter[n + 8 * j + i] = in[8 * i + j];
            }
        }
    }

    /* Hamming decode, eight bits at a time into one nibble. */
    for (i = 0; i < IMET54_CODED_BITS / 8; i++) {
        uint8_t *code = deinter + 8 * i;
        uint8_t byte = 0;
        int ecc = ham_correct(code);
        int nib;

        if (ecc < 0) { return -1; }
        corrected += ecc;

        /* The codeword is carried least significant bit first */
        for (j = 0; j < 8; j++) {
            byte |= (uint8_t)((code[j] & 1) << j);
        }

        for (nib = 0; nib < 16; nib++) {
            if (byte == HAM_LUT[nib]) { break; }
        }
        /* Correction above should have landed on a valid codeword. If it did not,
         * the frame is not ours to salvage. */
        if (nib >= 16) { return -1; }

        nibble[i] = (uint8_t)nib;
    }

    /* Two nibbles to a byte, high one first. */
    for (out = 0; out < IMET54_DATA_LEN; out++) {
        dst[out] = (uint8_t)((nibble[2 * out] << 4) | (nibble[2 * out + 1] & 0x0F));
    }

    return corrected;
}

/* CRC32/802.3 with a non-standard final xor, computed over the first 0x34 bytes
 * with each four byte word reversed, and compared against the value stored at
 * 0x34. */
static uint32_t
crc32_802(const uint8_t *msg, int len)
{
    const uint32_t poly = 0x04C11DB7;
    const uint32_t xorout = 0x63D60875;
    uint32_t rem = 0;
    int i, j;

    for (i = 0; i < len; i++) {
        rem ^= (uint32_t)msg[i] << 24;
        for (j = 0; j < 8; j++) {
            if (rem & 0x80000000u) { rem = (rem << 1) ^ poly; }
            else { rem <<= 1; }
        }
    }
    return rem ^ xorout;
}

int
imet54_frame_crc_ok(const uint8_t *frame)
{
    uint8_t swapped[IMET54_POS_CRC32];
    uint32_t expected, actual;
    int i, j;

    expected = ((uint32_t)frame[IMET54_POS_CRC32] << 24) |
               ((uint32_t)frame[IMET54_POS_CRC32 + 1] << 16) |
               ((uint32_t)frame[IMET54_POS_CRC32 + 2] << 8) |
               (uint32_t)frame[IMET54_POS_CRC32 + 3];

    for (i = 0; i < IMET54_POS_CRC32 / 4; i++) {
        for (j = 0; j < 4; j++) {
            swapped[4 * i + j] = frame[4 * i + 3 - j];
        }
    }

    actual = crc32_802(swapped, IMET54_POS_CRC32);
    return actual == expected;
}
