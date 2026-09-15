# Recorder codecs

Encoders for the recorder's compressed audio formats. They are kept in the tree so every
SDR++ target builds them the same way, with nothing to install and nothing to download.
Only the files the encoders need were taken; each keeps its upstream licence.

| Codec | Version | Licence | Source | SHA-512 of the release archive |
|---|---|---|---|---|
| LAME (MP3) | 3.100 | LGPL-2.0, see `lame/COPYING` | https://downloads.sourceforge.net/project/lame/lame/3.100/lame-3.100.tar.gz | `0844b9eadb4aacf8000444621451277de365041cc1d97b7f7a589da0b7a23899310afd4e4d81114b9912aa97832621d20588034715573d417b2923948c08634b` |
| libFLAC | 1.5.0 | BSD-3-Clause, see `flac/COPYING.Xiph` | https://github.com/xiph/flac/archive/refs/tags/1.5.0.tar.gz | `c8e119462205cfd8bbe22b0aff112625d3e51ca11de97e4de06a46fb43a0768d7ec9c245b299b09b7aa4d811c6fc7b57856eaa1c217e82cca9b3ad1c0e545cbe` |

The hashes are the ones vcpkg pins for the same releases.

## What was taken

- **LAME:** `include/lame.h`, `libmp3lame/*.c` and `*.h` except `mpglib_interface.c` (the
  decoder hook), and `libmp3lame/vector/lame_intrin.h`. The SSE file, the assembly, the
  frontend and mpglib are left out.
- **libFLAC:** the public headers in `include/FLAC`, the few `include/share` headers the
  library uses, the library sources the encoder and its decoder need, their private and
  protected headers, `deduplication/`, and `win_utf8_io.c` for Windows paths. The Ogg,
  intrinsics and assembly files are left out.

Neither upstream file was modified. `lame/config.h` and `flac/config.h` are written for
SDR++ in place of the ones the upstream build systems generate, and `CMakeLists.txt` here
builds both as static libraries for the recorder.

## Updating

Download the new release, check its hash against the vcpkg port, copy the same set of
files over, and compare the upstream config template against the matching `config.h`
here for anything new.
