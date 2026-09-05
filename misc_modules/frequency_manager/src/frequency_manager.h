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
    // Left out of the scan. A property of the channel and not of the scanner: the
    // scanner used to keep its own set of skipped names, which meant two lists with a
    // channel of the same name shared one skip flag, and a bookmark moved between
    // lists took the other list's setting with it. False on every bookmark saved
    // before this existed, which is the same as never having been skipped.
    bool skip = false;
    // The mode, and the value of record for it. modeIndex above is a position in the
    // loaded radio's mode list - it is what the combo box in the edit dialog indexes,
    // and it is -1 whenever there is no radio to index into, or the radio does not
    // list this mode. That makes it useless for storage, and storing it anyway is
    // what this field exists to stop.
    //
    // Saving used to convert modeIndex back through the radio module. A save made with
    // no radio loaded - a TETRA VFO selected, or no VFO at all - therefore wrote "no
    // mode" over every bookmark in the list, dropped the tone block off every NFM one
    // on the way past, and the next save with a radio present resolved the lot to NFM,
    // because getDemodByIndex answers NFM for any index outside the list including -1.
    // Two saves turned a list of airband channels into a list of NFM ones.
    //
    // This one needs no radio to be correct, so it is what gets written, and modeIndex
    // is derived from it rather than the other way round.
    //
    // A plain int and not a DemodID: this header is included by core, which builds
    // mobile_main_window.cpp, and the enum lives behind includes core has no path to.
    // 0 is RADIO_DEMOD_NFM, which is the right default for a bookmark that never got
    // told what it was.
    int demodId = 0;
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