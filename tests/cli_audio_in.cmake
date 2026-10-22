# mirage_cli --audio-in, end to end, with no disk: a first run writes a 1 s, 22,050 Hz
# WAV; later runs play it into the jack and must print how many frames they fed.
#   cmake -DCLI=<exe> -DDIR=<scratch dir> [-DNEEDS=<files separated by |>] -P cli_audio_in.cmake
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

run_cli(0 made --seconds 1 --rate 22050 --wav "${wav}")
if(NOT EXISTS "${wav}")
  message(FATAL_ERROR "the first run wrote no WAV\n${made}")
endif()

# 0.2 s to 1 s at 22,050 Hz = 17,640 frames of the file's 22,050; no --wav, so the output is discarded.
run_cli(0 played --seconds 1 --rate 22050 --audio-in "${wav}@0.2")
if(NOT played MATCHES "audio-in: [^\n]*source.wav 17640 frames played\n")
  message(FATAL_ERROR "unexpected frame count:\n${played}")
endif()
if(played MATCHES "wav:")
  message(FATAL_ERROR "a WAV was written without --wav:\n${played}")
endif()

# At 44,100 Hz the file is resampled to 44,100 frames; asked for 1.5 s (66,150), it has 44,100.
run_cli(0 resampled --seconds 2 --rate 44100 --audio-in "${wav}@0.5")
if(NOT resampled MATCHES "audio-in: [^\n]*source.wav 44100 frames played\n")
  message(FATAL_ERROR "unexpected resampled frame count:\n${resampled}")
endif()

# loop=1 repeats it; a later entry replaces the first from its time: 0 to 0.5 s = 11,025 frames
# of the first, then the second plays the rest (0.5 s to 1.5 s = 22,050 frames), with --wav.
run_cli(0 looped --seconds 1.5 --rate 22050 --wav "${DIR}/out.wav"
        --audio-in "${wav}@0,gain=0.5,loop=1" --audio-in "${wav}@0.5,loop=1")
if(NOT looped MATCHES "audio-in: [^\n]*source.wav 11025 frames played\naudio-in: [^\n]*source.wav 22050 frames played\n")
  message(FATAL_ERROR "unexpected looped frame counts:\n${looped}")
endif()
if(NOT looped MATCHES "wav: [^\n]*out.wav 33075 samples at 22050 Hz")
  message(FATAL_ERROR "unexpected wav line:\n${looped}")
endif()
message("ok: ${played}${resampled}${looped}")
