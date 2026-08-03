#pragma once
#include <string>
#include <vector>
#include <map>
#include <dsp/stream.h>
#include <dsp/types.h>
#include <utils/event.h>

class SourceManager {
public:
    SourceManager();

    struct SourceHandler {
        dsp::stream<dsp::complex_t>* stream = NULL;
        void (*menuHandler)(void* ctx) = NULL;
        void (*selectHandler)(void* ctx) = NULL;
        void (*deselectHandler)(void* ctx) = NULL;
        void (*startHandler)(void* ctx) = NULL;
        void (*stopHandler)(void* ctx) = NULL;
        void (*tuneHandler)(double freq, void* ctx) = NULL;

        // Optional. Re-enumerates the module's devices so one plugged in while
        // the application is running gets picked up on its own. Called from the
        // UI thread while the radio is stopped and the source menu is on screen,
        // so only cheap enumerations should implement it; anything slow should
        // leave it unset and rely on the module's own refresh button.
        void (*refreshHandler)(void* ctx) = NULL;

        // Optional. Returns a string that changes whenever the set of connected
        // devices changes. Called from a background thread, so it must only read
        // the device library into local storage: no module state, no config, no
        // GUI. When set, the far more expensive refreshHandler only runs once
        // this has reported an actual change.
        std::string (*probeHandler)(void* ctx) = NULL;

        // Optional. Reports whether the source is really streaming once
        // startHandler has run. Sources that can fail to open their device
        // should set this, otherwise the UI latches into a running state with no
        // data and no way to tell why.
        bool (*runningHandler)(void* ctx) = NULL;

        void* ctx = NULL;
    };

    enum TuningMode {
        NORMAL,
        PANADAPTER
    };

    void registerSource(std::string name, SourceHandler* handler);
    void unregisterSource(std::string name);
    void selectSource(std::string name);
    void showSelectedMenu();

    // Picks up devices plugged in while the application is running. Safe to call
    // every frame: the rate limiting and the device enumeration both live below
    // this call, off the UI thread whenever the source provides a probeHandler.
    void pollDeviceChanges();

    // Returns whether the source actually started. Sources that don't implement
    // runningHandler are assumed to have succeeded.
    bool start();
    void stop();
    void tune(double freq);
    void setTuningOffset(double offset);
    void setTuningMode(TuningMode mode);
    void setPanadapterIF(double freq);
    const std::string& getSelectedName() const { return selectedName; }

    std::vector<std::string> getSourceNames();

    Event<std::string> onSourceSelected;
    Event<std::string> onSourceRegistered;
    Event<std::string> onSourceUnregister;
    Event<std::string> onSourceUnregistered;
    Event<double> onTuneChanged;
    Event<double> onRetune;
    int secondsAdjustment;

#ifndef BUILD_TESTS
private:
#endif

    std::map<std::string, SourceHandler*> sources;
    std::string selectedName;
    SourceHandler* selectedHandler = NULL;
    double tuneOffset = 0.0;
    double currentFreq = 0.0;
    double ifFreq = 0.0;
    TuningMode tuneMode = TuningMode::NORMAL;
    dsp::stream<dsp::complex_t> nullSource;
};
