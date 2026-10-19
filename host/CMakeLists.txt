# JUCE-free host-side support: queues, MIDI parsing, panel layout, ROM identity.
# Linked by the unit tests and by the JUCE app.
add_library(mirage_host STATIC
  src/midi_byte_parser.cpp
  src/panel_layout.cpp
  src/boot_rom.cpp
)
target_include_directories(mirage_host PUBLIC include)
target_link_libraries(mirage_host PUBLIC mirage_engine)
