#include <config.h>
#include <core.h>
#include <ctm.h>   // currentTimeMillis
#include <gui/gui.h>
#include <gui/style.h>
#include <imgui.h>
#include <utils/flog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "main.h"

SDRPP_MOD_INFO{
    /* Name:            */ "radiosonde_decoder",
    /* Description:     */ "Decodes weather balloon radiosondes",
    /* Author:          */ "dbdexter-dev, PhantomRoute",
    /* Version:         */ 0, 10, 0,
    /* Max instances    */ -1
};

ConfigManager config;

namespace {
    // Sondes are assigned on a 1 kHz grid in the 400-406 MHz band.
    const double SNAP_INTERVAL = 1000.0;
    // What the decoders expect. Every sonde type is resampled to this.
    const double DECODER_SAMPLE_RATE = 48000.0;
    // How much of the climb to keep. A flight lasts around two hours, and one frame
    // a second over that is a few thousand points - nothing worth trimming.
    const size_t MAX_TRACK = 16384;

    const char* COMPASS[] = { "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
                              "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW" };

    const char* compassPoint(double bearingDeg) {
        while (bearingDeg < 0.0) { bearingDeg += 360.0; }
        int idx = (int)((fmod(bearingDeg, 360.0) / 22.5) + 0.5) % 16;
        return COMPASS[idx];
    }

    std::string fmtAltitude(double m) {
        char buf[64];
        if (fabs(m) >= 1000.0) { snprintf(buf, sizeof buf, "%.2f km", m / 1000.0); }
        else { snprintf(buf, sizeof buf, "%.0f m", m); }
        return std::string(buf);
    }

    std::string fmtDistance(double km) {
        char buf[64];
        if (km < 1.0) { snprintf(buf, sizeof buf, "%.0f m", km * 1000.0); }
        else { snprintf(buf, sizeof buf, "%.1f km", km); }
        return std::string(buf);
    }
}

RadiosondeDecoderModule::RadiosondeDecoderModule(std::string name) {
    this->name = name;

    bool created = false;
    config.acquire();
    if (!config.conf.contains(name)) {
        config.conf[name]["sondeType"] = 0;
        config.conf[name]["logEnabled"] = false;
        config.conf[name]["frameLogEnabled"] = false;
        config.conf[name]["logPath"] = (std::string(core::getRoot()) + "/radiosonde.gpx");
        created = true;
    }
    selectedType = config.conf[name].contains("sondeType") ? (int)config.conf[name]["sondeType"] : 0;
    logEnabled = config.conf[name].contains("logEnabled") && config.conf[name]["logEnabled"].is_boolean()
                     ? (bool)config.conf[name]["logEnabled"]
                     : false;
    frameLogEnabled = config.conf[name].contains("frameLogEnabled") && config.conf[name]["frameLogEnabled"].is_boolean()
                          ? (bool)config.conf[name]["frameLogEnabled"]
                          : false;
    std::string path = config.conf[name].contains("logPath") && config.conf[name]["logPath"].is_string()
                           ? config.conf[name]["logPath"].get<std::string>()
                           : (std::string(core::getRoot()) + "/radiosonde.gpx");
    config.release(created);

    selectedType = std::clamp<int>(selectedType, 0, (int)(sizeof(types) / sizeof(types[0])) - 1);
    snprintf(logPath, sizeof logPath, "%s", path.c_str());

    double bw = types[selectedType].bandwidth;

    vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0, bw, bw, bw, bw, true);
    vfo->setSnapInterval(SNAP_INTERVAL);

    // lowPass false: the resampler that follows does the band limiting. highPass
    // false and it matters - the frequency shift keying these sondes use carries
    // information right down to DC, and taking that out stops them decoding.
    // (This fifth argument does not exist in upstream SDR++; it does here.)
    fmDemod.init(vfo->output, bw, bw / 2.0, false, false);
    resampler.init(&fmDemod.out, bw, DECODER_SAMPLE_RATE);

    // Every decoder is wired up and left stopped. They all sit on the resampler
    // output, so switching type is a stop and a start rather than a rebuild.
    rs41decoder.init(&resampler.out, DECODER_SAMPLE_RATE, sondeDataHandler, this);
    dfm09decoder.init(&resampler.out, DECODER_SAMPLE_RATE, sondeDataHandler, this);
    ims100decoder.init(&resampler.out, DECODER_SAMPLE_RATE, sondeDataHandler, this);
    m10decoder.init(&resampler.out, DECODER_SAMPLE_RATE, sondeDataHandler, this);
    imet4decoder.init(&resampler.out, DECODER_SAMPLE_RATE, sondeDataHandler, this);
    c50decoder.init(&resampler.out, DECODER_SAMPLE_RATE, sondeDataHandler, this);
    mrzn1decoder.init(&resampler.out, DECODER_SAMPLE_RATE, sondeDataHandler, this);
    imet54decoder.init(&resampler.out, DECODER_SAMPLE_RATE, sondeDataHandler, this);

    // selectType builds the channel at the right width and starts the decoder, so
    // there is nothing to start here first.
    selectType(selectedType);
    if (logEnabled) { logEnabled = requestLog() || logAskingOverwrite; }
    if (frameLogEnabled) { frameLogEnabled = openFrameLog(); }

    enabled = true;
    gui::menu.registerEntry(name, menuHandler, this, this);
}

RadiosondeDecoderModule::~RadiosondeDecoderModule() {
    gui::menu.removeEntry(name);
    if (enabled) { disable(); }
    closeLog();
    closeFrameLog();
}

void RadiosondeDecoderModule::postInit() {}

void RadiosondeDecoderModule::startDSP() {
    if (dspRunning) { return; }
    fmDemod.start();
    resampler.start();
    dspRunning = true;
}

void RadiosondeDecoderModule::stopDSP() {
    if (!dspRunning) { return; }
    resampler.stop();
    fmDemod.stop();
    dspRunning = false;
}

void RadiosondeDecoderModule::enable() {
    if (enabled) { return; }
    // Same as the constructor: selectType makes the VFO, wires the demodulator to it
    // and starts everything.
    selectType(selectedType);
    enabled = true;
}

