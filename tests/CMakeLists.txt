include(Catch)

add_executable(mirage_tests
  non_interactive.cpp
  test_state_io.cpp
  test_disk_image.cpp
  test_cpu6809.cpp
  test_via6522.cpp
  test_panel.cpp
  test_acia6850.cpp
  test_wd1772.cpp
  test_doc5503.cpp
  test_voice_filter.cpp
  test_machine_map.cpp
  test_disasm6809.cpp
  test_boot.cpp
  test_audio.cpp
  test_snapshot.cpp
  test_sha1.cpp
  test_cli_run.cpp
  test_realtime.cpp
  test_spsc_queue.cpp
  test_midi_byte_parser.cpp
  test_panel_layout.cpp
  test_realtime_host.cpp
  test_disk_session.cpp
  test_acceptance.cpp
  alloc_counter.cpp
)
target_link_libraries(mirage_tests PRIVATE mirage_engine mirage_host mirage_cli_core Catch2::Catch2WithMain)
target_include_directories(mirage_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../engine/src)
target_compile_definitions(mirage_tests PRIVATE
  MIRAGE_ASSET_ROOT="${MIRAGE_ASSET_ROOT}")
catch_discover_tests(mirage_tests)
