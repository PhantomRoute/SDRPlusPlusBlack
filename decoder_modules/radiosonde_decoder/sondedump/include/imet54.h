#ifndef imet54_h
#define imet54_h

#include "data.h"

typedef struct imet54decoder IMET54Decoder;

/* Diagnostics. Not part of sondedump's API and not something the other sondes have -
 * these exist because "no data" has three quite different causes and no way to tell
 * them apart from the outside:
 *
 *   framed stays 0        - the framer never found the sync word. Wrong frequency,
 *                           wrong channel width, or not an iMet-54.
 *   framed climbs, ecc_fail
 *   climbs with it        - it is finding the sync but the payload does not survive
 *                           Hamming. Alignment or bit order.
 *   framed and ecc fine,
 *   crc_fail climbs       - the frame decoded and the checksum disagrees.
 *   ok climbs             - working.
 *
 * Plain ints written from the decoder thread and read by the UI. They are counters
 * for a human to watch, so a torn read costs nothing. */
extern volatile int imet54_stat_framed;
extern volatile int imet54_stat_ecc_fail;
extern volatile int imet54_stat_crc_fail;
extern volatile int imet54_stat_ok;
/* Ruined codewords in the most recent frame, out of 216. A handful is a weak
 * signal; a couple of hundred means the payload offset is wrong. */
extern volatile int imet54_stat_last_bad;

/**
 * Initialize an iMet-54 frame decoder
 *
 * @param samplerate samplerate of the raw FM-demodulated stream
 * @return an initialized decoder object
 */
IMET54Decoder *imet54_decoder_init(int samplerate);

/**
 * Deinitialize the given decoder
 *
 * @param d decoder to deinit
 */
void imet54_decoder_deinit(IMET54Decoder *d);

/**
 * Decode the next frame in the stream
 *
 * @param d decoder to use
 * @param dst pointer to data struct to fill
 * @param src pointer to raw samples to decode
 * @param len number of samples available
 *
 * @return PROCEED if the src buffer has been fully processed
 *         PARSED  if a frame has been decoded into *dst
 */
ParserStatus imet54_decode(IMET54Decoder *d, SondeData *dst, const float *src, size_t len);

#endif
