find_package(Qt6 REQUIRED COMPONENTS Test)

function(zmail_add_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE zmail_app Qt6::Test)
  add_test(NAME ${name} COMMAND ${name})
  # Headless: no display needed in CI.
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

zmail_add_test(tst_version)
zmail_add_test(tst_mainwindow)
zmail_add_test(tst_limits)
zmail_add_test(tst_compose)
zmail_add_test(tst_theme)
