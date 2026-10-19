# The JUCE standalone app. Only this directory includes JUCE; the engine and
# mirage_host stay JUCE-free.

# The top-level /W4 /permissive- /utf-8 must not apply to JUCE's sources (JUCE
# modules compile as part of the app target). The app gets JUCE's recommended
# warning flags below instead; engine, host and tests keep the strict flags.
set_directory_properties(PROPERTIES COMPILE_OPTIONS "")

include(FetchContent)
FetchContent_Declare(JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 8.0.4
  GIT_SHALLOW TRUE
  GIT_CONFIG core.longpaths=true)
FetchContent_MakeAvailable(JUCE)

juce_add_gui_app(MirageApp
  PRODUCT_NAME "Mirage"
  VERSION 0.2.0
  COMPANY_NAME "Mirage Emulator")

target_sources(MirageApp PRIVATE
  src/Main.cpp
  src/MainWindow.cpp
  src/AudioEngine.cpp
  src/Settings.cpp)

target_compile_definitions(MirageApp PRIVATE
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:MirageApp,JUCE_PRODUCT_NAME>"
  JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:MirageApp,JUCE_VERSION>"
  MIRAGE_ASSET_ROOT="${MIRAGE_ASSET_ROOT}")

target_link_libraries(MirageApp
  PRIVATE
    mirage_host
    juce::juce_gui_extra
    juce::juce_audio_utils
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
