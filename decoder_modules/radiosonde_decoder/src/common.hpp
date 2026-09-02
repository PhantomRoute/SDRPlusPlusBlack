#pragma once
#include <ctime>   // time_t, used below. Added: upstream relied on it arriving via
                   // another header, which it does not do on every toolchain.
#include <string>

class SondeFullData {
public:
	SondeFullData() { init(); }
	void init() {
		serial = "";
		seq = time = burstkill = 0;
		lat = lon = alt = 0;
		spd = hdg = climb = 0;
		temp = rh = dewpt = pressure = 0;
		calibrated = false;
		auxData = "";
		seenFields = 0;
		haveTemp = false;
		pressureMeasured = false;
		velocityDerived = false;
	};

	std::string serial;         /* Serial number */
	int seq;                    /* Frame sequence number */
	time_t time;                /* Onboard time */
	int burstkill;              /* Time to shutdown, -1 if inactive */
	float lat, lon, alt;        /* Latitude (degrees), longitude (degrees) altitude (meters) */
	float spd, hdg, climb;      /* Speed (m/s), heading (degrees), climb (m/s) */
	float temp, rh;             /* Temperature (degrees C), relative humidity (%) */
	float dewpt, pressure;      /* Dew point (degrees C), pressure (hPa) */
	bool calibrated;            /* Whether all the calibration data has been received */
	float calib_percent;        /* Calibration status (0-100) */
	std::string auxData;        /* Auxiliary freeform data */

	/* Which groups a parser has actually supplied, as a DataBitmask, accumulated over
	 * the flight. The struct is reused for every frame and a parser only writes what
	 * its sonde carries, so without this there is no way to tell a real reading of
	 * zero from a field nothing has ever filled in. */
	unsigned seenFields;

	/* Whether a temperature has ever actually arrived. Separate from seenFields
	 * because that bitmask is per group: DATA_PTU promises temperature, humidity and
	 * pressure together, and the iMet-54 sets it for a frame carrying only humidity.
	 * A consumer asking "is this temperature real" has nothing else to go on. */
	bool haveTemp;

	/* False when pressure was worked out from altitude rather than read off a
	 * barometer. Only set for the derivation done in Decoder::run - several of the
	 * vendored parsers derive it themselves and hand it back indistinguishable from a
	 * measurement, so false is reliable and true is only "not derived here". */
	bool pressureMeasured;

	/* Set when spd/hdg/climb came from differencing the GPS track rather than from the
	 * sonde. The iMet-54 sends no velocity at all. */
	bool velocityDerived;
};
