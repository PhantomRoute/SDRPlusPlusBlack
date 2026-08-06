#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// The CTCSS tone list, the DCS code table and the Golay check, with no DSP
// dependencies, so anything that needs to name or pick a tone can include this
// without dragging in fftw and volk. tone_detector.h includes it and does the
// actual detecting; the frequency manager includes it just for the pickers.
namespace tonedetect {
    struct Result {
        enum Kind {
            NONE,
            CTCSS,
            DCS
        };

        Kind kind = NONE;
        float ctcssFreq = 0.0f; // Hz, snapped to the standard tone
        int dcsNormal = 0;      // the code a radio set to normal polarity would send
        int dcsInverted = 0;    // the same waveform read as an inverted code

        bool operator==(const Result& o) const {
            return kind == o.kind && ctcssFreq == o.ctcssFreq && dcsNormal == o.dcsNormal;
        }

        // The badge drawn over the waterfall. Short enough not to crowd the spectrum
        // on a phone.
        std::string label() const {
            char buf[64];
            switch (kind) {
            case CTCSS:
                snprintf(buf, sizeof buf, "CTCSS %.1f Hz", ctcssFreq);
                return buf;
            case DCS:
                snprintf(buf, sizeof buf, "DCS D%03d", dcsNormal);
                return buf;
            default:
                return "";
            }
        }

        // The readout in the radio menu, where there is room to spell it out. A DCS
        // word and its complement are the same waveform read two ways, so both
        // codes are always valid and only the operator knows which one their radio
        // calls it - showing one and hiding the other would be a guess.
        std::string longLabel() const {
            char buf[96];
            switch (kind) {
            case CTCSS:
                snprintf(buf, sizeof buf, "CTCSS %.1f Hz", ctcssFreq);
                return buf;
            case DCS:
                snprintf(buf, sizeof buf, "DCS D%03dN (or D%03dI)", dcsNormal, dcsInverted);
                return buf;
            default:
                return "Listening...";
            }
        }
    };

    // The 52 tones every radio that does CTCSS agrees on: EIA/TIA-603 plus the
    // interstitials. 150.0 and 151.4 are only 1.4 Hz apart, which is what sets the
    // snapping tolerance below.
    static const float CTCSS_TONES[] = {
        67.0f, 69.3f, 71.9f, 74.4f, 77.0f, 79.7f, 82.5f, 85.4f, 88.5f, 91.5f,
        94.8f, 97.4f, 100.0f, 103.5f, 107.2f, 110.9f, 114.8f, 118.8f, 123.0f, 127.3f,
        131.8f, 136.5f, 141.3f, 146.2f, 150.0f, 151.4f, 156.7f, 159.8f, 162.2f, 165.5f,
        167.9f, 171.3f, 173.8f, 177.3f, 179.9f, 183.5f, 186.2f, 189.9f, 192.8f, 196.6f,
        199.5f, 203.5f, 206.5f, 210.7f, 218.1f, 221.3f, 225.7f, 229.1f, 233.6f, 241.8f,
        250.3f, 254.1f
    };
    static const int CTCSS_TONE_COUNT = (int)(sizeof(CTCSS_TONES) / sizeof(CTCSS_TONES[0]));

    // What the squelch is listening for. A DCS code and its inverted form are
    // different waveforms - D023I is the same thing on air as D047N - so the
    // polarity has to be part of the selection, exactly as it is on a radio.
    struct Target {
        enum Mode {
            OFF,
            CTCSS,
            DCS
        };

        Mode mode = OFF;
        float ctcssFreq = 100.0f;
        int dcsCode = 23;
        bool dcsInverted = false;

        bool matches(const Result& r) const {
            switch (mode) {
            case CTCSS:
                return r.kind == Result::CTCSS && std::fabs(r.ctcssFreq - ctcssFreq) < 0.05f;
            case DCS:
                if (r.kind != Result::DCS) { return false; }
                return dcsInverted ? (r.dcsInverted == dcsCode) : (r.dcsNormal == dcsCode);
            default:
                return false;
            }
        }
    };

    static const int DCS_CODES_COUNT = 42;
    static const int DCS_POLARITIES = 2;
    static const int DCS_WORDS_PER_CODE = 5;

