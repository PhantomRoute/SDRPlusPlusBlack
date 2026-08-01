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
    void refreshSelected();
    void start();
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
    double tuneOffset;
    double currentFreq;
    double ifFreq = 0.0;
    TuningMode tuneMode = TuningMode::NORMAL;
    dsp::stream<dsp::complex_t> nullSource;
};
