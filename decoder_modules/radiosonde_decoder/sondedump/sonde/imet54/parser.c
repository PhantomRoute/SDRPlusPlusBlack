#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "parser.h"

static uint32_t
u4be(const uint8_t *b)
{
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

static int32_t
i4be(const uint8_t *b)
{
    return (int32_t)u4be(b);
}

/* The floats are sent big endian, so they cannot simply be read over. */
static float
f4be(const uint8_t *b)
{
    uint32_t raw = u4be(b);
    float out;
    memcpy(&out, &raw, sizeof(out));
    return out;
}

/* Position comes as DDMM.mmmm scaled by a million, which is degrees and minutes run
 * together rather than a decimal degree. 4530.5 is 45 degrees 30.5 minutes, which is
 * 45.508 degrees and emphatically not 45.305. */
static float
ddmm_to_deg(int32_t raw)
{
    double ddmm = raw / 1e6;
    double deg = (double)(int)ddmm;
    double minutes = (ddmm - deg) * 100.0 / 60.0;
    return (float)(deg + minutes);
}

int
imet54_parse(SondeData *dst, const uint8_t *frame)
{
    uint32_t serial;
    int32_t raw;
    float lat, lon, alt, temp, rh;
    int hour, minute, second, ms;
    int position_ok = 1;
    int have_ptu = 0;

    /* The caller hands us an uninitialised SondeData off the stack and only ever
     * looks at the fields the bitmask claims are present - but the bitmask is per
     * group, not per field. Setting DATA_PTU therefore promises temperature,
     * humidity *and* pressure, and anything left unwritten is read as whatever was
     * on the stack. Pressure is the one that bites: this sonde has no barometer, so
     * nothing below ever writes it, and the caller's "derive it from altitude if it
     * is not positive" fallback keeps any stack garbage that happens to be positive
     * and reports it as a measurement. */
    dst->temp = -273.15f;   /* absolute zero: obviously not a reading, if one is missed */
    dst->rh = 0.0f;
    dst->pressure = 0.0f;   /* no barometer; the caller derives it from altitude */
    dst->calib_percent = 0.0f;

    /* Serial number */
    serial = u4be(frame + IMET54_POS_SN);
    dst->fields |= DATA_SERIAL;
    snprintf(dst->serial, sizeof(dst->serial), "IMET-%u", serial);

    /* Time of day, as HHMMSSmmm. The sonde does not send a date, so the seconds
     * since the epoch cannot be filled in from this alone - only the time of day
     * is meaningful, and that is what goes out. */
    raw = i4be(frame + IMET54_POS_GPSTIME);
    if (raw >= 0 && raw <= 235959999) {
        ms = raw % 1000;
        second = (raw / 1000) % 100;
        minute = (raw / 100000) % 100;
        hour = (raw / 10000000) % 100;
        (void)ms;

        if (hour < 24 && minute < 60 && second < 60) {
            /* Today's UTC date with the sonde's time of day. Done as arithmetic on
             * the epoch rather than through gmtime and timegm: gmtime hands back a
             * pointer to one shared static struct, and this runs on a decoder
             * thread. Unix time has no leap seconds, so a UTC day is exactly 86400
             * seconds and this is exact.
             *
             * Wrong for a few seconds either side of midnight UTC, which is the
             * price of the sonde sending a time of day and no date at all. */
            time_t now = time(NULL);
            time_t midnight = now - (now % 86400);
            dst->time = midnight + hour * 3600 + minute * 60 + second;
            dst->fields |= DATA_TIME;
        }
    }

    /* Position */
    lat = ddmm_to_deg(i4be(frame + IMET54_POS_GPSLAT));
    lon = ddmm_to_deg(i4be(frame + IMET54_POS_GPSLON));
    alt = i4be(frame + IMET54_POS_GPSALT) / 10.0f;

    if (lat < -90.0f || lat > 90.0f) { position_ok = 0; }
    if (lon < -180.0f || lon > 180.0f) { position_ok = 0; }
    if (alt < -400.0f || alt > 60000.0f) { position_ok = 0; }

    if (position_ok) {
        dst->fields |= DATA_POS;
        dst->lat = lat;
        dst->lon = lon;
        dst->alt = alt;
    }

    /* Temperature and humidity. A field the sonde has no reading for is filled with
     * 1e9 rather than left empty, and 1e9 degrees would otherwise be reported as a
     * measurement. */
    if (u4be(frame + IMET54_POS_T) != IMET54_NO_DATA) {
        temp = f4be(frame + IMET54_POS_T);
        if (temp > -120.0f && temp < 80.0f) {
            dst->temp = temp;
            have_ptu = 1;
        }
    }

    if (u4be(frame + IMET54_POS_RH) != IMET54_NO_DATA) {
        rh = f4be(frame + IMET54_POS_RH);
        if (rh < 0.0f) { rh = 0.0f; }
        if (rh > 100.0f) { rh = 100.0f; }
        dst->rh = rh;
        have_ptu = 1;
        /* The humidity sensor lags badly in the cold, so what it reports high up is
         * closer to a trend than a reading. Nothing to be done about that here. */
    }

    /* Calibration is not transmitted piecemeal the way the Vaisala sondes do it, so
     * there is nothing to count up to: whatever arrived is all there is. */
    if (have_ptu) {
        dst->fields |= DATA_PTU;
        dst->calib_percent = 100.0f;
    }

    return position_ok ? 0 : -1;
}
