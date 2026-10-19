cmake_minimum_required(VERSION 3.20)
project(mirage_emulator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Folder holding the user's ROM and disk images (never copied into this repo).
set(MIRAGE_ASSET_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH
    "Folder containing the Mirage ROM and disk images")

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
endif()

include(FetchContent)
FetchContent_Declare(Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.7.1)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")

enable_testing()
add_subdirectory(engine)
# mirage_cli's option parsing and event scheduling, shared with the unit tests.
add_library(mirage_cli_core STATIC tools/mirage_cli/run_options.cpp)
target_include_directories(mirage_cli_core PUBLIC tools/mirage_cli)
target_link_libraries(mirage_cli_core PUBLIC mirage_engine)
add_subdirectory(tests)
add_executable(mirage_cli tools/mirage_cli/main.cpp)
target_link_libraries(mirage_cli PRIVATE mirage_cli_core)
target_compile_definitions(mirage_cli PRIVATE MIRAGE_ASSET_ROOT="${MIRAGE_ASSET_ROOT}")

# mirage_cli end to end: exact exit code plus stdout/stderr regexes
# (tests/cli_check.cmake). Arguments are separated by "|".
set(MIRAGE_TEST_ROM "${MIRAGE_ASSET_ROOT}/Ensoniq Mirage EPROM Firmware/mirage.bin")
set(MIRAGE_TEST_SND_A01 "${MIRAGE_ASSET_ROOT}/Factory Library/IMG/SND-A01.img")
function(mirage_cli_test name rc args)
  add_test(NAME ${name}
           COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:mirage_cli> "-DARGS=${args}" -DRC=${rc}
                   ${ARGN} -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_check.cmake)
  set_tests_properties(${name} PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP: missing asset")
endfunction()

# Usage errors (exit 2) and unavailable options (exit 1) fail before the ROM loads.
mirage_cli_test(cli_run_unknown_option 2 "run|--no-such-option|1" "-DERR=^usage:")
mirage_cli_test(cli_run_missing_value 2 "run|--disk" "-DERR=^usage:")
mirage_cli_test(cli_run_negative_seconds 2 "run|--seconds|-1"
                "-DERR=^bad value for --seconds: -1/$")
mirage_cli_test(cli_run_unknown_key 2 "run|--key|bogus@1" "-DERR=^bad value for --key: bogus@1/$")
mirage_cli_test(cli_run_note_without_duration 2 "run|--note|60:100@1"
                "-DERR=^bad value for --note: 60:100@1/$")
mirage_cli_test(cli_run_bad_midi_hex 2 "run|--midi-hex|ZZ@1"
                "-DERR=^bad value for --midi-hex: ZZ@1/$")
mirage_cli_test(cli_run_bad_rate 2 "run|--rate|100" "-DERR=^bad value for --rate: 100/$")
mirage_cli_test(cli_run_snapshot_in_with_disk 2 "run|--disk|d.img|--snapshot-in|x.snap"
                "-DERR=^--disk cannot be combined with --snapshot-in: the snapshot holds its own disk/$")
mirage_cli_test(cli_run_missing_rom 1 "run|--rom|no-such-rom.bin|--seconds|1"
                "-DERR=^no-such-rom.bin: cannot read a 4096-byte boot ROM/$")
# Real runs (skipped without the user's ROM / disk).
# At 10 s SND-A01 has read 91 sectors (the OS is loading the lower sound; fdc.md layout), so
# the sector regexes below allow 20 to 99.
mirage_cli_test(cli_run_snd_a01_10s 0 "run|--disk|${MIRAGE_TEST_SND_A01}|--seconds|10"
                "-DNEEDS=${MIRAGE_TEST_ROM}|${MIRAGE_TEST_SND_A01}"
                "-DOUT=^display: [0-9A-F][0-9A-F] [0-9A-F][0-9A-F]/illegal: 0/sectors: [2-9][0-9]+/cycles: 200000[0-3][0-9]/$")
mirage_cli_test(cli_run_with_events 0
                "run|--seconds|3|--key|Enter@1:2|--key|enter@2:0.5|--note|60:100@1:0.5|--midi-hex|90 3C 64@2|--key|Cancel@5"
                "-DNEEDS=${MIRAGE_TEST_ROM}"
                "-DOUT=^display: [0-9A-F][0-9A-F] [0-9A-F][0-9A-F]/illegal: 0/sectors: 0/cycles: 60000[0-3][0-9]/$")
# --wav: an unwritable path fails after the run (exit 1); a real render of SND-A01 writes
# the WAV and reports it (22050 Hz x 10 s = 220500 samples).
mirage_cli_test(cli_run_wav_unwritable 1
                "run|--seconds|0.1|--wav|${CMAKE_CURRENT_BINARY_DIR}/no_such_dir/out.wav"
                "-DNEEDS=${MIRAGE_TEST_ROM}" "-DERR=^cannot write .*out.wav/$")
mirage_cli_test(cli_run_wav_snd_a01 0
                "run|--disk|${MIRAGE_TEST_SND_A01}|--seconds|10|--rate|22050|--note|60:100@9:0.5|--wav|${CMAKE_CURRENT_BINARY_DIR}/cli_test.wav"
                "-DNEEDS=${MIRAGE_TEST_ROM}|${MIRAGE_TEST_SND_A01}"
                "-DOUT=^wav: .*cli_test.wav 220500 samples at 22050 Hz, peak [0-9.]+, rms [0-9.]+/display: [0-9A-F][0-9A-F] [0-9A-F][0-9A-F]/illegal: 0/sectors: [2-9][0-9]+/cycles: 200000[0-3][0-9]/$")
# Snapshots: a missing or unusable --snapshot-in file exits 1 (the ROM file is not a snapshot);
# a 40 s run written with --snapshot-out restores with --snapshot-in into a note that renders
# the same WAV every time (tests/cli_snapshot_roundtrip.cmake).
mirage_cli_test(cli_run_snapshot_in_missing 1 "run|--snapshot-in|no-such-file.snap|--seconds|1"
                "-DNEEDS=${MIRAGE_TEST_ROM}" "-DERR=^no-such-file.snap: cannot read the snapshot/$")
mirage_cli_test(cli_run_snapshot_in_garbage 1 "run|--snapshot-in|${MIRAGE_TEST_ROM}|--seconds|1"
                "-DNEEDS=${MIRAGE_TEST_ROM}" "-DERR=mirage.bin: not a usable snapshot ")
mirage_cli_test(cli_run_snapshot_out_unwritable 1
                "run|--seconds|0.1|--snapshot-out|${CMAKE_CURRENT_BINARY_DIR}/no_such_dir/x.snap"
                "-DNEEDS=${MIRAGE_TEST_ROM}" "-DERR=^cannot write .*x.snap/$")
add_test(NAME cli_run_snapshot_round_trip
         COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:mirage_cli> -DDISK=${MIRAGE_TEST_SND_A01}
                 -DDIR=${CMAKE_CURRENT_BINARY_DIR}/snapshot_e2e
                 "-DNEEDS=${MIRAGE_TEST_ROM}|${MIRAGE_TEST_SND_A01}"
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_snapshot_roundtrip.cmake)
set_tests_properties(cli_run_snapshot_round_trip PROPERTIES
                     SKIP_REGULAR_EXPRESSION "SKIP: missing asset")
