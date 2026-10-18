add_library(mirage_engine STATIC
  src/state_io.cpp
  src/disk_image.cpp
  src/opcodes6809.cpp
  src/cpu6809.cpp
  src/cpu6809_ops.cpp
)
target_include_directories(mirage_engine PUBLIC include)