void RadiosondeDecoderModule::disable() {
    if (!enabled) { return; }

    if (activeDecoder) { activeDecoder->stop(); }
    activeDecoder = NULL;
    stopDSP();

    if (vfo) {
        sigpath::vfoManager.deleteVFO(vfo);
        vfo = NULL;
    }
    enabled = false;
}

bool RadiosondeDecoderModule::isEnabled() {
    return enabled;
}

void RadiosondeDecoderModule::selectType(int type) {
    const int count = (int)(sizeof(types) / sizeof(types[0]));
    if (type < 0 || type >= count) { return; }

    if (activeDecoder) { activeDecoder->stop(); }
    activeDecoder = NULL;

    selectedType = type;
    config.acquire();
    config.conf[name]["sondeType"] = selectedType;
    config.release(true);

    // A new sonde is a new flight: the serial, the track and the frame count all
    // belong to the one that was being decoded a moment ago.
    {
        std::lock_guard<std::mutex> lck(dataMtx);
        lastData.init();
        everDecoded = false;
        framesDecoded = 0;
        track.clear();
        // A different sonde is a different clock. Keeping the old reference would have
        // the guard reject the whole of the next flight until it gave up and
        // rebaselined, which is exactly the wrong way round.
        lastGoodSondeTime = 0;
        consecutiveTimeRejects = 0;
        framesTimeRejected = 0;
    }

    double bw = types[selectedType].bandwidth;

    // The channel has to be rebuilt at the new width. Stopping the demodulator first
    // means nothing is reading the VFO's output stream while it is deleted.
    stopDSP();
    if (vfo) {
        sigpath::vfoManager.deleteVFO(vfo);
        vfo = NULL;
    }
    vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0, bw, bw, bw, bw, true);
    vfo->setSnapInterval(SNAP_INTERVAL);

    fmDemod.setInput(vfo->output);
    fmDemod.setSamplerate(bw);
    fmDemod.setBandwidth(bw / 2.0);
    resampler.setInSamplerate(bw);
    startDSP();

    activeDecoder = types[selectedType].decoder;
    if (activeDecoder) { activeDecoder->start(); }
}

// Speed, heading and climb from the change between two consecutive fixes.
//
// The iMet-54 carries no velocity of any kind - checked against a live flight by
// correlating every offset in its 108 byte payload, as int16, uint16, int32 and
// float32 in both byte orders, against the speed and climb worked out from its own
// GPS track: nothing reached even a 0.9 correlation. So for that sonde these three
// numbers are zero for the whole flight unless they are derived, which is what every
// other tracker does with it.
void RadiosondeDecoderModule::deriveVelocity(SondeFullData& d) {
    const double DEG = 3.14159265358979323846 / 180.0;
    long long now = lastFrameTime;
    if (havePrevFix) {
        double dt = (double)(now - prevFixTime) / 1000.0;
        // One frame a second is the usual rate. Shorter than half of that and GPS
        // noise swamps the difference; longer than a few seconds and the balloon has
        // turned in between, so the straight line between the two fixes is not the
        // path it took. Outside that window, leave the figures alone rather than
        // publish one that is wrong.
        if (dt >= 0.4 && dt <= 8.0) {
            // Metres per degree of latitude, and of longitude at this latitude. Good
            // to a fraction of a percent over the second between fixes, which is far
            // below the GPS noise this is differentiating.
            const double MPERDEG = 111320.0;
            double dn = ((double)d.lat - prevLat) * MPERDEG;
            double de = ((double)d.lon - prevLon) * MPERDEG * cos((double)prevLat * DEG);
            d.spd = (float)(sqrt(dn * dn + de * de) / dt);
            d.climb = (float)(((double)d.alt - prevAlt) / dt);
            if (dn != 0.0 || de != 0.0) {
                double h = atan2(de, dn) / DEG;
                if (h < 0.0) { h += 360.0; }
                d.hdg = (float)h;
            }
            d.velocityDerived = true;
        }
    }
    prevLat = d.lat;
    prevLon = d.lon;
    prevAlt = d.alt;
    prevFixTime = now;
    havePrevFix = true;
}

// Runs on the decoder's worker thread, not the UI thread.
void RadiosondeDecoderModule::sondeDataHandler(SondeFullData* data, void* ctx) {
    RadiosondeDecoderModule* _this = (RadiosondeDecoderModule*)ctx;
    if (data == NULL) { return; }

    std::lock_guard<std::mutex> lck(_this->dataMtx);
    _this->everDecoded = true;
    _this->framesDecoded++;
    if (data->seq != 0) { _this->sondeSendsSeq = true; }
    _this->lastFrameTime = currentTimeMillis();

    // Derive into our own copy, never into *data.
    //
    // The decoder hands back a pointer to a SondeFullData it keeps and reuses for
    // every frame, and a parser only writes the fields its sonde actually carries.
    // Writing the derived velocity back into it therefore poisons the very test used
    // to decide whether the decoder supplied one: the next frame arrives still
    // holding last frame's numbers, the test says "already set", and the figures
    // freeze at their first value for the rest of the flight. Seen doing exactly that
    // on a live iMet-54 - 5.65 m/s on every frame while the balloon accelerated.
    _this->lastData = *data;
    if (data->spd == 0.0f && data->climb == 0.0f && data->hdg == 0.0f &&
        (data->lat != 0.0f || data->lon != 0.0f)) {
        _this->deriveVelocity(_this->lastData);
    }

    bool timeAccepted = _this->timeIsBelievable(data->time);

    // Written for every frame, including the ones with no position and the ones whose
    // time was thrown out, because those are the frames worth having when something
    // needs explaining. The module's copy, so it carries the derived velocity.
    if (_this->frameLogFile) { _this->writeFrameLogRow(_this->lastData, timeAccepted); }

    // Only keep a point once there is a position to keep. A frame can decode with
    // valid telemetry before the GPS has a fix, and plotting 0,0 puts the balloon in
    // the Atlantic.
    if (data->lat != 0.0f || data->lon != 0.0f) {
        SondeFix fix;
        fix.time = _this->lastFrameTime;
        fix.lat = data->lat;
        fix.lon = data->lon;
        fix.alt = data->alt;
        _this->track.push_back(fix);
        while (_this->track.size() > MAX_TRACK) { _this->track.pop_front(); }

        if (_this->logFile) { _this->writeLogPoint(_this->lastData, timeAccepted); }
    }
}

