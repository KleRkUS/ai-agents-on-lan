add_executable(aio_tests
  test_main.cpp
  test_config.cpp
  test_platform.cpp
  test_tmux.cpp
  test_mailbox.cpp
  test_mcp.cpp
  test_servers.cpp
  test_adapters.cpp
  test_control.cpp
  test_terminal.cpp
  test_relay.cpp
)
target_link_libraries(aio_tests PRIVATE aio_server)
aio_set_warnings(aio_tests)

add_test(NAME aio_tests COMMAND aio_tests)
