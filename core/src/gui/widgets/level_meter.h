#pragma once
#include <imgui/imgui.h>

namespace ImGui {

    // A horizontal bar showing a live level against the threshold it has to beat, with
    // the threshold draggable along the bar.
    //
    // This replaces a slider wherever the threshold on its own tells the user nothing.
    // The question being asked of a squelch or a scanner trigger is always "is what is
    // arriving right now above this or below it", and a slider can only show the half
    // of that the user already knows - which is why such a control has always needed a
    // second thing drawn somewhere else to be usable at all. Putting the level and the
    // threshold in one widget answers the question where the control is, and means
    // nothing has to be drawn across the spectrum to make the comparison possible.
    struct LevelMeterStyle {
        ImVec4 fillClosed;              // bar while the level is under the threshold
        ImVec4 fillOpen;                // bar once it is over
        ImVec4 threshold;               // the threshold line and its marker
        const char* caption = "Signal"; // leads the readout under the bar
        // Printed instead of "opens at ..." when the threshold sits on the bottom stop,
        // for the controls where the bottom of the scale means "off" rather than a
        // threshold of that many dB. NULL to always print the number.
        const char* floorCaption = NULL;
        float tickStepDb = 10.0f;
    };

    struct LevelMeterResult {
        bool changed = false;  // the threshold moved this frame
        bool released = false; // the drag ended this frame - a good moment to save it
        bool hovered = false;  // the caller's cue to put up its own tooltip
    };

    // level and peak are ignored unless haveLevel; a peak at or below level draws no
    // peak marker. open decides the fill colour and is passed in rather than worked out
    // from level and threshold, so that a caller whose gate has hysteresis colours the
    // bar by what the gate actually did rather than by a comparison that disagrees with
    // it. Honours style::beginDisabled() and ImGui::BeginDisabled().
    LevelMeterResult LevelMeter(const char* id, float minDb, float maxDb,
                                bool haveLevel, float level, float peak, bool open,
                                float* threshold, const LevelMeterStyle& look);
}
