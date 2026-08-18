cmake_minimum_required(VERSION 3.15)
project(lbzip2 VERSION 2.6.5 LANGUAGES C)

option(LBZIP2_COVERAGE "Build with LLVM source-based coverage instrumentation" OFF)

# An unadorned "cmake .." selects no build type, and therefore no optimisation
# at all, which the autotools build this replaced did not do: its default
# CFLAGS were -g -O2.  The result is a binary roughly twice as slow, reported
# upstream as kjn/lbzip2#39.  A coverage build wants Debug instead, since
# NDEBUG would compile out the assertions, which are code worth covering.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    if(LBZIP2_COVERAGE)
        set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    else()
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_definitions(
    _XOPEN_SOURCE=700
    _FILE_OFFSET_BITS=64
    PACKAGE_NAME="lbzip2"
    PACKAGE_VERSION="${PROJECT_VERSION}"
    PACKAGE_URL="https://github.com/caius72/lbzip2"
)
add_compile_options(-Wall -Wextra)

# Off by default on purpose: a compiler newer than this release will invent
# warnings this code has never seen, and turning those into errors would break
# the build for people who only want to install lbzip2.  CI turns it on.
option(LBZIP2_WERROR "Treat compiler warnings as errors" OFF)
if(LBZIP2_WERROR)
    add_compile_options(-Werror)
endif()

# The profile path is baked in at compile time rather than left to
# LLVM_PROFILE_FILE, because the test driver hands its children an environment
# containing only PATH.  %c asks for continuous mode, without which lbzip2
# records nothing at all: it leaves through _exit(), which skips the atexit
# handler that would otherwise write the profile out.
if(LBZIP2_COVERAGE)
    add_compile_options(-fprofile-instr-generate=${CMAKE_BINARY_DIR}/raw/%c%p.profraw
                        -fcoverage-mapping -O0 -g)
    add_link_options(-fprofile-instr-generate)
endif()

find_package(Threads REQUIRED)

set(SRC_FILES
    src/compress.c
    src/crctab.c
    src/decode.c
    src/divbwt.c
    src/encode.c
    src/expand.c
    src/main.c
    src/parse.c
    src/process.c
    src/signals.c
    src/timespec.c
)

add_executable(lbzip2 ${SRC_FILES})
target_link_libraries(lbzip2 PRIVATE Threads::Threads)

# ponytail: _XOPEN_SOURCE=700 puts macOS headers in strict-POSIX mode, which
# hides st_?timespec and _SC_NPROCESSORS_ONLN.  Scoped to this target because
# the test driver needs strict mode for SIGPOLL, which _DARWIN_C_SOURCE hides.
if(APPLE)
    target_compile_definitions(lbzip2 PRIVATE _DARWIN_C_SOURCE)
endif()

if(LBZIP2_COVERAGE)
    add_custom_target(coverage
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/raw ${CMAKE_BINARY_DIR}/coverage
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
        COMMAND sh ${CMAKE_SOURCE_DIR}/build-aux/coverage.sh ${CMAKE_BINARY_DIR} $<TARGET_FILE:lbzip2>
        DEPENDS lbzip2 driver minbzcat
        USES_TERMINAL
        COMMENT "Running the test suite and summarising coverage")
endif()

include(GNUInstallDirs)
install(TARGETS lbzip2 RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES man/lbzip2.1 man/lbunzip2.1 man/lbzcat.1
        DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)

enable_testing()
add_executable(driver tests/driver.c)
add_executable(minbzcat tests/minbzcat.c)

file(GLOB_RECURSE bz2_files_compress RELATIVE ${CMAKE_SOURCE_DIR}
        tests/suite/manual-compress/*.bz2
        tests/suite/fuzz-collect/*.bz2
        tests/suite/fuzz-divbwt/*.bz2)

file(GLOB_RECURSE bz2_files_expand RELATIVE ${CMAKE_SOURCE_DIR}
        tests/suite/manual-expand/*.bz2)

# Command line behaviour, as opposed to the compressed format.
set(CLI_CASES
    removes_input
    keeps_input_with_k
    removes_input_on_decompress
    roundtrip_preserves_content
    existing_target_skips
    force_overwrites_target
    missing_operand_warns
    directory_operand_warns
    compressed_suffix_skips
    not_bzip2_fails
    force_copies_non_bzip2
    rejects_unknown_option
    rejects_excessive_threads
    accepts_ignored_options
    help_and_version
    test_mode_detects_corruption
)
foreach(case_name ${CLI_CASES})
    add_test(NAME cli_${case_name}
            COMMAND sh ${CMAKE_SOURCE_DIR}/tests/cli.sh ${case_name} $<TARGET_FILE:lbzip2>)
endforeach()

foreach(bz2_file ${bz2_files_compress})
    string(REGEX MATCH "tests/suite/([^/]+)/([^.]+)\\.bz2" _ "${bz2_file}")
    set(suite "${CMAKE_MATCH_1}")
    set(case_id "${CMAKE_MATCH_2}")
    string(REPLACE "-" "_" suite_safe "${suite}")
    set(test_name "${suite_safe}_${case_id}")
    add_test(NAME ${test_name}
            COMMAND driver compress ${CMAKE_SOURCE_DIR} ${suite} ${case_id})
endforeach()

foreach(bz2_file ${bz2_files_expand})
    string(REGEX MATCH "tests/suite/([^/]+)/([^.]+)\\.bz2" _ "${bz2_file}")
    set(suite "${CMAKE_MATCH_1}")
    set(case_id "${CMAKE_MATCH_2}")
    string(REPLACE "-" "_" suite_safe "${suite}")
    set(test_name "${suite_safe}_${case_id}")
    add_test(NAME ${test_name}
            COMMAND driver expand ${CMAKE_SOURCE_DIR} ${suite} ${case_id})
endforeach()
