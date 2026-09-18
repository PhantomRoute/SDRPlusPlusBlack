#pragma once

#include <cctype>
#include <string>
#include <vector>

// The station side of a bookmark: what is on the channel, rather than how to receive
// it. Kept here because it is lists and value conversion, with nothing of the module
// in it, and because the language list is long enough to bury the code it sits in.
namespace station {

    struct Entry {
        const char* code;  // What is stored, in the file and in the CSV
        const char* label; // What is shown
    };

    // ISO 639-1, the two letter codes, which are what broadcast schedules and QSL
    // sites use. Not every language in the standard - the ones a receiver is likely
    // to hear - and "Other" for anything else, so the field is never a dead end.
    //
    // In the order they are shown, which is alphabetical by name rather than by code:
    // the list is read, and a list sorted by a code the reader cannot see looks like
    // no order at all.
    inline const std::vector<Entry>& languages() {
        static const std::vector<Entry> list = {
            { "", "Not set" },
            { "af", "Afrikaans" }, { "ar", "Arabic" }, { "bn", "Bengali" },
            { "bg", "Bulgarian" }, { "zh", "Chinese" }, { "hr", "Croatian" },
            { "cs", "Czech" }, { "da", "Danish" }, { "nl", "Dutch" },
            { "en", "English" }, { "et", "Estonian" }, { "fi", "Finnish" },
            { "fr", "French" }, { "de", "German" }, { "el", "Greek" },
            { "he", "Hebrew" }, { "hi", "Hindi" }, { "hu", "Hungarian" },
            { "is", "Icelandic" }, { "id", "Indonesian" }, { "it", "Italian" },
            { "ja", "Japanese" }, { "ko", "Korean" }, { "lv", "Latvian" },
            { "lt", "Lithuanian" }, { "ms", "Malay" }, { "no", "Norwegian" },
            { "fa", "Persian" }, { "pl", "Polish" }, { "pt", "Portuguese" },
            { "ro", "Romanian" }, { "ru", "Russian" }, { "sr", "Serbian" },
            { "sk", "Slovak" }, { "sl", "Slovenian" }, { "es", "Spanish" },
            { "sw", "Swahili" }, { "sv", "Swedish" }, { "th", "Thai" },
            { "tr", "Turkish" }, { "uk", "Ukrainian" }, { "ur", "Urdu" },
            { "vi", "Vietnamese" }, { "xh", "Xhosa" }, { "zu", "Zulu" },
            { "other", "Other" },
        };
        return list;
    }

    // What kind of station it is. Deliberately short: this is for sorting a long list,
    // not for classifying every service that exists.
    inline const std::vector<Entry>& services() {
        static const std::vector<Entry> list = {
            { "", "Not set" },
            { "broadcast", "Broadcast" },
            { "amateur", "Amateur" },
            { "marine", "Marine" },
            { "aviation", "Aviation" },
            { "utility", "Utility" },
            { "military", "Military" },
            { "emergency", "Emergency services" },
            { "business", "Business / PMR" },
            { "weather", "Weather" },
            { "timesignal", "Time signal" },
            { "beacon", "Beacon" },
            { "satellite", "Satellite" },
            { "other", "Other" },
        };
        return list;
    }

    // The codes as names, in the order they were chosen: "English, Afrikaans". A code
    // that is not in the list is shown as it was stored rather than dropped, so a file
    // from a newer version still says something.
    inline std::string labelsFor(const std::vector<Entry>& list, const std::vector<std::string>& codes) {
        std::string out;
        for (const auto& code : codes) {
            if (code.empty()) { continue; }
            if (!out.empty()) { out += ", "; }
            bool found = false;
            for (auto& e : list) {
                if (code == e.code) {
                    out += e.label;
                    found = true;
                    break;
                }
            }
            if (!found) { out += code; }
        }
        return out;
    }

    // The index of a stored code, or 0 ("Not set") for anything unrecognised - a code
    // from a newer version, or a hand edit. The value itself is left alone in the
    // bookmark, so opening and closing the dialog does not quietly discard it.
    inline int indexOf(const std::vector<Entry>& list, const std::string& code) {
        for (size_t i = 0; i < list.size(); i++) {
            if (code == list[i].code) { return (int)i; }
        }
        return 0;
    }

    // The combo's items, as ImGui wants them: one string with a null after each.
    inline std::string itemsFor(const std::vector<Entry>& list) {
        std::string out;
        for (auto& e : list) {
            out += e.label;
            out.push_back('\0');
        }
        return out;
    }

    // Several codes in one cell, semicolon separated: "en;af". A comma would be the
    // CSV's own separator, and quoting the cell to get round that makes it harder to
    // edit by hand, which is the whole point of the column.
    inline std::string joinCodes(const std::vector<std::string>& codes) {
        std::string out;
        for (const auto& c : codes) {
            if (c.empty()) { continue; }
            if (!out.empty()) { out.push_back(';'); }
            out += c;
        }
        return out;
    }

    // Takes a semicolon or a comma between them, and the spaces someone will leave
    // after typing "en, af".
    inline std::vector<std::string> splitCodes(const std::string& in) {
        std::vector<std::string> out;
        std::string cur;
        for (size_t i = 0; i <= in.size(); i++) {
            char c = (i < in.size()) ? in[i] : ';';
            if (c == ';' || c == ',') {
                size_t a = cur.find_first_not_of(" \t");
                size_t b = cur.find_last_not_of(" \t");
                if (a != std::string::npos) {
                    std::string code = cur.substr(a, b - a + 1);
                    for (char& ch : code) { ch = (char)tolower((unsigned char)ch); }
                    if (!code.empty()) { out.push_back(code); }
                }
                cur.clear();
                continue;
            }
            cur.push_back(c);
        }
        return out;
    }

    // Minutes past midnight UTC from "HH:MM", or -1 if it is not a time. Accepts
    // "0900", "9:00" and "09h00" as well, because those are what people type.
    inline int parseTime(const std::string& in) {
        int digits[4];
        int n = 0;
        for (char c : in) {
            if (c >= '0' && c <= '9') {
                if (n >= 4) { return -1; }
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

    inline std::string fmtTime(int minutes) {
        if (minutes < 0 || minutes > 23 * 60 + 59) { return ""; }
        char buf[8];
        snprintf(buf, sizeof buf, "%02d:%02d", minutes / 60, minutes % 60);
        return buf;
    }

}
