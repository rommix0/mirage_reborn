add_library(mirage_engine STATIC
  src/state_io.cpp
  src/disk_image.cpp
  src/opcodes6809.cpp
  src/cpu6809.cpp
  src/cpu6809_ops.cpp
  src/via6522.cpp
  src/panel.cpp
  src/acia6850.cpp
  src/wd1772.cpp
  src/doc5503.cpp
  src/voice_filter.cpp
  src/stream_resampler.cpp
  src/wav_writer.cpp
  src/sha1.cpp
  src/machine.cpp
  src/disasm6809.cpp
)
target_include_directories(mirage_engine PUBLIC include)
