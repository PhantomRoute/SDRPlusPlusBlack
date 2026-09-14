#pragma once
#include <string>

// A panel along the bottom showing what the signal inside the selected VFO is doing:
// its instantaneous frequency over time, its eye, and its constellation. It works from
// a channel of its own that follows the selected VFO, so it does not depend on the
// Radio module or on which mode is chosen, and it can also show what a decoder that
// publishes to symboltap actually sampled.
//
// It reports what it measures and nothing more. It never says what a signal is.
namespace sigan {
    void init();
    bool isShown();
    void setShown(bool shown);

    // Keeps the measurements running for a moment, for something other than the panel
    // that reads them - Signal ID's MODULATION section. Call it every frame it is drawn.
    void request();

    struct Measurements {
        bool running = false;           // a channel is open and samples are arriving
        bool haveSymbolRate = false;
        double symbolRate = 0.0;        // Bd
        float lineDb = 0.0f;            // how far the symbol-rate line stands above the rest
        int levels = 0;                 // distinct frequency levels at the symbol instants, 0 if none
        float levelHz[8] = { 0 };       // relative to the channel centre
        float deviationHz = 0.0f;       // half the gap between the outermost levels
        bool haveEdges = false;
        float edgeFraction = 0.0f;      // 20% to 80% of a step, as a fraction of a symbol
        bool haveEye = false;
        float eyeOpening = 0.0f;        // 0..1, the narrowest gap between adjacent levels
        bool haveOffset = false;
        double offsetHz = 0.0;          // power-weighted average frequency from the channel centre
    };

    // The latest measurements from the channel. False while the analyzer is not running.
    bool getMeasurements(Measurements& out);
}
