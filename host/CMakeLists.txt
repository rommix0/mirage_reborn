# JUCE-free host-side support: queues, MIDI parsing, panel layout, ROM identity,
# RealtimeHost (the Machine on the audio thread) and DiskSession (disk files).
# Linked by the unit tests and by the JUCE app.
add_library(mirage_host STATIC
  src/midi_byte_parser.cpp
  src/midi_in_feeder.cpp
  src/panel_layout.cpp
  src/boot_rom.cpp
  src/realtime_host.cpp
  src/disk_session.cpp
)
target_include_directories(mirage_host PUBLIC include)
target_link_libraries(mirage_host PUBLIC mirage_engine)
