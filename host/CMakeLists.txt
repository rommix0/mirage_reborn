# JUCE-free host-side support: queues, MIDI parsing, panel layout, ROM identity,
# and RealtimeHost (the Machine on the audio thread).
# Linked by the unit tests and by the JUCE app.
add_library(mirage_host STATIC
  src/midi_byte_parser.cpp
  src/panel_layout.cpp
  src/boot_rom.cpp
  src/realtime_host.cpp
)
target_include_directories(mirage_host PUBLIC include)
target_link_libraries(mirage_host PUBLIC mirage_engine)