bool RadiosondeDecoderModule::bearingFromOperator(const SondeFullData& d, utils::BearingDistance& bd,
                                                  double& elevationDeg) {
    utils::LatLng home = utils::gridToLatLng(sigpath::iqFrontEnd.operatorLocation);
    if (!home.isValid()) { return false; }
    if (d.lat == 0.0f && d.lon == 0.0f) { return false; }

    utils::LatLng there;
    there.lat = d.lat;
    there.lon = d.lon;
    bd = utils::bearingDistance(home, there);

    // Straight line angle above the horizon, ignoring refraction and the curve of
    // the earth. Good enough to point an antenna, which is all it is for.
    // Not M_PI: it is not in standard C++ and MSVC only defines it behind
    // _USE_MATH_DEFINES, which is exactly the sort of thing that builds everywhere
    // except on the one platform nobody tests on.
    const double RAD_TO_DEG = 57.29577951308232;
    double groundM = bd.distance * 1000.0;
    elevationDeg = (groundM > 1.0) ? (atan2((double)d.alt, groundM) * RAD_TO_DEG) : 90.0;
    return true;
}

// Whether to believe the onboard clock in this frame.
//
// A sonde's own time is normally the best clock in the system - it comes from GPS -
// but a frame can pass its checksum with the time field wrong, and one wrong reading
// is enough to make a track file that mapping software sorts into nonsense.
//
// The test is continuity against the last time that was believed, not agreement with
// this computer's clock. Comparing against the wall clock would be simpler and would
// break the moment anyone replayed a recording, where the sonde's time is quite
// properly hours or years away from now.
//
// Backwards is barely tolerated: frames arrive in order and a repeat is worth a second
// or two, no more. Forwards is tolerated generously, because losing the signal for a
// while and picking it up again is an ordinary part of a flight, not an error.
bool RadiosondeDecoderModule::timeIsBelievable(time_t t) {
    // How far back a frame may step before it is disbelieved, and how far forward.
    const time_t BACK_TOLERANCE = 3;
    const time_t FORWARD_TOLERANCE = 3600;
    // If this many in a row are rejected, the baseline itself was wrong - the first
    // frame of the flight carried a bad time, or the gap really was longer than an
    // hour - so take the current one and carry on from there. Without this a single
    // bad frame at the start would throw away the timestamps of the whole flight.
    const int REBASELINE_AFTER = 20;

    if (t <= 0) { return false; }

    if (lastGoodSondeTime == 0) {
        lastGoodSondeTime = t;
        consecutiveTimeRejects = 0;
        return true;
    }

    if (t >= lastGoodSondeTime - BACK_TOLERANCE && t <= lastGoodSondeTime + FORWARD_TOLERANCE) {
        lastGoodSondeTime = t;
        consecutiveTimeRejects = 0;
        return true;
    }

    if (++consecutiveTimeRejects >= REBASELINE_AFTER) {
        flog::warn("Radiosonde: onboard clock has disagreed for {0} frames, taking it as the new reference",
                   (int64_t)consecutiveTimeRejects);
        lastGoodSondeTime = t;
        consecutiveTimeRejects = 0;
        return true;
    }

    framesTimeRejected++;
    return false;
}

// ---- GPX logging

// Both of these run on the UI thread while the decoder thread may be part way
// through writeLogPoint, so they take the same lock that guards the data. Neither is
// ever called with it already held, and writeLogPoint does not take it - it is only
// reached from the callback, which does.
// How many points a file already holds, and nothing else - used only to tell the
// operator what they are about to continue or replace.
static int countTrackPoints(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { return 0; }
    int n = 0;
    char buf[4096];
    std::string carry;
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
        carry.append(buf, got);
        size_t at = 0;
        while ((at = carry.find("<trkpt", at)) != std::string::npos) { n++; at += 6; }
        // Keep one character less than the tag, which is just enough to catch one
        // straddling two reads and few enough that a whole tag can never survive into
        // the next pass to be counted twice. Keeping 8 did exactly that and reported
        // one point more than the file held.
        if (carry.size() > 5) { carry = carry.substr(carry.size() - 5); }
    }
    fclose(f);
    return n;
}

// The closing tags, in one place: openLog winds back over them to continue a track,
// writeLogPoint rewrites them after every point, and closeLog leaves them behind.
static const char* GPX_TRAILER = "    </trkseg>\n  </trk>\n</gpx>\n";

// GPX 1.1 has nowhere to put a temperature, so everything past position, altitude and
// time goes in <extensions>. Garmin's is the one schema mapping software actually
// reads, and it has an air temperature; the rest of what a sonde sends has no agreed
// home anywhere, so it goes in a namespace of this project's rather than being
// invented into someone else's and read as something it is not.
#define GPXTPX_NS "http://www.garmin.com/xmlschemas/TrackPointExtension/v1"
#define SONDE_NS "https://github.com/sannysanoff/SDRPlusPlusBrown/radiosonde/v1"

