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
        config.conf[name]["logPath"] = (std::string(core::getRoot()) + "/radiosonde.gpx");
        created = true;
    }
    selectedType = config.conf[name].contains("sondeType") ? (int)config.conf[name]["sondeType"] : 0;
    logEnabled = config.conf[name].contains("logEnabled") && config.conf[name]["logEnabled"].is_boolean()
                     ? (bool)config.conf[name]["logEnabled"]
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

    // selectType builds the channel at the right width and starts the decoder, so
    // there is nothing to start here first.
    selectType(selectedType);
    if (logEnabled) { logEnabled = openLog(); }

    enabled = true;
    gui::menu.registerEntry(name, menuHandler, this, this);
}

RadiosondeDecoderModule::~RadiosondeDecoderModule() {
    gui::menu.removeEntry(name);
    if (enabled) { disable(); }
    closeLog();
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

// Runs on the decoder's worker thread, not the UI thread.
void RadiosondeDecoderModule::sondeDataHandler(SondeFullData* data, void* ctx) {
    RadiosondeDecoderModule* _this = (RadiosondeDecoderModule*)ctx;
    if (data == NULL) { return; }

    std::lock_guard<std::mutex> lck(_this->dataMtx);
    _this->lastData = *data;
    _this->everDecoded = true;
    _this->framesDecoded++;
    _this->lastFrameTime = currentTimeMillis();

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

        if (_this->logFile) { _this->writeLogPoint(*data); }
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

// ---- GPX logging

// Both of these run on the UI thread while the decoder thread may be part way
// through writeLogPoint, so they take the same lock that guards the data. Neither is
// ever called with it already held, and writeLogPoint does not take it - it is only
// reached from the callback, which does.
bool RadiosondeDecoderModule::openLog() {
    closeLog();
    if (logPath[0] == 0) {
        logStatus = "No file chosen";
        return false;
    }
    std::lock_guard<std::mutex> lck(dataMtx);
    logFile = fopen(logPath, "wb");
    if (!logFile) {
        logStatus = std::string("Could not open ") + logPath;
        flog::error("Radiosonde: could not open {0}", logPath);
        return false;
    }
    fprintf(logFile,
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<gpx version=\"1.1\" creator=\"SDR++ radiosonde decoder\" xmlns=\"http://www.topografix.com/GPX/1/1\">\n"
            "  <trk>\n    <name>Radiosonde</name>\n    <trkseg>\n");
    fflush(logFile);
    logStatus = std::string("Writing ") + logPath;
    return true;
}

void RadiosondeDecoderModule::closeLog() {
    std::lock_guard<std::mutex> lck(dataMtx);
    if (!logFile) { return; }
    fprintf(logFile, "    </trkseg>\n  </trk>\n</gpx>\n");
    fclose(logFile);
    logFile = NULL;
}

void RadiosondeDecoderModule::writeLogPoint(const SondeFullData& d) {
    if (!logFile) { return; }

    char when[64] = { 0 };
    if (d.time > 0) {
        time_t t = (time_t)d.time;
        struct tm* utc = gmtime(&t);
        if (utc) { strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%SZ", utc); }
    }

    fprintf(logFile, "      <trkpt lat=\"%.6f\" lon=\"%.6f\">\n        <ele>%.1f</ele>\n",
            d.lat, d.lon, d.alt);
    if (when[0]) { fprintf(logFile, "        <time>%s</time>\n", when); }
    fprintf(logFile, "      </trkpt>\n");

    // Close the document, flush, then wind back over the closing tags so the next
    // point overwrites them. The file on disk is therefore valid GPX after every
    // single frame - pull the power out mid flight and what was written still opens.
    long resume = ftell(logFile);
    fprintf(logFile, "    </trkseg>\n  </trk>\n</gpx>\n");
    fflush(logFile);
    if (resume >= 0) { fseek(logFile, resume, SEEK_SET); }
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
        ImGui::SetTooltip("Which family of sonde to decode. They are not interchangeable, and there is\n"
                          "no auto detection: if you do not know, RS41 is the most widely flown, and\n"
                          "the channel width changes with the choice so a wrong pick usually looks\n"
                          "obviously wrong on the waterfall.");
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
    {
        std::lock_guard<std::mutex> lck(dataMtx);
        d = lastData;
        have = everDecoded;
        frames = framesDecoded;
        age = have ? ((double)(currentTimeMillis() - lastFrameTime) / 1000.0) : 0.0;
    }

    ImGui::SectionHeader("SONDE");

    if (!have) {
        ImGui::TextDisabled("Nothing decoded yet");
        ImGui::TextDisabled("Tune the marker onto the signal");
        return;
    }

    ImGui::Text("Serial    %s", d.serial.empty() ? "-" : d.serial.c_str());
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Printed on the sonde. This is what identifies the flight on tracking sites."); }

    ImGui::Text("Frame     %d", d.seq);
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Sequence number from the sonde, and %d frames decoded since it was tuned", frames); }

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
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Nothing has decoded recently. Everything above and below is from that frame."); }
    }
    else {
        ImGui::TextDisabled("Last      %.0f s ago", age);
    }

    if (!d.calibrated) {
        ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.0f, 1.0f), "Calibrat. %.0f%%", d.calib_percent);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("The sonde sends its calibration a piece at a time, over a minute or two.\n"
                              "Until it is complete the temperature and humidity below are approximate.");
        }
    }
    else {
        ImGui::TextDisabled("Calibrat. complete");
    }

    if (d.burstkill > 0) {
        ImGui::Text("Shutdown  in %d s", d.burstkill);
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("The sonde is on a timer and will switch itself off"); }
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
        ImGui::SetTooltip("Around 5 m/s up is a normal ascent. A sharp change to a fast descent is the\n"
                          "balloon bursting, usually somewhere above 25 km.");
    }

    ImGui::Text("Ground    %.0f km/h, %s", d.spd * 3.6f, compassPoint(d.hdg));
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("How fast the wind is carrying it, and which way it is going"); }

    // Where to point, from the grid square in the Source menu.
    utils::BearingDistance bd;
    double elevation = 0.0;
    if (bearingFromOperator(d, bd, elevation)) {
        ImGui::Text("From you  %s %s", fmtDistance(bd.distance).c_str(), compassPoint(bd.bearing));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Distance and direction from your grid square, and %.0f degrees above the\n"
                              "horizon. Set the grid square in the Source menu.", elevation);
        }
        ImGui::Text("Elevation %.0f deg", elevation);
    }
    else {
        ImGui::Text("From you  -");
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Set your grid square in the Source menu and this shows the distance,\nthe direction and how far above the horizon it is."); }
        ImGui::Text("Elevation -");
    }

    ImGui::SectionHeader("WEATHER");

    ImGui::Text("Temp      %.1f C", d.temp);
    ImGui::Text("Humidity  %.0f %%", d.rh);
    ImGui::Text("Dew point %.1f C", d.dewpt);
    ImGui::Text("Pressure  %.1f hPa", d.pressure);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Measured on the way up. This is the actual point of the flight: these are\n"
                          "the readings that go into weather forecasting models.");
    }

    if (!d.auxData.empty()) {
        ImGui::Text("Aux       %s", d.auxData.c_str());
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Extra instrument data, most often an ozone sonde"); }
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
            for (const auto& p : track) {
                if (p.alt < lowest) { lowest = p.alt; }
                if (p.alt > highest) { highest = p.alt; }
            }

            double span = (double)(lastTime - firstTime);   // milliseconds
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

    if (points.size() >= 2) {
        draw->AddPolyline(points.data(), (int)points.size(), ImGui::GetColorU32(ImGuiCol_PlotLines),
                          0, 1.5f * style::uiScale);
        // Where it is now.
        draw->AddCircleFilled(points.back(), 3.0f * style::uiScale, ImGui::GetColorU32(ImGuiCol_Text));
    }

    draw->AddRect(boxMin, boxMax, ImGui::GetColorU32(ImGuiCol_Border));
    ImGui::Dummy(ImVec2(width, height));

    if (count < 2) {
        ImGui::TextDisabled("Altitude will plot here once it has a fix");
    }
    else {
        ImGui::TextDisabled("%s to %s over %.0f min", fmtAltitude(lowest).c_str(),
                            fmtAltitude(highest).c_str(), (double)(lastTime - firstTime) / 60000.0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Altitude against time since this sonde was tuned. The peak is the burst;\n"
                              "after it the descent is much steeper than the climb.");
        }
    }
}

void RadiosondeDecoderModule::drawLogging() {
    ImGui::SectionHeader("LOG");

    if (ImGui::Checkbox(("Record the track##radiosonde_log_" + name).c_str(), &logEnabled)) {
        if (logEnabled) { logEnabled = openLog(); }
        else {
            closeLog();
            logStatus = "";
        }
        config.acquire();
        config.conf[name]["logEnabled"] = logEnabled;
        config.release(true);
    }
    ImGui::HelpMarker("Writes a GPX file as it decodes, which most mapping software will open.\n"
                      "The file is kept valid after every frame, so it is still readable if the\n"
                      "program stops part way through a flight.");

    ImGui::LeftLabel("File");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::InputText(("##radiosonde_logpath_" + name).c_str(), logPath, sizeof logPath)) {
        config.acquire();
        config.conf[name]["logPath"] = std::string(logPath);
        config.release(true);
    }
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Reopened when the tick box above is turned off and on again"); }

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
