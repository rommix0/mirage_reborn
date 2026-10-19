# mirage_cli snapshot round trip, end to end:
#   1. run a disk for 40 s emulated and write --snapshot-out;
#   2. restore it with --snapshot-in, play a note and write a WAV, twice, and
#      require the two WAVs to be byte-identical (determinism through the CLI).
#   cmake -DCLI=<exe> -DDISK=<img> -DDIR=<scratch dir> [-DNEEDS=<files separated by |>]
#         -P cli_snapshot_roundtrip.cmake
# Like cli_check.cmake, a missing asset prints "SKIP: missing asset" and returns.
string(REPLACE "|" ";" needs "${NEEDS}")
foreach(f IN LISTS needs)
  if(NOT EXISTS "${f}")
    message("SKIP: missing asset ${f}")
    return()
  endif()
endforeach()
file(MAKE_DIRECTORY "${DIR}")
set(snap "${DIR}/roundtrip.snap")
file(REMOVE "${snap}" "${DIR}/rt_a.wav" "${DIR}/rt_b.wav")

function(run_cli expect_rc out_regex)
  execute_process(COMMAND "${CLI}" run ${ARGN}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  string(REPLACE "\r" "" out "${out}")
  string(REPLACE "\n" "/" out "${out}")
  if(NOT rc STREQUAL expect_rc OR NOT out MATCHES "${out_regex}")
    message(FATAL_ERROR "mirage_cli run ${ARGN}\nexit code: ${rc} (expected ${expect_rc})\n"
                        "stdout: ${out}\nstderr: ${err}\nexpected stdout to match: ${out_regex}")
  endif()
  message("ok: ${out}")
endfunction()

run_cli(0 "^snapshot: .*roundtrip.snap [0-9]+ bytes/display: .*/cycles: 800000[0-3][0-9]/$"
        --disk "${DISK}" --seconds 40 --snapshot-out "${snap}")
if(NOT EXISTS "${snap}")
  message(FATAL_ERROR "--snapshot-out wrote no file")
endif()
# 2 s more on top of the 40 s: the cycle count continues from the loaded state,
# and the note makes an audible WAV (peak at least 0.1).
foreach(w a b)
  run_cli(0 "^wav: .*rt_${w}.wav 44100 samples at 22050 Hz, peak 0[.][1-9][0-9]*, rms [0-9.]+/display: .*/illegal: 0/sectors: [0-9]+/cycles: 840000[0-3][0-9]/$"
          --snapshot-in "${snap}" --seconds 2 --rate 22050 --note 60:100@0.2:1
          --wav "${DIR}/rt_${w}.wav")
endforeach()
file(SHA1 "${DIR}/rt_a.wav" sha_a)
file(SHA1 "${DIR}/rt_b.wav" sha_b)
if(NOT sha_a STREQUAL sha_b)
  message(FATAL_ERROR "two restores of one snapshot rendered different WAVs")
endif()
