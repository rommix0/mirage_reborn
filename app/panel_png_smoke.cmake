# ctest smoke test for `Mirage --panel-png <file>`: runs the app (no audio
# device or ROM needed), expects exit code 0 and a PNG on disk.
#   cmake -DAPP=<Mirage.exe> -DPNG=<output file> -P panel_png_smoke.cmake
file(REMOVE "${PNG}")
execute_process(COMMAND "${APP}" --panel-png "${PNG}" RESULT_VARIABLE rc TIMEOUT 60)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "--panel-png exited with '${rc}'")
endif()
if(NOT EXISTS "${PNG}")
  message(FATAL_ERROR "--panel-png wrote no file at ${PNG}")
endif()
file(READ "${PNG}" signature LIMIT 8 HEX)
if(NOT signature STREQUAL "89504e470d0a1a0a")
  message(FATAL_ERROR "${PNG} does not start with the PNG signature (got ${signature})")
endif()
