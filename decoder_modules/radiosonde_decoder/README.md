# Radiosonde decoder

Decodes weather balloon radiosondes: the disposable instrument packages launched
twice a day from a few hundred sites, which transmit their position and their
pressure, temperature and humidity in the clear all the way to around 30 km.

Supported: **RS41** (Vaisala), **DFM06/09** (GRAW), **iMS-100 / RS-11G** (Meisei),
**M10 / M20** (Meteomodem), **iMet-1/4** (InterMet), **SRS-C50** (Meteolabor) and
**MRZ-N1** (Meteo-Radiy). They are mostly found between 400 and 406 MHz.

Plus **iMet-54** (InterMet), which is ours — see below.

### The iMet-54 decoder

`sondedump/sonde/imet54/` is **not** upstream sondedump code. Upstream has never had
an iMet-54 decoder, and the iMet-4 decoder next to it cannot be made to do the job:
the -4 is 1200 baud AFSK, the -54 is 4800 baud GFSK, sent 8N1, interleaved in 64 bit
blocks and protected by Hamming[8,4].

It was written against two independent reference implementations, which was the
whole reason for trusting it:

- [`rs1729/RS`](https://github.com/rs1729/RS) `demod/mod/imet54mod.c` (GPL-3.0), the
  SDR decoder that [radiosonde_auto_rx](https://github.com/projecthorus/radiosonde_auto_rx)
  drives as sonde type `IMET5`
- [`sakul7-stack/iMet54`](https://github.com/sakul7-stack/iMet54) (MIT), an
  ESP32/CC1101 receiver where the radio chip demodulates in hardware

The two were reverse engineered separately, for completely different hardware, and
agree on every constant: sync word, baud rate, frame length, the Hamming code and
its lookup table, the 8x8 interleaver, the CRC polynomials and all the field
offsets. Where they agreed, the value was used; nothing here was guessed.

The frame maths also lands the same way from both directions: 220 symbols of 10 bits
= 2200 bits on air, minus the start and stop bits = 1760, minus the four header
symbols = 1728 coded bits = 27 blocks of 64, halving through Hamming[8,4] to the 108
byte payload both sources describe.

Two things the iMet-54 does not send, so the panel cannot show them: **pressure**
(there is no barometer - other sondes' figures are derived from altitude) and a
**date** (only a time of day, so it is combined with today's UTC date, which is
wrong for a few seconds either side of midnight).

There is no automatic type detection — pick the family in the panel. The channel
width changes with the choice, so a wrong pick is usually obvious against the signal
on the waterfall.

## Where the code came from

The decoding is not ours. It is [sondedump](https://github.com/dbdexter-dev/sondedump)
by dbdexter-dev, vendored under `sondedump/`, and the C++ wrapper around it
(`src/decoder.hpp`, `src/common.hpp`) comes from the same author's
[sdrpp_radiosonde](https://github.com/dbdexter-dev/sdrpp_radiosonde) plugin. Both are
MIT licensed; the licences are kept alongside the code as `sondedump/LICENSE` and
`LICENSE.sdrpp_radiosonde`.

What is ours is `src/main.cpp` and `src/main.h`: the DSP path from the VFO to the
decoder, and the panel.

Vendored rather than added as a submodule so that the CI jobs, which check out
without `submodules: recursive`, keep building, and so the shipped `.deb` and `.zip`
artifacts are reproducible from the tree alone.

### What was changed from upstream

Deliberately very little, so that the difference from upstream stays legible:

- `sondedump/CMakeLists.txt` is ours. Upstream's builds sondedump as a program in its
  own right — it declares its own `project()`, requires ncurses and portaudio for a
  TUI this module does not use, and adds `-mcpu=native -mfpu=auto` to the release
  flags on ARM. That last one is right for something compiled on the machine that
  runs it and wrong for a `.deb` that has to run on whatever CPU downloads it. The
  file list is upstream's `LIBRARY_SOURCES`, unchanged.
- The CLI-only parts are not vendored at all: `tui/`, `io/`, `compat/`, `main.c`,
  `decode.c`. Nothing in the library references them.
- `src/common.hpp` gained an `#include <ctime>` for `time_t`, which it had been
  getting by luck through another header.

The decoder sources under `sondedump/` are otherwise untouched. Fixes belong
upstream.

## Notes for anyone updating this

`dsp::demod::FM::init` in this fork takes a fifth `highPass` argument that upstream
SDR++ does not have, so upstream's `main.cpp` will not compile here as it stands.
That argument must stay `false`: the frequency shift keying these sondes use carries
information down to DC, and high passing it stops them decoding.
