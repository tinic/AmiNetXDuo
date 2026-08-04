# Assert that a linked AmigaOS command still carries its "$VER:" string.
#
#   cmake -DTOOL_BINARY=<file> -P cmake/check-version-tag.cmake
#
# WHY THIS EXISTS
#
# Every command in src/tools/ declares
#
#     static const char version_tag[] __attribute__((used)) = "$VER: ...";
#
# and nothing in the program ever reads it: the AmigaDOS `Version` command
# finds it by scanning the file for the magic prefix.  `used` stops the
# COMPILER discarding it; it says nothing to the linker, and this toolchain
# ignores the `retain` attribute that would (gcc 15.2.0 warns
# "'retain' attribute ignored", the m68k assembler has no SHF_GNU_RETAIN).
#
# So under `--gc-sections` the tag lives or dies by which section it lands in.
# With -fdata-sections it gets a section of its own, nothing relocates to it,
# and every command in the tree silently loses its version string, measured,
# all seventeen of them.  Without -fdata-sections it shares the object's
# read-only section with the string literals main() prints, which are
# referenced, so it survives.  That is why src/tools/CMakeLists.txt passes
# -ffunction-sections and not -fdata-sections.
#
# That is a real mechanism and not a coincidence, but it is one an innocent
# flag change can break without a single test failing, so the build checks the
# linked file instead of trusting the reasoning.
#
# SPDX-License-Identifier: MIT

if(NOT DEFINED TOOL_BINARY)
    message(FATAL_ERROR "check-version-tag.cmake: -DTOOL_BINARY=<file> required")
endif()

file(STRINGS "${TOOL_BINARY}" _version_tag REGEX "\\$VER: " LIMIT_COUNT 1)

if(NOT _version_tag)
    get_filename_component(_name "${TOOL_BINARY}" NAME)
    message(FATAL_ERROR
        "${_name} has no \"\$VER:\" string.\n"
        "The linker has collected the version tag away.  Either the command's\n"
        "source lost its version_tag[], or a compile flag has moved the tag\n"
        "into a section of its own -- -fdata-sections does exactly that, which\n"
        "is why src/tools/CMakeLists.txt does not pass it.")
endif()
