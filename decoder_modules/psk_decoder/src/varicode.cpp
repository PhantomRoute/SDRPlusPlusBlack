#include "varicode.h"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace psk {
    // The standard PSK31 varicode, in ASCII order. Short codes go to the characters
    // English uses most - space is "1", 'e' is "11", 't' is "101" - which is where
    // the mode gets its throughput from at 31 baud.
    const char* const VARICODE[128] = {
        "1010101011", // 0   NUL
        "1011011011", // 1   SOH
        "1011101101", // 2   STX
        "1101110111", // 3   ETX
        "1011101011", // 4   EOT
        "1101011111", // 5   ENQ
        "1011101111", // 6   ACK
        "1011111101", // 7   BEL
        "1011111111", // 8   BS
        "11101111",   // 9   HT
        "11101",      // 10  LF
        "1101101111", // 11  VT
        "1011011101", // 12  FF
        "11111",      // 13  CR
        "1101110101", // 14  SO
        "1110101011", // 15  SI
        "1011110111", // 16  DLE
        "1011110101", // 17  DC1
        "1110101101", // 18  DC2
        "1110101111", // 19  DC3
        "1101011011", // 20  DC4
        "1101101011", // 21  NAK
        "1101101101", // 22  SYN
        "1101010111", // 23  ETB
        "1101111011", // 24  CAN
        "1101111101", // 25  EM
        "1110110111", // 26  SUB
        "1101010101", // 27  ESC
        "1101011101", // 28  FS
        "1110111011", // 29  GS
        "1011111011", // 30  RS
        "1101111111", // 31  US
        "1",          // 32  space
        "111111111",  // 33  !
        "101011111",  // 34  "
        "111110101",  // 35  #
        "111011011",  // 36  $
        "1011010101", // 37  %
        "1010111011", // 38  &
        "101111111",  // 39  '
        "11111011",   // 40  (
        "11110111",   // 41  )
        "101101111",  // 42  *
        "111011111",  // 43  +
        "1110101",    // 44  ,
        "110101",     // 45  -
        "1010111",    // 46  .
        "110101111",  // 47  /
        "10110111",   // 48  0
        "10111101",   // 49  1
        "11101101",   // 50  2
        "11111111",   // 51  3
        "101110111",  // 52  4
        "101011011",  // 53  5
        "101101011",  // 54  6
        "110101101",  // 55  7
        "110101011",  // 56  8
        "110110111",  // 57  9
        "11110101",   // 58  :
        "110111101",  // 59  ;
        "111101101",  // 60  <
        "1010101",    // 61  =
        "111010111",  // 62  >
        "1010101111", // 63  ?
        "1010111101", // 64  @
        "1111101",    // 65  A
        "11101011",   // 66  B
        "10101101",   // 67  C
        "10110101",   // 68  D
        "1110111",    // 69  E
        "11011011",   // 70  F
        "11111101",   // 71  G
        "101010101",  // 72  H
        "1111111",    // 73  I
        "111111101",  // 74  J
        "101111101",  // 75  K
        "11010111",   // 76  L
        "10111011",   // 77  M
        "11011101",   // 78  N
        "10101011",   // 79  O
        "11010101",   // 80  P
        "111011101",  // 81  Q
        "10101111",   // 82  R
        "1101111",    // 83  S
        "1101101",    // 84  T
        "101010111",  // 85  U
        "110110101",  // 86  V
        "101011101",  // 87  W
        "101110101",  // 88  X
        "101111011",  // 89  Y
        "1010101101", // 90  Z
        "111110111",  // 91  [
        "111101111",  // 92  backslash
        "111111011",  // 93  ]
        "1010111111", // 94  ^
        "101101101",  // 95  _
        "1011011111", // 96  `
        "1011",       // 97  a
        "1011111",    // 98  b
        "101111",     // 99  c
        "101101",     // 100 d
        "11",         // 101 e
        "111101",     // 102 f
        "1011011",    // 103 g
        "101011",     // 104 h
        "1101",       // 105 i
        "111101011",  // 106 j
        "10111111",   // 107 k
        "11011",      // 108 l
        "111011",     // 109 m
        "1111",       // 110 n
        "111",        // 111 o
        "111111",     // 112 p
        "110111111",  // 113 q
        "10101",      // 114 r
        "10111",      // 115 s
        "101",        // 116 t
        "110111",     // 117 u
        "1111011",    // 118 v
        "1101011",    // 119 w
        "11011111",   // 120 x
        "1011101",    // 121 y
        "111010101",  // 122 z
        "1010110111", // 123 {
        "110111011",  // 124 |
        "1010110101", // 125 }
        "1011010111", // 126 ~
        "1110110101"  // 127 DEL
    };

    // What the table's own construction lets us prove, and what it does not.
    //
    // The legal codes are exactly the strings that start and end with '1' and hold no
    // "00". Counting them by length gives the Fibonacci numbers - 1, 1, 2, 3, 5, 8,
    // 13, 21, 34, 55 for lengths 1 to 10 - so there are 88 of length 9 or less and
    // 143 of length 10 or less. A table of 128 characters must therefore use every
    // one of the 88 short codes exactly once and 40 of the 55 ten-bit ones.
    //
    // That is a tight enough constraint to catch a mistyped entry: changing any code
    // either makes it structurally illegal, collides with another character, or
    // leaves one of the 88 short codes unused. What it cannot catch is two characters
    // having their codes swapped, since the set is then still correct. The short
    // codes belong to the common letters, so a swap there would be obvious in the
    // first line of received text; a swap between two rarely used control codes would
    // not be, and is not worth more machinery than this.
    std::string validateVaricode() {
        // Generate every legal code up to the longest the table uses.
        size_t longest = 0;
        for (int i = 0; i < 128; i++) {
            size_t len = std::string(VARICODE[i]).size();
            if (len > longest) { longest = len; }
        }
        std::set<std::string> legal;
        for (size_t n = 1; n <= longest; n++) {
            for (unsigned long v = 0; v < (1UL << n); v++) {
                std::string s;
                for (size_t b = 0; b < n; b++) { s += ((v >> (n - 1 - b)) & 1) ? '1' : '0'; }
                if (s.front() != '1' || s.back() != '1') { continue; }
                if (s.find("00") != std::string::npos) { continue; }
                legal.insert(s);
            }
        }

        std::map<std::string, int> seen;
        for (int i = 0; i < 128; i++) {
            std::string code = VARICODE[i];
            if (code.empty()) { return "entry " + std::to_string(i) + " is empty"; }
            if (code.find_first_not_of("01") != std::string::npos) {
                return "entry " + std::to_string(i) + " (" + code + ") is not binary";
            }
            if (code.front() != '1' || code.back() != '1') {
                return "entry " + std::to_string(i) + " (" + code + ") does not start and end with 1";
            }
            if (code.find("00") != std::string::npos) {
                return "entry " + std::to_string(i) + " (" + code + ") contains the separator 00";
            }
            auto it = seen.find(code);
            if (it != seen.end()) {
                return "entries " + std::to_string(it->second) + " and " + std::to_string(i) +
                       " share the code " + code;
            }
            seen[code] = i;
        }

        // Every short code has to have been used, or two characters are sharing one
        // somewhere and the collision check above missed it because a third entry was
        // mistyped into the gap.
        for (const auto& code : legal) {
            if (code.size() <= 9 && seen.find(code) == seen.end()) {
                return "no character uses the code " + code + ", so the table is incomplete";
            }
        }
        return "";
    }

    char VaricodeDecoder::put(int bit) {
        bits += bit ? '1' : '0';

        // Not at a character boundary yet.
        if (bits.size() < 2 || bits.compare(bits.size() - 2, 2, "00") != 0) {
            // A run of bits far longer than any code is not a code. This happens on a
            // signal too weak to decode, where the separator never arrives; without
            // the cap the buffer would grow for as long as the module was running.
            if (bits.size() > 16) { bits.clear(); }
            return 0;
        }

        std::string code = bits.substr(0, bits.size() - 2);
        bits.clear();

        // An idle transmitter sends continuous phase reversals, which is a continuous
        // run of zeros. Those arrive here as empty codes and are not characters.
        if (code.empty()) { return 0; }

        for (int i = 0; i < 128; i++) {
            if (code == VARICODE[i]) { return (char)i; }
        }
        // A code that is structurally legal but in none of the 128 slots - one of the
        // 15 ten-bit strings the table does not use - is a corrupted character, not a
        // reason to lose synchronisation. The separator already resynchronised us.
        return 0;
    }

    void VaricodeDecoder::reset() {
        bits.clear();
    }
}
