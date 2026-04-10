# spirv_to_header.cmake — Convert a SPIR-V binary to a C++ header.
#
# Expected variables (passed via -D):
#   SPIRV_FILE  — path to the .spv binary
#   HEADER_FILE — path to the output .h file
#   ARRAY_NAME  — C++ identifier for the array

file(READ "${SPIRV_FILE}" spirv_hex HEX)

# SPIR-V is a stream of 32-bit words (little-endian on disk).
string(LENGTH "${spirv_hex}" hex_len)
math(EXPR word_count "${hex_len} / 8")

set(body "")
math(EXPR last "${word_count} - 1")
foreach(i RANGE ${last})
    math(EXPR offset "${i} * 8")
    string(SUBSTRING "${spirv_hex}" ${offset} 8 word)

    # Reorder bytes: file is little-endian, emit as 0xAABBCCDD.
    string(SUBSTRING "${word}" 6 2 b3)
    string(SUBSTRING "${word}" 4 2 b2)
    string(SUBSTRING "${word}" 2 2 b1)
    string(SUBSTRING "${word}" 0 2 b0)

    if(i EQUAL last)
        string(APPEND body "    0x${b3}${b2}${b1}${b0}\n")
    else()
        string(APPEND body "    0x${b3}${b2}${b1}${b0},\n")
    endif()
endforeach()

cmake_path(GET SPIRV_FILE FILENAME spirv_filename)

file(WRITE "${HEADER_FILE}"
"#pragma once\n\
#include <cstdint>\n\
\n\
// Auto-generated from ${spirv_filename} — do not edit.\n\
inline constexpr uint32_t ${ARRAY_NAME}[] = {\n\
${body}};\n\
\n\
inline constexpr size_t ${ARRAY_NAME}_size = sizeof(${ARRAY_NAME}) / sizeof(uint32_t);\n"
)
