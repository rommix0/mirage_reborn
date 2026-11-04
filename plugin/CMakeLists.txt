# The VST3 plugin (Mirage.vst3) and its smoke test. JUCE is fetched in app/, so
# this directory is added after it. Like app/, it includes JUCE; the engine and
# mirage_host stay JUCE-free.

# JUCE's sources build as part of these targets: no /W4 /permissive- (see app/).
set_directory_properties(PROPERTIES COMPILE_OPTIONS "")

juce_add_plugin(MiragePlugin
  PRODUCT_NAME "Mirage"
  COMPANY_NAME "Mirage Emulator"
  FORMATS VST3
  VERSION 0.3.0
  PLUGIN_MANUFACTURER_CODE MrEm
  PLUGIN_CODE Mrg8
  IS_SYNTH TRUE
  NEEDS_MIDI_INPUT TRUE
  NEEDS_MIDI_OUTPUT TRUE
  VST3_CATEGORIES Instrument Sampler
  COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(MiragePlugin PRIVATE
  src/PluginProcessor.cpp
  src/PluginEditor.cpp
  src/PluginState.cpp
  src/PluginSettings.cpp
  # The app's UI, shared as is (the panel artwork is finished).
  ../app/src/PanelComponent.cpp
  ../app/src/ImportDialog.cpp
  ../app/src/InputMeter.cpp
  ../app/src/DiskActions.cpp)
target_include_directories(MiragePlugin PRIVATE ../app/src)

target_compile_definitions(MiragePlugin PUBLIC
  JUCE_VST3_CAN_REPLACE_VST2=0
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  MIRAGE_ROM="${MIRAGE_ROM}")

target_link_libraries(MiragePlugin
  PRIVATE
    MirageBranding
    mirage_host
    juce::juce_audio_utils
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# Loads the built Mirage.vst3 through JUCE's own VST3 hosting (plugin_smoke.cmake).
juce_add_console_app(plugin_host_smoke VERSION 0.3.0)
target_sources(plugin_host_smoke PRIVATE tools/plugin_host_smoke.cpp src/PluginState.cpp)
target_include_directories(plugin_host_smoke PRIVATE src ${PROJECT_SOURCE_DIR}/tools/mirage_cli)
target_compile_definitions(plugin_host_smoke PRIVATE
  JUCE_PLUGINHOST_VST3=1
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0)
target_link_libraries(plugin_host_smoke
  PRIVATE
    mirage_engine
    juce::juce_audio_processors
    juce::juce_audio_utils
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
add_dependencies(plugin_host_smoke MiragePlugin_VST3)

add_test(NAME plugin_vst3_smoke
         COMMAND ${CMAKE_COMMAND} -DTOOL=$<TARGET_FILE:plugin_host_smoke>
                 -DPLUGIN_DLL=$<TARGET_FILE:MiragePlugin_VST3>
                 "-DROM=${MIRAGE_ROM}"
                 "-DDISK=${MIRAGE_ASSET_ROOT}/Factory Library/IMG/SND-A01.img"
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/plugin_smoke.cmake)
set_tests_properties(plugin_vst3_smoke PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP: missing asset")

# The editor of a restored SND-A01, captured at 1200 px wide.
add_test(NAME plugin_editor_png_smoke
         COMMAND ${CMAKE_COMMAND} -DTOOL=$<TARGET_FILE:plugin_host_smoke>
                 -DPLUGIN_DLL=$<TARGET_FILE:MiragePlugin_VST3>
                 "-DROM=${MIRAGE_ROM}"
                 "-DDISK=${MIRAGE_ASSET_ROOT}/Factory Library/IMG/SND-A01.img"
                 -DEDITOR_PNG=${CMAKE_BINARY_DIR}/plugin_editor_smoke.png
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/plugin_smoke.cmake)
set_tests_properties(plugin_editor_png_smoke PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP: missing asset")

# MIDI out from a synthetic ROM, in a run of its own (the plugin caches its ROM
# per process): whole messages, in order, at their frames.
add_test(NAME plugin_midi_out_smoke
         COMMAND ${CMAKE_COMMAND} -DTOOL=$<TARGET_FILE:plugin_host_smoke>
                 -DPLUGIN_DLL=$<TARGET_FILE:MiragePlugin_VST3> -DMIDI_OUT=ON
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/plugin_smoke.cmake)
