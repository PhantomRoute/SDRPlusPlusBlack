#pragma once

#include "csv.h"
#include "../../../decoder_modules/radio/src/radio_interface.h"
#include <cmath>
#include <cstdio>
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
