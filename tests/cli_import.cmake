# mirage_cli --import, end to end: a first run (no disk, 0.5 s) writes a 22,050 Hz
# WAV; a second run imports it at 3 s and must print where the bytes went; a third
# run, with SND-A01 loading from the drive at 2 s, must refuse the import.
#   cmake -DCLI=<exe> -DDISK=<img> -DDIR=<scratch dir> [-DNEEDS=<files separated by |>]
#         -P cli_import.cmake
# Like cli_check.cmake, a missing asset prints "SKIP: missing asset" and returns.
string(REPLACE "|" ";" needs "${NEEDS}")
foreach(f IN LISTS needs)
  if(NOT EXISTS "${f}")
    message("SKIP: missing asset ${f}")
    return()
  endif()
endforeach()
file(MAKE_DIRECTORY "${DIR}")
set(wav "${DIR}/source.wav")
file(REMOVE "${wav}")

function(run_cli expect_rc out_var)
  execute_process(COMMAND "${CLI}" run ${ARGN}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  string(REPLACE "\r" "" out "${out}")
  string(REPLACE "\r" "" err "${err}")
  if(NOT rc EQUAL expect_rc)
    message(FATAL_ERROR "mirage_cli run ${ARGN} exited ${rc}, expected ${expect_rc}\nstdout: ${out}\nstderr: ${err}")
  endif()
  set(${out_var} "${out}${err}" PARENT_SCOPE)
endfunction()

run_cli(0 made --seconds 0.5 --rate 22050 --wav "${wav}")
if(NOT EXISTS "${wav}")
  message(FATAL_ERROR "the first run wrote no WAV\n${made}")
endif()

# Without a disk the boot ROM keeps the motor on for the first 2 s or so, so the
# imports come later. 11,025 samples at 22,050 Hz = 0.5 s = 14,706 bytes at the default 29,412 Hz
# (last page 57); 0.1 s to 0.3 s at 20,000 Hz = 4,000 bytes from page 2 of the
# upper half ($10200), last page 2 + 15 = 17.
run_cli(0 imported --seconds 5 --import "${wav}@3"
        --import "${wav}@4,rate=20000,start=0.1,end=0.3,half=upper,page=2,normalize=0")
if(NOT imported MATCHES "import: [^\n]*source.wav 14706 bytes at [$]00000, last page 57\n")
  message(FATAL_ERROR "unexpected first import line:\n${imported}")
endif()
if(NOT imported MATCHES "import: [^\n]*source.wav 4000 bytes at [$]10200, last page 17\n")
  message(FATAL_ERROR "unexpected second import line:\n${imported}")
endif()

# A bad rate stops before the run: exit 1, the message on stderr, nothing run.
run_cli(1 refused --import "${wav}@3,rate=5000")
if(NOT refused MATCHES "source.wav: The sample rate must be between 10101 and 33333 Hz.")
  message(FATAL_ERROR "unexpected message for a bad rate:\n${refused}")
endif()

# SND-A01 is loading its sound from disk at 2 s (the motor is on).
run_cli(0 busy --disk "${DISK}" --seconds 3 --import "${wav}@2")
if(NOT busy MATCHES "import: [^\n]*source.wav refused, the drive is busy\n")
  message(FATAL_ERROR "the import was not refused while the drive was busy:\n${busy}")
endif()
message("ok: ${imported}${busy}")
