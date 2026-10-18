# Runs mirage_cli once and checks its exact exit code and output.
#   cmake -DCLI=<exe> -DARGS=<args separated by |> -DRC=<exit code>
#         [-DOUT=<regex>] [-DERR=<regex>] [-DNEEDS=<files separated by |>]
#         -P cli_check.cmake
# Line breaks in stdout/stderr are turned into "/" before matching, so a regex
# can span lines ("^display: 00 00/illegal: 0/"). If NEEDS names a missing file
# (a user-owned asset) the test prints "SKIP: missing asset" and is skipped
# through the SKIP_REGULAR_EXPRESSION test property.
string(REPLACE "|" ";" needs "${NEEDS}")
foreach(f IN LISTS needs)
  if(NOT EXISTS "${f}")
    message("SKIP: missing asset ${f}")
    return()
  endif()
endforeach()
string(REPLACE "|" ";" args "${ARGS}")
execute_process(COMMAND "${CLI}" ${args}
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
foreach(stream out err)
  string(REPLACE "\r" "" ${stream} "${${stream}}")
  string(REPLACE "\n" "/" ${stream} "${${stream}}")
endforeach()
set(report "exit code: ${rc}\nstdout: ${out}\nstderr: ${err}")
if(NOT rc STREQUAL RC)
  message(FATAL_ERROR "expected exit code ${RC}\n${report}")
endif()
if(DEFINED OUT AND NOT out MATCHES "${OUT}")
  message(FATAL_ERROR "stdout does not match: ${OUT}\n${report}")
endif()
if(DEFINED ERR AND NOT err MATCHES "${ERR}")
  message(FATAL_ERROR "stderr does not match: ${ERR}\n${report}")
endif()
message("ok: ${report}")
