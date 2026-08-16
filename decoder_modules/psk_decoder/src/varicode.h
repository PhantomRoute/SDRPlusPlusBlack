#pragma once
#include <string>

namespace psk {
    // PSK31 varicode. One entry per ASCII code point, each a string of '1' and '0'.
    //
    // Three rules govern every code, and they are what make the stream self
    // synchronising: a code starts and ends with '1', never contains "00", and "00"
    // is what separates one character from the next. A receiver that joins the
    // stream part way through only has to wait for the next "00".
    extern const char* const VARICODE[128];

    // Checks VARICODE against what its construction guarantees, which is enough to
    // catch a mistyped or duplicated entry. See varicode.cpp for what is and is not
    // provable this way. Returns an empty string when the table is sound, otherwise
    // a description of the first problem found.
    std::string validateVaricode();

    // Turns a stream of bits into text. Bits go in one at a time in the order they
    // came off the air; characters come out whenever a separator completes one.
    class VaricodeDecoder {
    public:
        // Returns the decoded character, or 0 if this bit did not complete one.
        // A run of zeros - which is what an idle PSK31 transmitter sends - produces
        // nothing rather than a stream of nulls.
        char put(int bit);

        void reset();

    private:
        // The bits since the last separator. Never longer than the longest code plus
        // the two zeros that end it, because anything longer cannot be a code and is
        // thrown away.
        std::string bits;
    };
}
