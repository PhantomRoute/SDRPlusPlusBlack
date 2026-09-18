#pragma once

#include "csv.h"
#include "../../../decoder_modules/radio/src/radio_interface.h"
#include <cmath>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <string>
#include <vector>

// Turning a bookmark into spreadsheet columns and back.
//
// Separate from main.cpp because none of it needs the module: it is value
// conversion, which is where the fiddly cases live - a frequency someone typed as
// "145.5 MHz", a boolean someone typed as "Y", a tone list that has to survive in a
// single cell. Keeping it here means it can be built and tested on its own.
namespace bmcsv {

    // The column order a file is written in. Reading matches on the heading instead,
    // so a file whose columns have been reordered, or which is missing all but name
    // and frequency, still imports.
    inline const std::vector<std::string>& columns() {
        static const std::vector<std::string> cols = {
            "name", "frequency", "bandwidth", "mode", "vfo",
            "tone_mode", "ctcss", "dcs_code", "dcs_invert",
            "tone_squelch", "tone_filter", "tone_identify", "tone_tail_close",
            "tone_list",
            // Whether the scanner passes over this channel. A column rather than
            // something left behind on export, because a list is often built and
            // pruned in a spreadsheet, and "which of these am I not interested in"
            // is exactly the kind of decision made there.
            "skip",
            // The station: what is on the channel rather than how to receive it. A
            // spreadsheet is where a list like this is usually built, so these are
            // columns like any other and round trip the same way.
            "full_name", "language", "service",
            "always_on", "active_from_utc", "active_to_utc",
            "times_heard", "last_heard_utc",
            // Last on purpose: it is the one field with no length limit, and a long
            // free text column in the middle pushes everything else off the screen in
            // a spreadsheet.
            "notes"
        };
        return cols;
    }

    // Enough decimals for a frequency in Hz, with the trailing zeros taken off so a
    // whole number of Hz reads as one. Never exponent notation - a spreadsheet will
    // happily take 1.455e+08 back, but nobody wants to read it.
    inline std::string fmtNumber(double v) {
        if (!std::isfinite(v)) { return "0"; }
        char buf[64];
        snprintf(buf, sizeof buf, "%.6f", v);
        std::string s = buf;
        size_t dot = s.find('.');
        if (dot != std::string::npos) {
            size_t last = s.find_last_not_of('0');
            if (last == dot) { last--; }
            s.erase(last + 1);
        }
        return s;
    }

    // Hz unless the cell says otherwise. A suffix is honoured because a list someone
    // has kept by hand is as likely to be in MHz as in Hz, and "145.5 MHz" says which
    // it is - unlike a bare 145.5, which is a guess either way and so is left alone.
    inline bool parseNumber(const std::string& in, double* out) {
        size_t i = 0;
        while (i < in.size() && (in[i] == ' ' || in[i] == '\t')) { i++; }
        size_t start = i;
        while (i < in.size() && (isdigit((unsigned char)in[i]) || in[i] == '+' || in[i] == '-' || in[i] == '.' || in[i] == 'e' || in[i] == 'E')) { i++; }
        if (i == start) { return false; }
        std::string num = in.substr(start, i - start);
        char* end = nullptr;
        double v = strtod(num.c_str(), &end);
        if (end == num.c_str()) { return false; }

        while (i < in.size() && (in[i] == ' ' || in[i] == '\t')) { i++; }
        if (i < in.size()) {
            char u = in[i];
            if (u == 'k' || u == 'K') { v *= 1e3; }
            else if (u == 'M' || u == 'm') { v *= 1e6; }
            else if (u == 'G' || u == 'g') { v *= 1e9; }
        }
        *out = v;
        return true;
    }

    // "yes"/"no" rather than 1/0: this is a file people open and edit by hand, and a
    // column of yes and no needs no explaining.
    inline std::string fmtBool(bool v) { return v ? "yes" : "no"; }

    inline bool parseBool(const std::string& in, bool def) {
        for (size_t i = 0; i < in.size(); i++) {
            char c = in[i];
            if (c == ' ' || c == '\t') { continue; }
            if (c == 'y' || c == 'Y' || c == 't' || c == 'T' || c == '1') { return true; }
            if (c == 'n' || c == 'N' || c == 'f' || c == 'F' || c == '0') { return false; }
            return def;
        }
        return def;
    }

    // The squelch mode, named rather than numbered. The numbers are an enum in a
    // header; they mean nothing in a spreadsheet and would silently change meaning if
    // the enum were ever reordered.
    inline std::string fmtToneMode(int mode) {
        switch (mode) {
        case 1: return "ctcss";
        case 2: return "dcs";
        case 3: return "any";
        case 4: return "list";
        default: return "off";
        }
    }

    inline int parseToneMode(const std::string& in) {
        std::string s = csv::normaliseHeader(in); // same lowercase-and-strip treatment
        if (s == "ctcss") { return 1; }
        if (s == "dcs") { return 2; }
        if (s == "any" || s == "anytone") { return 3; }
        if (s == "list" || s == "customlist") { return 4; }
        return 0;
    }