bool RadiosondeDecoderModule::openLog(bool startFresh) {
    closeLog();
    if (logPath[0] == 0) {
        logStatus = "No file chosen";
        return false;
    }
    std::lock_guard<std::mutex> lck(dataMtx);

    // Continue an existing track rather than destroying it.
    //
    // This used to open "wb", which truncates. Restarting the program - or just
    // switching this checkbox off and on - therefore threw away everything recorded
    // so far and wrote the header straight back, so the file kept its name and held
    // nothing. It cost a real flight before it was noticed; the only reason that
    // flight survived is that the frame log beside it opens "ab" and had every point.
    //
    // So: never truncate a file that already has something in it. writeLogPoint
    // leaves the closing tags at the end after every frame, so the usual case is to
    // find them and wind back over them. Anything else, append at the end and let the
    // next point put a fresh trailer on.
    bool appending = false;
    logFile = startFresh ? NULL : fopen(logPath, "r+b");
    if (logFile) {
        fseek(logFile, 0, SEEK_END);
        long size = ftell(logFile);
        if (size > 0) {
            appending = true;
            const long trailerLen = (long)strlen(GPX_TRAILER);
            fseek(logFile, 0, SEEK_END);
            if (size > trailerLen) {
                char tail[64];
                fseek(logFile, size - trailerLen, SEEK_SET);
                if (fread(tail, 1, (size_t)trailerLen, logFile) == (size_t)trailerLen &&
                    memcmp(tail, GPX_TRAILER, (size_t)trailerLen) == 0) {
                    fseek(logFile, size - trailerLen, SEEK_SET);
                }
                else {
                    fseek(logFile, 0, SEEK_END);
                }
            }
        }
        else {
            // Existed but empty, so there is nothing to keep.
            fclose(logFile);
            logFile = NULL;
        }
    }

    // A file written before the extensions existed has no declaration for their
    // prefixes, and a prefix with nothing declaring it is not well formed XML - the
    // whole track would stop parsing at the first point this session appended. So
    // look, and if it is an older file, carry the declarations on every element
    // instead. Verbose, but it keeps a continued track readable.
    logInlineNs = false;
    if (logFile && appending) {
        long resume = ftell(logFile);
        char head[2048];
        fseek(logFile, 0, SEEK_SET);
        size_t got = fread(head, 1, (sizeof head) - 1, logFile);
        head[got] = 0;
        logInlineNs = (strstr(head, "xmlns:sonde=") == NULL);
        if (resume >= 0) { fseek(logFile, resume, SEEK_SET); }
    }

    if (!logFile) {
        logFile = fopen(logPath, "wb");
        if (!logFile) {
            logStatus = std::string("Could not open ") + logPath;
            flog::error("Radiosonde: could not open {0}", logPath);
            return false;
        }
    }

    if (!appending) {
    fprintf(logFile,
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<gpx version=\"1.1\" creator=\"SDR++ radiosonde decoder\"\n"
            "     xmlns=\"http://www.topografix.com/GPX/1/1\"\n"
            "     xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"\n"
            "     xmlns:gpxtpx=\"" GPXTPX_NS "\"\n"
            "     xmlns:sonde=\"" SONDE_NS "\"\n"
            "     xsi:schemaLocation=\"http://www.topografix.com/GPX/1/1"
            " http://www.topografix.com/GPX/1/1/gpx.xsd\">\n"
            "  <trk>\n    <name>Radiosonde</name>\n    <trkseg>\n");
    fflush(logFile);
    }

    logStatus = std::string(appending ? "Appending to " : "Writing ") + logPath;
    return true;
}

// Start logging, asking first if that would touch a track that is already there.
bool RadiosondeDecoderModule::requestLog() {
    logAskingOverwrite = false;
    if (logPath[0] == 0) {
        logStatus = "No file chosen";
        return false;
    }
    FILE* probe = fopen(logPath, "rb");
    if (probe) {
        fseek(probe, 0, SEEK_END);
        long size = ftell(probe);
        fclose(probe);
        if (size > 0) {
            // Neither answer is safe to assume, so do not pick one. Nothing is opened
            // and nothing is written until the operator says which they meant.
            logExistingPoints = countTrackPoints(logPath);
            logAskingOverwrite = true;
            logStatus = "Waiting - that file already exists";
            return false;
        }
    }
    return openLog(true);
}

void RadiosondeDecoderModule::closeLog() {
    std::lock_guard<std::mutex> lck(dataMtx);
    if (!logFile) { return; }
    fputs(GPX_TRAILER, logFile);
    fclose(logFile);
    logFile = NULL;
}

void RadiosondeDecoderModule::writeLogPoint(const SondeFullData& d, bool timeAccepted) {
    if (!logFile) { return; }

    char when[64] = { 0 };
    // A point with no time is valid GPX and every reader takes it. A point with a
    // wrong one is worse than useless: anything that sorts by time puts it at the far
    // end of the track and draws a line across the map to reach it.
    if (timeAccepted && d.time > 0) {
        time_t t = (time_t)d.time;
        struct tm* utc = gmtime(&t);
        if (utc) { strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%SZ", utc); }
    }

    // Only what a parser has actually handed over.
    //
    // The frame struct is cumulative and a parser writes only the groups its sonde
    // carries, so an untouched temperature reads as 0.0 - or as the -273.15 the
    // iMet-54 parser writes to mean "this frame missed it". Either one written to the
    // file is a plausible looking reading that nothing measured, which is worse than
    // leaving the point without a temperature at all.
    const bool havePTU = (d.seenFields & DATA_PTU) != 0 && d.temp > -273.0f;
    const bool haveVel = (d.seenFields & DATA_SPEED) != 0 || d.velocityDerived;
    const bool haveAny = havePTU || haveVel || d.pressure > 0.0f;

    fprintf(logFile, "      <trkpt lat=\"%.6f\" lon=\"%.6f\">\n        <ele>%.1f</ele>\n",
            d.lat, d.lon, d.alt);
    if (when[0]) { fprintf(logFile, "        <time>%s</time>\n", when); }

    // The same numbers twice, because GPX readers fall into two camps. <desc> is free
    // text that every viewer shows when a point is clicked, so it is the one a person
    // reads; the extensions after it are the one a program reads. Element order here
    // is fixed by the GPX schema - ele, time, desc, then extensions last.
    if (haveAny) {
        std::string desc;
        char part[160];
        if (havePTU) {
            snprintf(part, sizeof part, "%.1f C, %.0f%% RH, dew point %.1f C",
                     (double)d.temp, (double)d.rh, (double)d.dewpt);
            desc += part;
        }
        if (d.pressure > 0.0f) {
            snprintf(part, sizeof part, "%s%.1f hPa%s", desc.empty() ? "" : ", ",
                     (double)d.pressure, d.pressureMeasured ? "" : " (from altitude)");
            desc += part;
        }
        if (haveVel) {
            snprintf(part, sizeof part, "%s%.1f m/s at %.0f deg, climb %+.1f m/s%s",
                     desc.empty() ? "" : ", ", (double)d.spd, (double)d.hdg,
                     (double)d.climb, d.velocityDerived ? " (from GPS)" : "");
            desc += part;
        }
        // Every value in it is a number this module formatted, so there is nothing in
        // here that needs escaping. The free text fields the sonde sends - serial and
        // aux - deliberately stay out of the track for that reason; they are in the
        // frame CSV beside it.
        fprintf(logFile, "        <desc>%s</desc>\n", desc.c_str());

        const char* ns = logInlineNs
                             ? " xmlns:gpxtpx=\"" GPXTPX_NS "\" xmlns:sonde=\"" SONDE_NS "\""
                             : "";
        fprintf(logFile, "        <extensions%s>\n", ns);
        if (havePTU) {
            fprintf(logFile,
                    "          <gpxtpx:TrackPointExtension>"
                    "<gpxtpx:atemp>%.2f</gpxtpx:atemp>"
                    "</gpxtpx:TrackPointExtension>\n"
                    "          <sonde:rh>%.1f</sonde:rh>\n"
                    "          <sonde:dewpt>%.2f</sonde:dewpt>\n",
                    (double)d.temp, (double)d.rh, (double)d.dewpt);
        }
        if (d.pressure > 0.0f) {
            // Only the derivation this module does is flagged. Several of the vendored
            // parsers work pressure out from altitude inside themselves and return it
            // indistinguishable from a barometer reading, so the attribute being
            // absent is not a promise that anything measured it.
            fprintf(logFile, "          <sonde:pressure%s>%.2f</sonde:pressure>\n",
                    d.pressureMeasured ? "" : " derived=\"altitude\"", (double)d.pressure);
        }
        if (haveVel) {
            fprintf(logFile,
                    "          <sonde:speed%s>%.2f</sonde:speed>\n"
                    "          <sonde:heading>%.1f</sonde:heading>\n"
                    "          <sonde:climb>%.2f</sonde:climb>\n",
                    d.velocityDerived ? " derived=\"gps\"" : "",
                    (double)d.spd, (double)d.hdg, (double)d.climb);
        }
        if (d.burstkill > 0) {
            fprintf(logFile, "          <sonde:burstkill>%d</sonde:burstkill>\n", d.burstkill);
        }
        fprintf(logFile, "        </extensions>\n");
    }

    fprintf(logFile, "      </trkpt>\n");

    // Close the document, flush, then wind back over the closing tags so the next
    // point overwrites them. The file on disk is therefore valid GPX after every
    // single frame - pull the power out mid flight and what was written still opens.
    long resume = ftell(logFile);
    fputs(GPX_TRAILER, logFile);
    fflush(logFile);
    if (resume >= 0) { fseek(logFile, resume, SEEK_SET); }
}

// ---- Frame log
//
// One row per decoded frame, next to the GPX. Every field the decoder produces, the
// receiver's own clock, and whether the onboard time was believed - so a flight can be
// replayed against a change to the parsing rather than waiting for the next launch.
//
// It is the parsed frame rather than the bytes off the air: the vendored decoders hand
// back a filled-in SondeData and never expose the frame they built it from, so this is
// as raw as anything gets on this side of them. It is enough to re-run everything this
// module does with the values it was given - which is where the timestamp fault was.

// Beside the track rather than in a place of its own, so the two files from a flight
// stay together. radiosonde.gpx gives radiosonde.frames.csv.
static std::string frameLogPathFor(const std::string& gpxPath) {
    std::string base = gpxPath;
    size_t slash = base.find_last_of("/\\");
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        base.erase(dot);
    }
    return base + ".frames.csv";
}

