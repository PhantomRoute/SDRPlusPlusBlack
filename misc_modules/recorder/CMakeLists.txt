cmake_minimum_required(VERSION 3.13)
project(recorder)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(recorder PRIVATE "src/")
target_include_directories(recorder PRIVATE "../../decoder_modules/radio/src")

# MP3 and FLAC encoders, built from the sources under codecs/.
add_subdirectory(codecs)
target_link_libraries(recorder PRIVATE sdrpp_lame sdrpp_flac)
