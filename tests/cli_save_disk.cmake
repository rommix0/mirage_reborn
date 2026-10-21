# mirage_cli --save-disk, end to end: run a disk for 1 s emulated, write the disk
# with --save-disk and require a raw image of exactly 450,560 bytes.
#   cmake -DCLI=<exe> -DDISK=<img> -DDIR=<scratch dir> [-DNEEDS=<files separated by |>]
#         -P cli_save_disk.cmake
# Like cli_check.cmake, a missing asset prints "SKIP: missing asset" and returns.
string(REPLACE "|" ";" needs "${NEEDS}")
foreach(f IN LISTS needs)
  if(NOT EXISTS "${f}")
    message("SKIP: missing asset ${f}")
    return()
  endif()
endforeach()
file(MAKE_DIRECTORY "${DIR}")
set(out "${DIR}/saved.img")
file(REMOVE "${out}")
execute_process(COMMAND "${CLI}" run --disk "${DISK}" --seconds 1 --save-disk "${out}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE stdout ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "mirage_cli run --save-disk exited ${rc}\nstdout: ${stdout}\nstderr: ${err}")
endif()
if(NOT EXISTS "${out}")
  message(FATAL_ERROR "--save-disk wrote no file\nstdout: ${stdout}")
endif()
file(SIZE "${out}" size)
if(NOT size EQUAL 450560)
  message(FATAL_ERROR "--save-disk wrote ${size} bytes, expected 450560")
endif()
# The disk was only read for 1 s, so the image is the original, byte for byte.
file(SHA1 "${out}" saved_sha)
file(SHA1 "${DISK}" disk_sha)
if(NOT saved_sha STREQUAL disk_sha)
  message(FATAL_ERROR "an unmodified disk was saved with different contents")
endif()
message("ok: ${stdout}")
