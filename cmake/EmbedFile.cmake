# Turns a binary file into a C++ source defining a byte array.
#
#   cmake -DINPUT=<file> -DOUTPUT=<file.cpp> -DSYMBOL=<name> -P EmbedFile.cmake
#
# Run at build time by add_custom_command, so the vendored asset stays
# byte-identical to upstream and no multi-megabyte generated file lives in git.
# Portable C++20 has no #embed, and this needs no extra tool on any platform.

if(NOT INPUT OR NOT OUTPUT OR NOT SYMBOL)
    message(FATAL_ERROR "EmbedFile.cmake needs INPUT, OUTPUT and SYMBOL")
endif()

file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" hex_length)
math(EXPR byte_count "${hex_length} / 2")

string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")

# CMake regex has no {n} quantifier, so spell out 24 bytes per line. Short
# lines keep compiler diagnostics and editors sane on a 400 KB array.
set(group "")
foreach(i RANGE 1 24)
    string(APPEND group "0x[0-9a-f][0-9a-f],")
endforeach()
string(REGEX REPLACE "(${group})" "\\1\n    " bytes "${bytes}")

get_filename_component(input_name "${INPUT}" NAME)
file(WRITE "${OUTPUT}"
"// Generated from ${input_name} by cmake/EmbedFile.cmake. Do not edit.
#include <cstddef>

namespace em::embedded {

alignas(4) extern const unsigned char ${SYMBOL}[] = {
    ${bytes}
};
extern const std::size_t ${SYMBOL}Size = ${byte_count};

}  // namespace em::embedded
")