bool RadiosondeDecoderModule::openFrameLog() {
    closeFrameLog();
    std::string path = frameLogPathFor(logPath);
    // Appended, not truncated. Losing the signal and starting again is normal, and
    // the point of the file is that nothing decoded goes missing.
    bool existed = false;
    {
        FILE* probe = fopen(path.c_str(), "rb");
        if (probe) {
            existed = true;
            fclose(probe);
        }
    }
    frameLogFile = fopen(path.c_str(), "ab");
    if (!frameLogFile) {
        flog::error("Radiosonde: could not open frame log '{0}'", path);
        return false;
    }
    if (!existed) {
        fprintf(frameLogFile,
                "rx_unix_ms,serial,seq,sonde_time,time_accepted,lat,lon,alt,"
                "spd,hdg,climb,temp,rh,dewpt,pressure,calibrated,calib_percent,burstkill,aux\r\n");
    }
    fflush(frameLogFile);
    return true;
}

void RadiosondeDecoderModule::closeFrameLog() {
    if (!frameLogFile) { return; }
    fclose(frameLogFile);
    frameLogFile = NULL;
}

// Quoted the same way the frequency manager's exporter does it, because a serial or an
// aux field is free text off the air and there is nothing stopping it holding a comma.
static std::string frameCsvEscape(const std::string& in) {
    bool needsQuote = false;
    for (char c : in) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') {
            needsQuote = true;
            break;
        }
    }
    if (!needsQuote) { return in; }
    std::string out;
    out.reserve(in.size() + 2);
    out.push_back('"');
    for (char c : in) {
        if (c == '"') { out.push_back('"'); }
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

void RadiosondeDecoderModule::writeFrameLogRow(const SondeFullData& d, bool timeAccepted) {
    if (!frameLogFile) { return; }
    // sonde_time is written as the raw seconds the frame carried, not as a formatted
    // date - the whole point is to keep what arrived, including the value that was
    // rejected, so the guard can be re-run over it.
    fprintf(frameLogFile,
            "%lld,%s,%d,%lld,%d,%.6f,%.6f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%.1f,%d,%s\r\n",
            (long long)currentTimeMillis(),
            frameCsvEscape(d.serial).c_str(),
            d.seq,
            (long long)d.time,
            timeAccepted ? 1 : 0,
            (double)d.lat, (double)d.lon, (double)d.alt,
            (double)d.spd, (double)d.hdg, (double)d.climb,
            (double)d.temp, (double)d.rh, (double)d.dewpt, (double)d.pressure,
            d.calibrated ? 1 : 0, (double)d.calib_percent,
            d.burstkill,
            frameCsvEscape(d.auxData).c_str());
    // Flushed every frame for the same reason the GPX is: a flight that ends with the
    // program being killed should still have everything it decoded.
    fflush(frameLogFile);
}

// ---- UI

void RadiosondeDecoderModule::menuHandler(void* ctx) {
    RadiosondeDecoderModule* _this = (RadiosondeDecoderModule*)ctx;
    if (!_this->enabled) { style::beginDisabled(); }

    float menuWidth = ImGui::GetContentRegionAvail().x;

    ImGui::LeftLabel("Sonde");
    ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
    if (ImGui::BeginCombo(("##radiosonde_type_" + _this->name).c_str(), _this->types[_this->selectedType].name)) {
        for (int i = 0; i < (int)(sizeof(_this->types) / sizeof(_this->types[0])); i++) {
            bool isSelected = (i == _this->selectedType);
            if (ImGui::Selectable(_this->types[i].name, isSelected) && !isSelected) {
                _this->selectType(i);
            }
            if (isSelected) { ImGui::SetItemDefaultFocus(); }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        style::tooltip("Which family of sonde to decode. They are not interchangeable, and there is\n"
                          "no auto detection: if you do not know, RS41 is the most widely flown, and\n"
                          "the channel width changes with the choice so a wrong pick usually looks\n"
                          "obviously wrong on the waterfall.\n"
                          "\n"
                          "The two InterMet entries are different radios, not settings of one: the\n"
                          "iMet-1/4 is 1200 baud audio shift keying, the iMet-54 is 4800 baud\n"
                          "frequency shift keying in a wider channel. Neither will decode the other.");
    }

    _this->drawTelemetry();
    _this->drawTrack();
    _this->drawLogging();

    if (!_this->enabled) { style::endDisabled(); }
}

void RadiosondeDecoderModule::drawTelemetry() {
    // Copied out under the lock and then drawn, so the panel cannot be holding the
    // decoder's thread off while it lays out text.
    SondeFullData d;
    bool have;
    int frames;
    double age;
    bool hasSeq;
    {
        std::lock_guard<std::mutex> lck(dataMtx);
        d = lastData;
        have = everDecoded;
        frames = framesDecoded;
        hasSeq = sondeSendsSeq;
        age = have ? ((double)(currentTimeMillis() - lastFrameTime) / 1000.0) : 0.0;
    }

    ImGui::SectionHeader("SONDE");

    if (!have) {
        ImGui::TextDisabled("Nothing decoded yet");
        ImGui::TextDisabled("Tune the marker onto the signal");

        // For the iMet-54 only, because it is the one decoder here that is ours and
        // the only one whose silence has no other explanation to hand. Says which of
        // the three stages is failing instead of leaving it to guesswork.
        if (types[selectedType].decoder == &imet54decoder) {
            ImGui::Separator();
            ImGui::TextDisabled("Framed    %d", imet54_stat_framed);
            if (ImGui::IsItemHovered()) { style::tooltip("Times the sync word was found. Zero means it is not seeing the signal at\nall - check the frequency, and that the channel covers the whole of it."); }
            ImGui::TextDisabled("FEC fail  %d", imet54_stat_ecc_fail);
            if (ImGui::IsItemHovered()) { style::tooltip("Framed, but the payload did not survive error correction. If this tracks\nFramed one for one, the payload is being read from the wrong offset."); }
            ImGui::TextDisabled("CRC fail  %d", imet54_stat_crc_fail);
            if (ImGui::IsItemHovered()) { style::tooltip("Error correction succeeded and the checksum still disagreed."); }
            ImGui::TextDisabled("Bad words %d / 216", imet54_stat_last_bad);
            if (ImGui::IsItemHovered()) {
                style::tooltip("Codewords in the last frame that error correction could not repair.\n"
                                  "A handful is a weak signal and the checksum will sort it out.\n"
                                  "A hundred or more means the payload is being read from the wrong\n"
                                  "place - random bytes almost never land on a valid codeword.");
            }
        }
        return;
    }

    ImGui::Text("Serial    %s", d.serial.empty() ? "-" : d.serial.c_str());
    if (ImGui::IsItemHovered()) { style::tooltip("Printed on the sonde. This is what identifies the flight on tracking sites."); }

    // Only the sonde's own counter when there is one. Sondes that send none - the
    // iMet-54 among them - would otherwise show "Frame 0" for the whole flight, which
    // reads as a measurement rather than as an absence.
    if (hasSeq) {
        ImGui::Text("Frame     %d", d.seq);
        if (ImGui::IsItemHovered()) { style::tooltip("Sequence number from the sonde, and %d frames decoded since it was tuned", frames); }
    }
    else {
        ImGui::Text("Frames    %d", frames);
        if (ImGui::IsItemHovered()) {
            style::tooltip("Frames decoded since this sonde was tuned. This type sends no sequence\n"
                           "number of its own, so there is nothing to count gaps against.");
        }
    }

    if (d.time > 0) {
        char when[64] = "-";
        time_t t = (time_t)d.time;
        struct tm* utc = gmtime(&t);
        if (utc) { strftime(when, sizeof when, "%H:%M:%S", utc); }
        ImGui::Text("Onboard   %s UTC", when);
    }
    else {
        ImGui::Text("Onboard   -");
    }

    // A sonde that has stopped being decoded goes on showing its last frame, so the
    // age of that frame has to be visible or the panel looks live when it is not.
    if (age > 10.0) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Last      %.0f s ago", age);
        if (ImGui::IsItemHovered()) { style::tooltip("Nothing has decoded recently. Everything above and below is from that frame."); }
    }
    else {
        ImGui::TextDisabled("Last      %.0f s ago", age);
    }

    if (!d.calibrated) {
        ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.0f, 1.0f), "Calibrat. %.0f%%", d.calib_percent);
        if (ImGui::IsItemHovered()) {
            style::tooltip("The sonde sends its calibration a piece at a time, over a minute or two.\n"
                              "Until it is complete the temperature and humidity below are approximate.");
        }
    }
    else {
        ImGui::TextDisabled("Calibrat. complete");
    }

    if (d.burstkill > 0) {
        ImGui::Text("Shutdown  in %d s", d.burstkill);
        if (ImGui::IsItemHovered()) { style::tooltip("The sonde is on a timer and will switch itself off"); }
    }

    ImGui::SectionHeader("POSITION");

    // Both rows are drawn whether or not there is a fix, rather than collapsing to a
    // single "no fix" line: the fix arrives part way through the first minute, and
    // everything below here would jump up a line when it did.
    bool haveFix = (d.lat != 0.0f || d.lon != 0.0f);
    if (haveFix) {
        ImGui::Text("Lat       %.5f", d.lat);
        ImGui::Text("Lon       %.5f", d.lon);
    }
    else {
        ImGui::TextDisabled("Lat       waiting for GPS");
        ImGui::TextDisabled("Lon       waiting for GPS");
    }
    ImGui::Text("Altitude  %s", fmtAltitude(d.alt).c_str());

    if (fabs(d.climb) < 0.5f) { ImGui::Text("Climb     level"); }
    else if (d.climb > 0.0f) { ImGui::Text("Climb     up %.1f m/s", d.climb); }
    else { ImGui::Text("Climb     down %.1f m/s", -d.climb); }
    if (ImGui::IsItemHovered()) {
        style::tooltip("Around 5 m/s up is a normal ascent. A sharp change to a fast descent is the\n"
                          "balloon bursting, usually somewhere above 25 km.");
    }

    ImGui::Text("Ground    %.0f km/h, %s", d.spd * 3.6f, compassPoint(d.hdg));
    if (ImGui::IsItemHovered()) { style::tooltip("How fast the wind is carrying it, and which way it is going"); }

    // Where to point, from the grid square in the Source menu.
    utils::BearingDistance bd;
    double elevation = 0.0;
    if (bearingFromOperator(d, bd, elevation)) {
        ImGui::Text("From you  %s %s", fmtDistance(bd.distance).c_str(), compassPoint(bd.bearing));
        if (ImGui::IsItemHovered()) {
            style::tooltip("Distance and direction from your grid square, and %.0f degrees above the\n"
                              "horizon. Set the grid square in the Source menu.", elevation);
        }
        ImGui::Text("Elevation %.0f deg", elevation);
    }
    else {
        ImGui::Text("From you  -");
        if (ImGui::IsItemHovered()) { style::tooltip("Set your grid square in the Source menu and this shows the distance,\nthe direction and how far above the horizon it is."); }
        ImGui::Text("Elevation -");
    }

    ImGui::SectionHeader("WEATHER");

    ImGui::Text("Temp      %.1f C", d.temp);
    ImGui::Text("Humidity  %.0f %%", d.rh);
    ImGui::Text("Dew point %.1f C", d.dewpt);
    ImGui::Text("Pressure  %.1f hPa", d.pressure);
    if (ImGui::IsItemHovered()) {
        style::tooltip("Measured on the way up. This is the actual point of the flight: these are\n"
                          "the readings that go into weather forecasting models.");
    }

    if (!d.auxData.empty()) {
        ImGui::Text("Aux       %s", d.auxData.c_str());
        if (ImGui::IsItemHovered()) { style::tooltip("Extra instrument data, most often an ozone sonde"); }
    }
}

