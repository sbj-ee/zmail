find_package(Qt6 REQUIRED COMPONENTS Test)

# In-process mock of Google's OAuth + Gmail endpoints (tests and screenshots
# only; never linked into the app).
add_library(zmail_mock STATIC mock/MockGoogle.cpp mock/MockGoogle.h)
target_include_directories(zmail_mock PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(zmail_mock PUBLIC zmail_app)

function(zmail_add_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE zmail_app zmail_mock Qt6::Test)
  add_test(NAME ${name} COMMAND ${name})
  # Headless: no display needed in CI. Never touch the real keyring/config.
  set_tests_properties(${name} PROPERTIES ENVIRONMENT
    "QT_QPA_PLATFORM=offscreen;ZMAIL_OAUTH_CLIENT=${CMAKE_CURRENT_BINARY_DIR}/no-such-client.json;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/xdg-config")
endfunction()

zmail_add_test(tst_version)
zmail_add_test(tst_mainwindow)
zmail_add_test(tst_limits)
zmail_add_test(tst_compose)
zmail_add_test(tst_theme)
zmail_add_test(tst_auth)
zmail_add_test(tst_sync)
zmail_add_test(tst_connect)
zmail_add_test(tst_send)
zmail_add_test(tst_viewer)
target_compile_definitions(tst_viewer PRIVATE ZMAIL_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

# Offline screenshot driver: zmail against the mock server.
add_executable(zmail_mockshot mockshot.cpp)
target_link_libraries(zmail_mockshot PRIVATE zmail_app zmail_mock)