    // Each row is one code in both polarities; each entry lists the Golay words that
    // decode to it. Column 0 and column 1 of a row are the same transmission read
    // with opposite polarity.
    static const int DCS_CODES[DCS_CODES_COUNT][DCS_POLARITIES][DCS_WORDS_PER_CODE] = {
        { { 023, 0340, 0766, 0, 0 }, { 047, 0375, 0707, 0, 0 } },
        { { 025, 0, 0, 0, 0 }, { 0244, 0417, 0176, 0, 0 } },
        { { 026, 0566, 0, 0, 0 }, { 0464, 0642, 0772, 0237, 0 } },
        { { 031, 0374, 0643, 0, 0 }, { 0627, 037, 0560, 0, 0 } },
        { { 032, 0, 0, 0, 0 }, { 051, 0520, 0771, 0, 0 } },
        { { 043, 0355, 0, 0, 0 }, { 0445, 0457, 0575, 0222, 0 } },
        { { 054, 0405, 0675, 0, 0 }, { 0413, 0620, 0133, 0, 0 } },
        { { 065, 0301, 0, 0, 0 }, { 0271, 0427, 0510, 0762, 0 } },
        { { 071, 0603, 0717, 0746, 0 }, { 0306, 0761, 0147, 0303, 0 } },
        { { 072, 0470, 0701, 0, 0 }, { 0245, 0370, 0554, 0, 0 } },
        { { 073, 0640, 0, 0, 0 }, { 0506, 0574, 0224, 0313, 0 } },
        { { 074, 0360, 0721, 0, 0 }, { 0174, 0270, 0142, 0, 0 } },
        { { 0114, 0327, 0615, 0, 0 }, { 0712, 0136, 0502, 0, 0 } },
        { { 0115, 0534, 0674, 0, 0 }, { 0152, 0366, 0415, 0, 0 } },
        { { 0125, 0173, 0, 0, 0 }, { 0365, 0107, 0, 0, 0 } },
        { { 0131, 0572, 0702, 0, 0 }, { 0364, 0641, 0130, 0, 0 } },
        { { 0132, 0605, 0634, 0714, 0 }, { 0546, 0614, 0751, 0317, 0 } },
        { { 0134, 0273, 0, 0, 0 }, { 0223, 0350, 0475, 0750, 0 } },
        { { 0143, 0333, 0, 0, 0 }, { 0412, 0441, 0711, 0127, 0 } },
        { { 0155, 0233, 0660, 0, 0 }, { 0731, 0744, 0447, 0473, 0474 } },
        { { 0156, 0517, 0741, 0, 0 }, { 0265, 0426, 0171, 0, 0 } },
        { { 0162, 0416, 0553, 0, 0 }, { 0503, 0157, 0322, 0, 0 } },
        { { 0165, 0354, 0, 0, 0 }, { 0251, 0704, 0742, 0236, 0 } },
        { { 0172, 057, 0, 0, 0 }, { 036, 0137, 0, 0, 0 } },
        { { 0205, 0610, 0135, 0, 0 }, { 0263, 0736, 0213, 0, 0 } },
        { { 0226, 0557, 0104, 0, 0 }, { 0411, 0756, 0117, 0, 0 } },
        { { 0243, 0267, 0342, 0, 0 }, { 0351, 0353, 0435, 0, 0 } },
        { { 0261, 0567, 0227, 0, 0 }, { 0732, 0164, 0207, 0, 0 } },
        { { 0311, 0330, 0456, 0561, 0 }, { 0664, 0715, 0344, 0471, 0 } },
        { { 0315, 0321, 0673, 0, 0 }, { 0423, 0563, 0621, 0713, 0234 } },
        { { 0331, 0372, 0507, 0, 0 }, { 0465, 0656, 056, 0, 0 } },
        { { 0343, 0570, 0324, 0, 0 }, { 0532, 0161, 0345, 0, 0 } },
        { { 0346, 0616, 0635, 0724, 0 }, { 0612, 0706, 0254, 0314, 0 } },
        { { 0371, 0453, 0530, 0217, 0 }, { 0734, 066, 0, 0, 0 } },
        { { 0431, 0730, 0262, 0316, 0 }, { 0723, 0235, 0611, 0671, 0 } },
        { { 0432, 0276, 0326, 0, 0 }, { 0516, 0720, 067, 0, 0 } },
        { { 0466, 0666, 0144, 0, 0 }, { 0662, 0363, 0436, 0443, 0444 } },
        { { 0565, 0307, 0362, 0, 0 }, { 0703, 0150, 0256, 0, 0 } },
        { { 0606, 0630, 0153, 0, 0 }, { 0631, 0636, 0745, 0231, 0504 } },
        { { 0624, 075, 0501, 0, 0 }, { 0632, 0657, 0123, 0, 0 } },
        { { 0654, 0163, 0460, 0607, 0 }, { 0743, 0312, 0515, 0663, 0 } },
        { { 0754, 076, 0203, 0, 0 }, { 0116, 0734, 0, 0, 0 } }
    };

