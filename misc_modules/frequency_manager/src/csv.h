#pragma once

#include <string>
#include <vector>

// RFC 4180 CSV, deliberately with no dependency on anything else in the tree: a
// frequency list is the one thing an operator is most likely to have already, in a
// spreadsheet, and the point of this is that it survives the round trip through
// Excel, LibreOffice and Google Sheets without the notes column losing its commas.
//
// Kept in its own header so it can be built and exercised on its own.
namespace csv {

    // A field needs quoting if it contains a separator, a quote or a line break.
    // Leading and trailing spaces are quoted too - not required by the spec, but
    // without it a note that starts with a space comes back trimmed from some
    // readers, and silently changing what someone typed is worse than a few extra
    // quotes.
    inline std::string escape(const std::string& in) {
        bool needsQuote = false;
        for (char c : in) {
            if (c == ',' || c == '"' || c == '\n' || c == '\r') {
                needsQuote = true;
                break;
            }
        }
        if (!needsQuote && !in.empty() && (in.front() == ' ' || in.back() == ' ')) { needsQuote = true; }
        if (!needsQuote) { return in; }

        std::string out;
        out.reserve(in.size() + 2);
        out.push_back('"');
        for (char c : in) {
            // A quote inside a quoted field is written twice. This is the whole of
            // the escaping scheme - there is no backslash in CSV.
            if (c == '"') { out.push_back('"'); }
            out.push_back(c);
        }
        out.push_back('"');
        return out;
    }

    // CRLF rather than LF: the spec says so, and Excel is the reader most likely to
    // be pointed at the result. Every reader worth using copes with either.
    inline std::string row(const std::vector<std::string>& fields) {
        std::string out;
        for (size_t i = 0; i < fields.size(); i++) {
            if (i) { out.push_back(','); }
            out += escape(fields[i]);
        }
        out += "\r\n";
        return out;
    }

    // Splits a whole file into records. Tolerant on purpose - this is fed files that
    // have been through a spreadsheet, a mail client and someone's text editor:
    //
    //  - a UTF-8 byte order mark is skipped, because Excel writes one and it would
    //    otherwise become part of the first column's name and stop the header from
    //    being recognised
    //  - CRLF, LF and bare CR all end a record
    //  - a file that does not end with a line break still yields its last record
    //  - a line break inside a quoted field is content, not a record separator, which
    //    is what lets a note run to several lines
    //
    // A record can come back with a different number of fields from its neighbours;
    // matching them up is the caller's problem, since only the caller knows what the
    // columns are supposed to be.
    inline std::vector<std::vector<std::string>> parse(const std::string& text) {
        std::vector<std::vector<std::string>> rows;
        std::vector<std::string> cur;
        std::string field;
        bool inQuotes = false;
        // Tells a field that was written as "" apart from one that was simply absent,
        // so a trailing empty field on the last line is not thrown away.
        bool fieldStarted = false;

        size_t i = 0;
        if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
            i = 3;
        }

        for (; i < text.size(); i++) {
            char c = text[i];

            if (inQuotes) {
                if (c == '"') {
                    if (i + 1 < text.size() && text[i + 1] == '"') {
                        field.push_back('"');
                        i++;
                    }
                    else {
                        inQuotes = false;
                    }
                }
                else if (c == '\r') {
                    // Normalised to a bare newline. A note typed in the program uses
                    // one; a note that has been through Excel comes back with both,
                    // and it should not grow a stray character every round trip.
                    if (i + 1 < text.size() && text[i + 1] == '\n') { i++; }
                    field.push_back('\n');
                }
                else {
                    field.push_back(c);
                }
                continue;
            }

            if (c == '"' && field.empty()) {
                inQuotes = true;
                fieldStarted = true;
                continue;
            }
            if (c == ',') {
                cur.push_back(field);
                field.clear();
                fieldStarted = false;
                continue;
            }
            if (c == '\r' || c == '\n') {
                if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') { i++; }
                cur.push_back(field);
                field.clear();
                fieldStarted = false;
                rows.push_back(cur);
                cur.clear();
                continue;
            }
            field.push_back(c);
        }

        if (!field.empty() || fieldStarted || !cur.empty()) {
            cur.push_back(field);
            rows.push_back(cur);
        }
        return rows;
    }

    // True for a record that carries nothing at all. A blank line is a record by the
    // letter of the spec, but nobody means one by it, and importing it as a bookmark
    // with no name would be obtuse.
    inline bool rowIsBlank(const std::vector<std::string>& fields) {
        for (const auto& f : fields) {
            if (!f.empty()) { return false; }
        }
        return true;
    }

    // Header matching is deliberately loose - case, spaces and underscores all
    // ignored - so that "Tone Mode", "tone_mode" and "TONEMODE" are the same column.
    // Someone who has kept a list in a spreadsheet for years should not have to
    // rename their headings to get it in.
    inline std::string normaliseHeader(const std::string& in) {
        std::string out;
        out.reserve(in.size());
        for (char c : in) {
            if (c == ' ' || c == '_' || c == '-' || c == '\t') { continue; }
            out.push_back((c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c);
        }
        return out;
    }

}
