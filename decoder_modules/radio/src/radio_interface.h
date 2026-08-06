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

// The CTCSS/DCS state of a radio, in plain types so this header keeps its lack of
// dependencies. `mode` matches tonedetect::Target::Mode. Only meaningful for NFM;
// setting it on any other demodulator is ignored.
struct RadioToneSettings {
    bool squelchEnabled = false;
    int mode = 0; // 0 off, 1 CTCSS, 2 DCS
    float ctcssFreq = 100.0f;
    int dcsCode = 23;
    bool dcsInverted = false;
    bool filterEnabled = false;
    bool identifyEnabled = false;
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