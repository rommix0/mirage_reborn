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
add_subdirectory(tests)
add_executable(mirage_cli tools/mirage_cli/main.cpp)
target_link_libraries(mirage_cli PRIVATE mirage_engine)
target_compile_definitions(mirage_cli PRIVATE MIRAGE_ASSET_ROOT="${MIRAGE_ASSET_ROOT}")

# mirage_cli argument handling (no assets needed: both fail before loading the ROM).
add_test(NAME cli_run_rejects_unknown_option COMMAND mirage_cli run --no-such-option 1)
add_test(NAME cli_run_wav_not_available_yet COMMAND mirage_cli run --wav out.wav --rate 48000)
set_tests_properties(cli_run_rejects_unknown_option cli_run_wav_not_available_yet
                     PROPERTIES WILL_FAIL TRUE)
