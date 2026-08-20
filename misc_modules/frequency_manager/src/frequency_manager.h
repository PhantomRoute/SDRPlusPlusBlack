#pragma once

#include <string>
#include "config.h"
// Relative, not <radio_interface.h>: core builds mobile_main_window.cpp, which
// includes this header, and core has no include path into the decoder modules.
#include "../../../decoder_modules/radio/src/radio_interface.h"

struct FrequencyBookmark {
    double frequency;
    double bandwidth;
    int modeIndex;
    bool selected;
    std::string vfoName; // VFO/radio this bookmark belongs to; empty = legacy (apply to currently selected VFO)
    // A repeater's tone is a property of the channel, so it belongs on the
    // bookmark. hasTone is false for bookmarks saved before this existed; those are
    // recalled without touching the radio's tone settings, rather than silently
    // clearing them.
    bool hasTone = false;
    RadioToneSettings tone;
    // Free text about the channel - who is on it, when it is worth listening to, the
    // things that otherwise end up in a spreadsheet beside the program. Empty on every
    // bookmark saved before this existed, which reads the same as having no notes.
    std::string notes;
};

struct WaterfallBookmark {
    std::string listName;
    std::string bookmarkName;
    std::string extraInfo;
    bool worked;
    FrequencyBookmark bookmark;
    long long notValidAfter;
};

struct TransientBookmarkManager {
    std::vector<WaterfallBookmark> transientBookmarks;

    virtual void refreshWaterfallBookmarks(bool lockConfig = true) = 0;
    virtual const char *getModesList() = 0;
};

void applyBookmark(FrequencyBookmark bm, std::string vfoName);
ConfigManager &getFrequencyManagerConfig();