    static inline int octalToDecimal(int v) {
        return (v & 07) + ((v >> 3) & 07) * 10 + ((v >> 6) & 07) * 100;
    }

    // Given the nine code bits of a matched word, reports the code the transmitter
    // would be set to in each polarity.
    //
    // The column the word was found in is the code itself: a radio set to D023 sends
    // a word whose rotations are exactly column 0 of that row, and the complement of
    // that same waveform gives column 1. The squelch this table came from reads the
    // columns the other way round, which has it opening on the wrong polarity.
    static inline void golayFind(int v, int* normal, int* inverted) {
        *normal = 0;
        *inverted = 0;
        if (v == 0) { return; }
        for (int m = 0; m < DCS_CODES_COUNT; m++) {
            for (int n = 0; n < DCS_POLARITIES; n++) {
                for (int p = 0; p < DCS_WORDS_PER_CODE; p++) {
                    if (DCS_CODES[m][n][p] == 0 || DCS_CODES[m][n][p] != v) { continue; }
                    *normal = octalToDecimal(DCS_CODES[m][n][0]);
                    *inverted = octalToDecimal(DCS_CODES[m][(n + 1) % 2][0]);
                    return;
                }
            }
        }
    }

    // Checks the three sync bits and all eleven Golay parity bits of a 23 bit word.
    // Only a real DCS word at the right bit alignment passes, which is what lets the
    // decoder below find its alignment by sliding rather than by searching.
    static inline bool golayMatch(uint32_t v) {
        if ((v & (1u << 9)) != 0 || (v & (1u << 10)) != 0 || (v & (1u << 11)) == 0) { return false; }

        int c1 = (v >> 0) & 1, c2 = (v >> 1) & 1, c3 = (v >> 2) & 1;
        int c4 = (v >> 3) & 1, c5 = (v >> 4) & 1, c6 = (v >> 5) & 1;
        int c7 = (v >> 6) & 1, c8 = (v >> 7) & 1, c9 = (v >> 8) & 1;

        if ((int)((v >> 12) & 1) != ((c1 + c2 + c3 + c4 + c5 + c8) % 2)) { return false; }
        if ((int)((v >> 13) & 1) != (~((c2 + c3 + c4 + c5 + c6 + c9) % 2) & 1)) { return false; }
        if ((int)((v >> 14) & 1) != ((c1 + c2 + c6 + c7 + c8) % 2)) { return false; }
        if ((int)((v >> 15) & 1) != (~((c2 + c3 + c7 + c8 + c9) % 2) & 1)) { return false; }
        if ((int)((v >> 16) & 1) != (~((c1 + c2 + c5 + c9) % 2) & 1)) { return false; }
        if ((int)((v >> 17) & 1) != (~((c1 + c4 + c5 + c6 + c8) % 2) & 1)) { return false; }
        if ((int)((v >> 18) & 1) != ((c1 + c3 + c4 + c6 + c7 + c8 + c9) % 2)) { return false; }
        if ((int)((v >> 19) & 1) != ((c2 + c4 + c5 + c7 + c8 + c9) % 2)) { return false; }
        if ((int)((v >> 20) & 1) != ((c3 + c5 + c6 + c8 + c9) % 2)) { return false; }
        if ((int)((v >> 21) & 1) != (~((c4 + c6 + c7 + c9) % 2) & 1)) { return false; }
        if ((int)((v >> 22) & 1) != (~((c1 + c2 + c3 + c4 + c7) % 2) & 1)) { return false; }

        return true;
    }

    // Every code a transmitter can be set to, ascending, for the squelch's picker.
    // Both columns of the table qualify: a row's two columns are two different
    // codes, each of which some radio out there is set to.
    static inline std::vector<int> dcsCodeList() {
        std::vector<int> codes;
        codes.reserve(DCS_CODES_COUNT * DCS_POLARITIES);
        for (int m = 0; m < DCS_CODES_COUNT; m++) {
            for (int n = 0; n < DCS_POLARITIES; n++) {
                codes.push_back(octalToDecimal(DCS_CODES[m][n][0]));
            }
        }
        std::sort(codes.begin(), codes.end());
        codes.erase(std::unique(codes.begin(), codes.end()), codes.end());
        return codes;
    }
}
