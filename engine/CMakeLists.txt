add_library(mirage_engine STATIC
  src/state_io.cpp
  src/disk_image.cpp
)
target_include_directories(mirage_engine PUBLIC include)
