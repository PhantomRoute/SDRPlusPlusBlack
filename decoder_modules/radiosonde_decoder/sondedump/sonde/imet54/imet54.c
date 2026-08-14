#include <include/imet54.h>
#include <stdlib.h>
#include <string.h>
#include "decode/framer.h"
#include "frame.h"
#include "log/log.h"
#include "parser.h"
#include "protocol.h"

/* See include/imet54.h for what these are for. */
volatile int imet54_stat_framed = 0;
volatile int imet54_stat_ecc_fail = 0;
volatile int imet54_stat_crc_fail = 0;
volatile int imet54_stat_ok = 0;
volatile int imet54_stat_last_bad = 0;

struct imet54decoder {
    Framer f;
    /* Two frames' worth - see IMET54_RAW_FRAME_LEN. */
    uint8_t raw_frame[IMET54_RAW_FRAME_LEN];
    uint8_t frame[IMET54_DATA_LEN];
};

__global IMET54Decoder*
imet54_decoder_init(int samplerate)
{
    IMET54Decoder *d = malloc(sizeof(*d));
    if (!d) { return NULL; }

    if (framer_init_gfsk(&d->f, samplerate, IMET54_BAUDRATE, IMET54_FRAME_LEN,
                         IMET54_SYNCWORD, IMET54_SYNC_LEN)) {
        free(d);
        return NULL;
    }
    memset(d->frame, 0, sizeof(d->frame));

    return d;
}

__global void
imet54_decoder_deinit(IMET54Decoder *d)
{
    if (!d) { return; }
    framer_deinit(&d->f);
    free(d);
}

__global ParserStatus
imet54_decode(IMET54Decoder *self, SondeData *dst, const float *src, size_t len)
{
    int errcount;

    switch (framer_read(&self->f, self->raw_frame, src, len)) {
    case PROCEED:
        return PROCEED;
    case PARSED:
        break;
    }

    dst->fields = 0;
    imet54_stat_framed++;

    /* Undo 8N1, the interleaver and the Hamming code. */
    errcount = imet54_frame_decode(self->frame, self->raw_frame);
    imet54_stat_last_bad = errcount;

    /* Beyond about a quarter of the codewords ruined there is no signal here to
     * speak of - the framer matched noise, or the payload is being read from the
     * wrong offset. Counting that separately from a checksum failure is what makes
     * the two distinguishable from outside. */
    if (errcount < 0 || errcount > (IMET54_DATA_LEN * 2) / 4) {
        imet54_stat_ecc_fail++;
        return PARSED;
    }

    /* The CRC covers the first 0x34 bytes, which is everything the parser reads
     * except the humidity sensor temperature. With ruined codewords passed through
     * as zeroes rather than rejected, this is what stops a partly broken frame being
     * reported as telemetry. */
    if (!imet54_frame_crc_ok(self->frame)) {
        imet54_stat_crc_fail++;
        return PARSED;
    }

    imet54_stat_ok++;
    imet54_parse(dst, self->frame);

    return PARSED;
}
