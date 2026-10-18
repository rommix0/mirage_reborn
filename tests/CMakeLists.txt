include(Catch)

add_executable(mirage_tests
  test_state_io.cpp
  test_disk_image.cpp
  test_cpu6809.cpp
  test_via6522.cpp
  test_panel.cpp
  test_acia6850.cpp
  test_wd1772.cpp
  test_machine_map.cpp
  test_disasm6809.cpp
  test_boot.cpp
  test_cli_run.cpp
)
target_link_libraries(mirage_tests PRIVATE mirage_engine mirage_cli_core Catch2::Catch2WithMain)
target_compile_definitions(mirage_tests PRIVATE
  MIRAGE_ASSET_ROOT="${MIRAGE_ASSET_ROOT}")
catch_discover_tests(mirage_tests)