// The shape of the flight: up at a steady few metres a second for an hour and a
// half, a burst, then down fast under a parachute. One glance says which part of it
// you are watching, which no single row of numbers does.
void RadiosondeDecoderModule::drawTrack() {
    ImGui::SectionHeader("FLIGHT");

    float height = ImGui::GetTextLineHeight() * 5.0f;
    float width = ImGui::GetContentRegionAvail().x;
    if (width < 32.0f) { return; }

    ImVec2 boxMin = ImGui::GetCursorScreenPos();
    ImVec2 boxMax = ImVec2(boxMin.x + width, boxMin.y + height);
    ImDrawList* draw = ImGui::GetWindowDrawList();

    draw->AddRectFilled(boxMin, boxMax, ImGui::GetColorU32(ImGuiCol_FrameBg));

    long long firstTime = 0, lastTime = 0;   // milliseconds, see SondeFix::time
    float lowest = 0.0f, highest = 0.0f;
    size_t count = 0;
    double span = 0.0;                       // milliseconds covered by the plot
    long long burstTime = 0;                 // when the highest point was reached
    float lastAlt = 0.0f;
    bool haveBurst = false;

    // Snapshot under the lock, draw outside it. A two hour flight is thousands of
    // points and there are only a few hundred pixels to put them in, so it is taken
    // one point per column: that keeps the copy short, keeps the decoder thread from
    // waiting on the drawing, and stops the plot spending a frame on line segments
    // that land on top of each other. A member and not a static local because this
    // module allows more than one instance.
    std::vector<ImVec2>& points = trackPoints;
    points.clear();
    {
        std::lock_guard<std::mutex> lck(dataMtx);
        count = track.size();
        if (count >= 2) {
            firstTime = track.front().time;
            lastTime = track.back().time;
            lowest = highest = track.front().alt;
            burstTime = track.front().time;
            lastAlt = track.back().alt;
            for (const auto& p : track) {
                if (p.alt < lowest) { lowest = p.alt; }
                if (p.alt > highest) { highest = p.alt; burstTime = p.time; }
            }

            // The highest point is only the burst once the sonde has come down from
            // it. On the way up the highest point is simply the most recent one, and
            // marking that would park the label on the balloon for the whole ascent.
            // A couple of hundred metres is past anything GPS noise or a pocket of
            // lift accounts for.
            const float BURST_CONFIRM_DROP_M = 250.0f;
            haveBurst = (lastAlt < highest - BURST_CONFIRM_DROP_M);

            span = (double)(lastTime - firstTime);   // milliseconds
            float range = highest - lowest;
            // A second of track and a metre of climb before there is anything worth
            // drawing a line between.
            if (span > 1000.0 && range > 1.0f) {
                int columns = std::min<int>((int)count, std::max<int>(2, (int)width));
                points.reserve(columns);
                for (int c = 0; c < columns; c++) {
                    size_t idx = (size_t)((int64_t)c * (int64_t)(count - 1) / (columns - 1));
                    const SondeFix& p = track[idx];
                    points.push_back(ImVec2(boxMin.x + (float)(((p.time - firstTime) / span) * width),
                                            boxMax.y - (((p.alt - lowest) / range) * height)));
                }
            }
        }
    }

    // Amber for the fall, so the two halves of the flight read apart at a glance
    // instead of having to be inferred from the slope.
    const ImU32 descentCol = IM_COL32(255, 170, 60, 255);

    if (points.size() >= 2) {
        const ImU32 ascentCol = ImGui::GetColorU32(ImGuiCol_PlotLines);
        float burstX = 0.0f;
        int splitCol = -1;
        if (haveBurst && span > 0.0) {
            burstX = boxMin.x + (float)(((double)(burstTime - firstTime) / span) * width);
            for (size_t i = 0; i < points.size(); i++) {
                if (points[i].x >= burstX) { splitCol = (int)i; break; }
            }
        }

        if (splitCol > 0 && splitCol < (int)points.size() - 1) {
            // The two runs share a point, so they meet instead of leaving a gap.
            draw->AddPolyline(points.data(), splitCol + 1, ascentCol, 0, 1.5f * style::uiScale);
            draw->AddPolyline(points.data() + splitCol, (int)points.size() - splitCol,
                              descentCol, 0, 1.5f * style::uiScale);
        }
        else {
            draw->AddPolyline(points.data(), (int)points.size(), ascentCol, 0, 1.5f * style::uiScale);
        }

        if (haveBurst && splitCol >= 0) {
            // A hairline the full height, so the moment can be read off the time axis
            // and not only off the curve.
            draw->AddLine(ImVec2(burstX, boxMin.y), ImVec2(burstX, boxMax.y),
                          IM_COL32(255, 170, 60, 90), style::uiScale);
            // At the top of the box, not at a computed y: the plot is scaled to
            // lowest..highest, and the burst IS the highest point, so it always lands
            // exactly on the top edge. Working it out from the altitude would be the
            // same number with more arithmetic to get wrong.
            draw->AddCircle(ImVec2(burstX, boxMin.y), 4.0f * style::uiScale, descentCol, 0,
                            2.0f * style::uiScale);
        }

        // Where it is now.
        draw->AddCircleFilled(points.back(), 3.0f * style::uiScale, ImGui::GetColorU32(ImGuiCol_Text));
    }

    draw->AddRect(boxMin, boxMax, ImGui::GetColorU32(ImGuiCol_Border));
    ImGui::Dummy(ImVec2(width, height));

    if (count < 2) {
        ImGui::TextDisabled("Altitude will plot here once it has a fix");
    }
    else if (haveBurst) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(descentCol), "Burst %s",
                           fmtAltitude(highest).c_str());
        if (ImGui::IsItemHovered()) {
            style::tooltip("The balloon burst at %s, %.0f min after this sonde was tuned.\n"
                              "It has fallen %s since.",
                              fmtAltitude(highest).c_str(),
                              (double)(burstTime - firstTime) / 60000.0,
                              fmtAltitude(highest - lastAlt).c_str());
        }
        ImGui::SameLine();
        ImGui::TextDisabled("- now %s", fmtAltitude(lastAlt).c_str());
    }
    else {
        ImGui::TextDisabled("%s to %s over %.0f min", fmtAltitude(lowest).c_str(),
                            fmtAltitude(highest).c_str(), (double)(lastTime - firstTime) / 60000.0);
        if (ImGui::IsItemHovered()) {
            style::tooltip("Altitude against time since this sonde was tuned. The peak is the burst;\n"
                              "after it the descent is much steeper than the climb, and the plot\n"
                              "turns amber from that point on.");
        }
    }
}

