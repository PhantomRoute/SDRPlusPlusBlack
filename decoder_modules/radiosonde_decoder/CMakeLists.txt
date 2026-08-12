cmake_minimum_required(VERSION 3.13)
project(radiosonde_decoder)

file(GLOB_RECURSE SRC "src/*.cpp" "src/*.c")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(radiosonde_decoder PRIVATE "src/")
# The vendored glue includes the decoder headers as "sondedump/include/rs41.h", so
# the directory holding sondedump/ has to be on the path. Keeping it that way means
# decoder.hpp is byte for byte upstream's, which is what makes it obvious later that
# it is not ours to fix.
target_include_directories(radiosonde_decoder PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# The decoders themselves, built as a static library and linked in. EXCLUDE_FROM_ALL
# so that "radiosonde" is only built when this module is, rather than on every build
# of the tree whether or not the module is enabled.
add_subdirectory("sondedump" EXCLUDE_FROM_ALL)
target_link_libraries(radiosonde_decoder PRIVATE radiosonde)
