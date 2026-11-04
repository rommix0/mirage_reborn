# ctest smoke test for Mirage.vst3: runs plugin_host_smoke, which loads the
# plugin through JUCE's VST3 hosting. Skipped when the user's ROM or SND-A01 is
# missing (they are never copied into the repo).
#   cmake -DTOOL=<plugin_host_smoke> -DPLUGIN_DLL=<Mirage.vst3/Contents/x86_64-win/Mirage.vst3>
#         -DROM=<mirage.bin> -DDISK=<SND-A01.img> [-DEDITOR_PNG=<out.png>] -P plugin_smoke.cmake
# The bundle (the Mirage.vst3 folder) holds the DLL in Contents/x86_64-win.
# With EDITOR_PNG the tool captures the editor instead, and the file must
# start with the PNG signature. With -DMIDI_OUT=ON it runs the MIDI-out check
# on a synthetic ROM instead, which needs neither ROM nor DISK.
get_filename_component(plugin "${PLUGIN_DLL}/../../.." ABSOLUTE)
if(MIDI_OUT)
  execute_process(COMMAND "${TOOL}" "${plugin}" --midi-out
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 600)
  message("${out}${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "plugin_host_smoke --midi-out exited with '${rc}'")
  endif()
  return()
endif()
foreach(f IN ITEMS "${ROM}" "${DISK}")
  if(NOT EXISTS "${f}")
    message("SKIP: missing asset ${f}")
    return()
  endif()
endforeach()
set(extra)
if(EDITOR_PNG)
  file(REMOVE "${EDITOR_PNG}")
  set(extra --editor-png "${EDITOR_PNG}")
endif()
execute_process(COMMAND "${TOOL}" "${plugin}" "${ROM}" "${DISK}" ${extra}
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 600)
message("${out}${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "plugin_host_smoke exited with '${rc}'")
endif()
if(EDITOR_PNG)
  file(READ "${EDITOR_PNG}" signature LIMIT 8 HEX)
  if(NOT signature STREQUAL "89504e470d0a1a0a")
    message(FATAL_ERROR "${EDITOR_PNG} does not start with the PNG signature (got ${signature})")
  endif()
endif()