void RadiosondeDecoderModule::drawLogging() {
    ImGui::SectionHeader("LOG");

    if (ImGui::Checkbox(("Record the track##radiosonde_log_" + name).c_str(), &logEnabled)) {
        if (logEnabled) { logEnabled = requestLog() || logAskingOverwrite; }
        else {
            closeLog();
            logAskingOverwrite = false;
            logStatus = "";
        }
        config.acquire();
        config.conf[name]["logEnabled"] = logEnabled;
        config.release(true);
    }
    ImGui::HelpMarker("Writes a GPX file as it decodes, which most mapping software will open.\n"
                      "The file is kept valid after every frame, so it is still readable if the\n"
                      "program stops part way through a flight.");

    // Asked rather than assumed, because both answers destroy something: continuing
    // draws two flights as one line across the map, starting fresh throws the old one
    // away. Nothing is opened and nothing is written until this is answered.
    if (logAskingOverwrite) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
                           "That file already holds %d point(s).", logExistingPoints);
        if (ImGui::Button(("Continue it##radiosonde_logcont_" + name).c_str())) {
            logAskingOverwrite = false;
            logEnabled = openLog(false);
            config.acquire();
            config.conf[name]["logEnabled"] = logEnabled;
            config.release(true);
        }
        if (ImGui::IsItemHovered()) {
            style::tooltip("Adds this flight to the end of the track already in the file.\n"
                           "Nothing there now is lost, but two flights in one file draw\n"
                           "as a single line between them.");
        }
        ImGui::SameLine();
        if (ImGui::Button(("Start fresh##radiosonde_lognew_" + name).c_str())) {
            logAskingOverwrite = false;
            logEnabled = openLog(true);
            config.acquire();
            config.conf[name]["logEnabled"] = logEnabled;
            config.release(true);
        }
        if (ImGui::IsItemHovered()) {
            style::tooltip("Replaces the file. The %d point(s) in it now are gone for good.",
                           logExistingPoints);
        }
    }

    if (ImGui::Checkbox(("Record every frame##radiosonde_framelog_" + name).c_str(), &frameLogEnabled)) {
        if (frameLogEnabled) { frameLogEnabled = openFrameLog(); }
        else { closeFrameLog(); }
        config.acquire();
        config.conf[name]["frameLogEnabled"] = frameLogEnabled;
        config.release(true);
    }
    ImGui::HelpMarker("Writes every decoded frame to a .frames.csv beside the track - position,\n"
                      "telemetry, the onboard clock as it arrived, and whether that clock was\n"
                      "believed. Rows are kept for frames with no position too.\n"
                      "It is what lets a flight be gone over afterwards, or replayed against a\n"
                      "change, instead of waiting for the next launch.");

    if (framesTimeRejected > 0) {
        ImGui::TextDisabled("%d frame(s) had an unbelievable clock", framesTimeRejected);
        if (ImGui::IsItemHovered()) {
            style::tooltip("Their position was kept and only the timestamp dropped, so the track\n"
                           "is complete and nothing is stamped with a wrong time.");
        }
    }

    ImGui::LeftLabel("File");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::InputText(("##radiosonde_logpath_" + name).c_str(), logPath, sizeof logPath)) {
        config.acquire();
        config.conf[name]["logPath"] = std::string(logPath);
        config.release(true);
    }
    if (ImGui::IsItemHovered()) { style::tooltip("Reopened when the tick box above is turned off and on again"); }

    if (!logStatus.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", logStatus.c_str());
        ImGui::PopTextWrapPos();
    }
}

MOD_EXPORT void _INIT_() {
    config.setPath(std::string(core::getRoot()) + "/radiosonde_decoder_config.json");
    config.load(json::object());
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new RadiosondeDecoderModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (RadiosondeDecoderModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
