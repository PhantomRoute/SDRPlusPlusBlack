#ifndef imet54_parser_h
#define imet54_parser_h

#include <include/data.h>
#include <stdint.h>
#include "protocol.h"

/**
 * Pull the telemetry out of a decoded iMet-54 payload and into dst, setting the
 * field bits for whatever was actually present and plausible.
 *
 * @param dst   destination, its fields bitmask already cleared
 * @param frame the IMET54_DATA_LEN byte payload
 *
 * @return 0 if the position parsed and passed its range checks, -1 otherwise. A
 *         frame can still carry usable temperature with an unusable position, so a
 *         negative return does not mean nothing was written.
 */
int imet54_parse(SondeData *dst, const uint8_t *frame);

#endif
