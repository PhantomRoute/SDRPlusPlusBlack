# Build file for the vendored sondedump decoder library.
#
# This replaces the one that ships with sondedump rather than reusing it, because
# that one is written for building sondedump as a program in its own right: it
# declares its own project(), looks for ncurses and portaudio, adds install and
# uninstall targets, and - the part that actually matters here - puts
# "-mcpu=native -mfpu=auto" into the release flags whenever the compiler targets
# ARM. That is right for something you compile on the machine you run it on and
# wrong for the .deb and .zip artifacts this repository ships, which have to run on
# whatever CPU the person downloading them has.
#
# The file list below is sondedump's own LIBRARY_SOURCES, unchanged. The sources
# themselves are untouched upstream code - see README.md in this directory.

cmake_minimum_required(VERSION 3.13)

set(SONDEDUMP_SOURCES
    decode/correlator/correlator.c
    decode/framer.c
    decode/manchester.c
    decode/ecc/crc.c
    decode/ecc/rs.c

    demod/dsp/filter.c
    demod/dsp/timing.c
    demod/dsp/agc.c
    demod/gfsk.c
    demod/afsk.c

    sonde/rs41/rs41.c
    sonde/rs41/frame.c
    sonde/rs41/parser.c

    sonde/dfm09/dfm09.c
    sonde/dfm09/frame.c
    sonde/dfm09/parser.c

    sonde/ims100/ims100.c
    sonde/ims100/frame.c
    sonde/ims100/parser.c

    sonde/m10/m10.c
    sonde/m10/frame.c
    sonde/m10/parser.c

    sonde/imet4/imet4.c
    sonde/imet4/frame.c
    sonde/imet4/subframe.c
    sonde/imet4/parser.c

    sonde/c50/c50.c
    sonde/c50/frame.c
    sonde/c50/parser.c

    sonde/mrz-n1/mrzn1.c
    sonde/mrz-n1/frame.c
    sonde/mrz-n1/parser.c

    # Not from upstream sondedump - see ../README.md
    sonde/imet54/imet54.c
    sonde/imet54/frame.c
    sonde/imet54/parser.c

    xdata/xdata.c

    gps/ecef.c
    gps/time.c

    log/log.c

    bitops.c
    physics.c
    utils.c
)

add_library(radiosonde STATIC ${SONDEDUMP_SOURCES})

target_include_directories(radiosonde PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

set_target_properties(radiosonde PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED OFF)

# The library is linked into a shared module, so every object in it has to be
# position independent. Without this it builds and then fails to link on Linux.
set_property(TARGET radiosonde PROPERTY POSITION_INDEPENDENT_CODE ON)

# __global marks the entry points the decoders export. It has no fallback
# definition anywhere in the sources, so leaving it undefined is a compile error
# rather than something that quietly degrades.
if (MSVC)
    target_compile_definitions(radiosonde PRIVATE __global=)
else ()
    target_compile_definitions(radiosonde PRIVATE "__global=__attribute__((visibility(\"default\")))")
endif ()

# Third party code that is not ours to tidy: its warnings would drown out warnings
# from this repository's own sources every build.
if (MSVC)
    target_compile_options(radiosonde PRIVATE /W0)
else ()
    target_compile_options(radiosonde PRIVATE -w)
endif ()

if (NOT WIN32)
    target_link_libraries(radiosonde PUBLIC m)
endif ()