    // "HH:MM" in the cell, minutes past midnight in the bookmark. Empty for a time
    // that is not set, which a spreadsheet shows as an empty cell rather than a zero
    // that would read as midnight.
    inline std::string fmtTimeOfDay(int minutes) {
        if (minutes < 0 || minutes > 23 * 60 + 59) { return ""; }
        char buf[8];
        snprintf(buf, sizeof buf, "%02d:%02d", minutes / 60, minutes % 60);
        return buf;
    }

    // Takes what a spreadsheet is likely to hand back as well as what was written:
    // "09:30", "9:30", "0930", and Excel's habit of turning a time into "09:30:00".
    inline int parseTimeOfDay(const std::string& in) {
        int digits[6];
        int n = 0;
        for (char c : in) {
            if (c >= '0' && c <= '9') {
                if (n >= 6) { break; }
                digits[n++] = c - '0';
            }
        }
        if (n == 0) { return -1; }
        int h = 0, m = 0;
        if (n <= 2) { h = (n == 1) ? digits[0] : digits[0] * 10 + digits[1]; }
        else if (n == 3) { h = digits[0]; m = digits[1] * 10 + digits[2]; }
        else { h = digits[0] * 10 + digits[1]; m = digits[2] * 10 + digits[3]; }
        if (h > 23 || m > 59) { return -1; }
        return h * 60 + m;
    }

    // A date and time a person and a spreadsheet can both read, in UTC like everything
    // else here. Stored as a Unix time, which no one wants to see in a cell.
    inline std::string fmtDateTime(long long unixSeconds) {
        if (unixSeconds <= 0) { return ""; }
        std::time_t t = (std::time_t)unixSeconds;
        std::tm* gm = std::gmtime(&t);
        if (gm == NULL) { return ""; }
        char buf[32];
        if (strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", gm) == 0) { return ""; }
        return buf;
    }

    inline long long parseDateTime(const std::string& in) {
        int y = 0, mo = 0, d = 0, h = 0, mi = 0;
        if (sscanf(in.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) < 3) { return 0; }
        if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31) { return 0; }
        // Days since the epoch, worked out directly: timegm is not portable and mktime
        // would read the cell as local time, which is the one thing it is not.
        static const int cumulative[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
        long long days = (long long)(y - 1970) * 365 + ((y - 1969) / 4) - ((y - 1901) / 100) + ((y - 1601) / 400);
        days += cumulative[mo - 1];
        bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
        if (leap && mo > 2) { days += 1; }
        days += d - 1;
        return days * 86400LL + h * 3600LL + mi * 60LL;
    }

    // The accept list in one cell, semicolon separated: "100.0;D023N;123.0". A CTCSS
    // entry is its frequency and a DCS entry is the code the way a radio writes it,
    // so the cell is readable and editable without a key to it.
    inline std::string encodeToneList(const RadioToneSettings& t) {
        std::string out;
        for (int i = 0; i < t.listCount && i < RADIO_TONE_LIST_MAX; i++) {
            if (!out.empty()) { out.push_back(';'); }
            const RadioToneListEntry& e = t.list[i];
            if (e.kind == 1) {
                char buf[16];
                snprintf(buf, sizeof buf, "D%03d%c", e.dcsCode, e.dcsInverted ? 'I' : 'N');
                out += buf;
            }
            else {
                out += fmtNumber(e.ctcssFreq);
            }
        }
        return out;
    }

    inline void decodeToneList(const std::string& in, RadioToneSettings& t) {
        t.listCount = 0;
        size_t pos = 0;
        while (pos <= in.size() && t.listCount < RADIO_TONE_LIST_MAX) {
            size_t end = in.find(';', pos);
            if (end == std::string::npos) { end = in.size(); }
            std::string item = in.substr(pos, end - pos);
            pos = end + 1;

            size_t a = item.find_first_not_of(" \t");
            if (a == std::string::npos) {
                if (end >= in.size()) { break; }
                continue;
            }
            size_t b = item.find_last_not_of(" \t");
            item = item.substr(a, b - a + 1);
            if (item.empty()) {
                if (end >= in.size()) { break; }
                continue;
            }

            RadioToneListEntry e;
            if (item[0] == 'D' || item[0] == 'd') {
                e.kind = 1;
                e.dcsCode = atoi(item.c_str() + 1);
                char last = item[item.size() - 1];
                e.dcsInverted = (last == 'I' || last == 'i');
                e.ctcssFreq = 100.0f;
            }
            else {
                double v = 0.0;
                if (!parseNumber(item, &v)) {
                    if (end >= in.size()) { break; }
                    continue;
                }
                e.kind = 0;
                e.ctcssFreq = (float)v;
                e.dcsCode = 23;
                e.dcsInverted = false;
            }
            t.list[t.listCount++] = e;
            if (end >= in.size()) { break; }
        }
    }

}
