# Encoders for the recorder's compressed audio formats, built from source kept in the
# tree so every target builds them the same way with no packages to install and nothing
# to download. See README.md here for where each came from.

# ---- LAME 3.100, the MP3 encoder (LGPL-2.0). The library only.
file(GLOB SDRPP_LAME_SRC "${CMAKE_CURRENT_SOURCE_DIR}/lame/libmp3lame/*.c")
add_library(sdrpp_lame STATIC ${SDRPP_LAME_SRC})
target_include_directories(sdrpp_lame
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/lame" "${CMAKE_CURRENT_SOURCE_DIR}/lame/libmp3lame"
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/lame/include")
target_compile_definitions(sdrpp_lame PRIVATE HAVE_CONFIG_H)
set_target_properties(sdrpp_lame PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---- libFLAC 1.5.0 (BSD-3-Clause). Portable code only, no Ogg.
set(SDRPP_FLAC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/flac")
set(SDRPP_FLAC_SRC
    ${SDRPP_FLAC_DIR}/src/libFLAC/bitmath.c
    ${SDRPP_FLAC_DIR}/src/libFLAC/bitreader.c
    ${SDRPP_FLAC_DIR}/src/libFLAC/bitwriter.c
    ${SDRPP_FLAC_DIR}/src/libFLAC/cpu.c
    ${SDRPP_FLAC_DIR}/src/libFLAC/crc.c
    ${SDRPP_FLAC_DIR}/src/libFLAC/fixed.c
    ${SDRPP_FLAC_DIR}/src/libFLAC/float.c
    ${SDRPP_FLAC_DIR}/src/libFLAC/format.c
    ${SDRPP_FLAC_DIR}/src/libFLAC/lpc.c
    ${SDRPP_FLAC_DIR}/src/libFLAC/md5.c
    ${SDRPP_FLAC_DIR}/src/libFLAC/memory.c
    ${SDRPP_FLAC_DIR}/src/libFLAC/metadata_iterators.c
    ${SDRPP_FLAC_DIR}/src/libFLAC/metadata_object.c
    ${SDRPP_FLAC_DIR}/src/libFLAC/stream_decoder.c
    ${SDRPP_FLAC_DIR}/src/libFLAC/stream_encoder.c
    ${SDRPP_FLAC_DIR}/src/libFLAC/stream_encoder_framing.c
    ${SDRPP_FLAC_DIR}/src/libFLAC/window.c)
if (WIN32)
    list(APPEND SDRPP_FLAC_SRC ${SDRPP_FLAC_DIR}/src/share/win_utf8_io/win_utf8_io.c)
endif ()
add_library(sdrpp_flac STATIC ${SDRPP_FLAC_SRC})
target_include_directories(sdrpp_flac
    PRIVATE "${SDRPP_FLAC_DIR}" "${SDRPP_FLAC_DIR}/src/libFLAC/include"
    PUBLIC "${SDRPP_FLAC_DIR}/include")
target_compile_definitions(sdrpp_flac PRIVATE HAVE_CONFIG_H FLAC__NO_ASM PUBLIC FLAC__NO_DLL)
set_target_properties(sdrpp_flac PROPERTIES POSITION_INDEPENDENT_CODE ON)
if (NOT WIN32)
    include(CheckLibraryExists)
    check_library_exists(m lround "" SDRPP_FLAC_NEEDS_LIBM)
    if (SDRPP_FLAC_NEEDS_LIBM)
        target_link_libraries(sdrpp_flac PUBLIC m)
    endif ()
endif ()

# Third-party code: its own warnings are not ours to fix.
foreach (codec sdrpp_lame sdrpp_flac)
    if (MSVC)
        target_compile_options(${codec} PRIVATE /W0 /O2 /wd4996)
    else ()
        target_compile_options(${codec} PRIVATE -w -O2)
    endif ()
endforeach ()
