# ctest smoke test for Mirage.vst3: runs plugin_host_smoke, which loads the
# plugin through JUCE's VST3 hosting. Skipped when the user's ROM or SND-A01 is
# missing (they are never copied into the repo).
#   cmake -DTOOL=<plugin_host_smoke> -DPLUGIN_DLL=<Mirage.vst3/Contents/x86_64-win/Mirage.vst3>
#         -DROM=<mirage.bin> -DDISK=<SND-A01.img> -P plugin_smoke.cmake
# The bundle (the Mirage.vst3 folder) holds the DLL in Contents/x86_64-win.
get_filename_component(plugin "${PLUGIN_DLL}/../../.." ABSOLUTE)
foreach(f IN ITEMS "${ROM}" "${DISK}")
  if(NOT EXISTS "${f}")
    message("SKIP: missing asset ${f}")
    return()
  endif()
endforeach()
execute_process(COMMAND "${TOOL}" "${plugin}" "${ROM}" "${DISK}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 600)
message("${out}${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "plugin_host_smoke exited with '${rc}'")
endif()
