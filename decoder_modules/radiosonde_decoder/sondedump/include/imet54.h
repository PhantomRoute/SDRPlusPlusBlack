#ifndef imet54_h
#define imet54_h

#include "data.h"

typedef struct imet54decoder IMET54Decoder;

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
