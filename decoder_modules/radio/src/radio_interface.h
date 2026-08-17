#pragma once

enum {
    RADIO_IFACE_CMD_GET_MODE,
    RADIO_IFACE_CMD_SET_MODE,
    RADIO_IFACE_CMD_GET_BANDWIDTH,
    RADIO_IFACE_CMD_SET_BANDWIDTH,
    RADIO_IFACE_CMD_GET_SQUELCH_ENABLED,
    RADIO_IFACE_CMD_SET_SQUELCH_ENABLED,
    RADIO_IFACE_CMD_GET_SQUELCH_LEVEL,
    RADIO_IFACE_CMD_SET_SQUELCH_LEVEL,

    RADIO_IFACE_CMD_ADD_TO_IFCHAIN,
    RADIO_IFACE_CMD_REMOVE_FROM_IFCHAIN,
    RADIO_IFACE_CMD_ENABLE_IN_IFCHAIN,
    RADIO_IFACE_CMD_DISABLE_IN_IFCHAIN,

    RADIO_IFACE_CMD_ADD_TO_AFCHAIN,
    RADIO_IFACE_CMD_REMOVE_FROM_AFCHAIN,
    RADIO_IFACE_CMD_ENABLE_IN_AFCHAIN,
    RADIO_IFACE_CMD_DISABLE_IN_AFCHAIN,

    // Appended, never inserted: other modules hold these values.
    RADIO_IFACE_CMD_GET_TONE_SETTINGS,
    RADIO_IFACE_CMD_SET_TONE_SETTINGS,
};

// How many codes one channel can be told to open for. Held equal to
// tonedetect::TONE_LIST_MAX by a static_assert in radio_module.h, which is the one
// place both are visible.
static const int RADIO_TONE_LIST_MAX = 16;

// One code of an accept list, with the same fields and the same meaning as
// tonedetect::ToneKey. Spelled out again rather than shared because this header is
// the boundary between separately built modules and deliberately has no includes;
// tone_tables.h owns the real definition and the radio module converts between them.
struct RadioToneListEntry {
    int kind = 0; // 0 CTCSS, 1 DCS
    float ctcssFreq = 100.0f;
    int dcsCode = 23;
    bool dcsInverted = false;
};

// The CTCSS/DCS state of a radio, in plain types so this header keeps its lack of
// dependencies. `mode` matches tonedetect::Target::Mode. Only meaningful for NFM;
// setting it on any other demodulator is ignored.
struct RadioToneSettings {
    bool squelchEnabled = false;
    int mode = 0; // 0 off, 1 CTCSS, 2 DCS, 3 any tone, 4 list
    float ctcssFreq = 100.0f;
    int dcsCode = 23;
    bool dcsInverted = false;
    bool filterEnabled = false;
    bool identifyEnabled = false;

    // Appended, never inserted: other modules are built against the layout above.
    // Only consulted when `mode` is the list mode, but carried either way so that
    // switching a bookmark's mode back and forth does not lose the codes.
    int listCount = 0;
    RadioToneListEntry list[RADIO_TONE_LIST_MAX];
};

enum {
    RADIO_IFACE_MODE_NFM,
    RADIO_IFACE_MODE_WFM,
    RADIO_IFACE_MODE_AM,
    RADIO_IFACE_MODE_DSB,
    RADIO_IFACE_MODE_USB,
    RADIO_IFACE_MODE_CW,
    RADIO_IFACE_MODE_LSB,
    RADIO_IFACE_MODE_RAW
};