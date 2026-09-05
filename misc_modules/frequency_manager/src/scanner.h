#pragma once

#include <string>
#include <vector>
#include <deque>
#include <map>
#include <set>
#include <functional>
#include <cmath>
#include <imgui.h>
#include <module.h>
#include <gui/widgets/waterfall.h>
#include "frequency_manager.h"

class FrequencyManagerModule; // Forward declaration

class Scanner {
public:
    Scanner(FrequencyManagerModule* module);
    ~Scanner();

    // What the scanner is doing right now. The panel, the waterfall overlay and
    // the bookmark list all read this, so there is one description of the state
    // rather than a set of booleans that can disagree with each other.
    enum State {
        SCAN_IDLE,
        SCAN_SETTLING,  // just retuned; the FFT still shows the previous channel
        SCAN_MEASURING, // sampling this channel to decide whether it is busy
        SCAN_LISTENING, // signal found, parked on it
        SCAN_HELD       // parked by the user until they release it
    };

    static const char* stateName(State state);

    bool isScanning() const { return state != SCAN_IDLE; }
    State getState() const { return state; }

    // Whether a channel is one the scan will actually stop on. Public because the
    // bookmark list colours its rows by it, and having the list work the rule out
    // for itself is how the two end up disagreeing.
    bool isScannable(const std::string& name) const;

    // How the scanner asks for a channel's skip flag to be changed. The flag lives
    // on the bookmark now, so only the frequency manager can write and save it;
    // these are set by the module at construction. Both may be empty, and every
    // call site checks - the scanner has to keep working if it is ever used
    // without a manager behind it.
    std::function<void(const std::string&, bool)> onSetSkip;
    std::function<void()> onClearSkips;

    // The level a channel has to beat to count as busy.
    float getTriggerLevel() const { return noiseFloor + signalMarginDb; }

    std::string getCurrentStation() const { return currentStation; }
    std::string getStatusMessage() const { return statusMessage; }
    void startScanner() { start(); }
    void stopScanner() { stop(); }

    void onPlayStateChange(bool playing);

    void setBookmarks(const std::vector<std::string>& bookmarks, const std::map<std::string, FrequencyBookmark>& bookmarksMap);

    void render();
    void update(float deltaTime);

    // Drawn over the FFT from the frequency manager's redraw handler: the channel
    // being checked right now and the other channels in the scan. The trigger level
    // is not drawn here - it is on the meter in the panel, beside the level it is
    // being compared against.
    void drawWaterfallOverlay(const ImGui::WaterFall::FFTRedrawArgs& args);

private:
    FrequencyManagerModule* module;

    // ---- Settings, all persisted under "scanner" in the frequency manager config
    float dwellMs = 250.0f;        // how long to wait on a quiet channel ("scanIntervalMs")
    float settleMs = 120.0f;       // ignored after a retune, while the FFT catches up
    float listenTimeSec = 10.0f;   // how long to stay on a busy channel
    float noiseFloor = 3.0f;       // dB over the local noise, not absolute dBFS
    float signalMarginDb = 4.0f;   // how far over the floor a channel has to be
    bool squelchEnabled = false;
    bool carrierHoldMode = false;

    // ---- Scan state
    State state = SCAN_IDLE;
    State stateBeforeHold = SCAN_MEASURING;
    float stateTime = 0.0f; // seconds spent in the current state

    std::vector<std::string> bookmarks;
    std::map<std::string, FrequencyBookmark> bookmarksMap;
    size_t currentStationIndex = 0;
    std::string currentStation;
    FrequencyBookmark currentBookmark;
    bool haveCurrentBookmark = false;
    std::string statusMessage; // why the scan will not start, shown under the button

    // ---- Measurement
    bool haveLevel = false;
    float level = 0.0f;         // dB over the local noise floor
    float meterPeak = 0.0f;     // decaying peak, for the meter only
    std::vector<float> noiseScratch;

    // Rolling 500ms window, used by "Set from noise".
    struct SignalSample {
        float level;
        float time;
    };
    std::deque<SignalSample> signalHistory;

    // ---- Activity log
    struct Hit {
        std::string name;
        double frequency;
        float level;
        double time;
    };
    std::deque<Hit> hits;

    bool weMuted = false;    // the temp mute is ours to release
    double lastRenderTime = -1000.0;

    void start();
    void stop();
    void gotoStation(size_t index);
    void step(int dir);
    void enterState(State newState);
    int findScannable(int from, int dir) const;
    // The channels in the current list that are marked skip, worked out from the
    // bookmarks themselves rather than kept as a second copy that can go stale.
    std::vector<std::string> skippedNames() const;
    void setMuted(bool muted);
    void logHit();
    float historyAverage() const;

    void sampleLevel(float deltaTime);
    bool measureChannel(double freq, double bandwidth, float& snrOut, float& noiseOut);
    bool currentChannel(double& freq, double& bandwidth) const;

    void drawTransport();
    void drawStatus();
    void drawMeter();
    void drawSettings();
    void drawActivity();

    EventHandler<bool> playStateHandler;
};
