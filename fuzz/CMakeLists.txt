# libFuzzer harnesses for the parsers that read untrusted input (clang only).
# Configure with -DZMAIL_FUZZ=ON -DZMAIL_SANITIZE=ON and a clang compiler; see
# fuzz/README.md. Each fuzzer is also a ctest that replays its seed corpus
# (and any crash regressions) once, so a fixed crash stays fixed.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Sql)

function(zmail_add_fuzzer name)
  add_executable(${name} ${name}.cpp FuzzCommon.h)
  target_link_libraries(${name} PRIVATE zmail_app Qt6::Core Qt6::Gui Qt6::Sql)
  target_include_directories(${name} PRIVATE "${CMAKE_SOURCE_DIR}/src")
  target_link_options(${name} PRIVATE -fsanitize=fuzzer)
  set(corpus "${CMAKE_CURRENT_SOURCE_DIR}/corpus/${ARGV1}")
  add_test(NAME ${name}_corpus COMMAND ${name} -runs=0 "${corpus}")
  set_tests_properties(${name}_corpus PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

zmail_add_fuzzer(fuzz_rawmime rawmime)
zmail_add_fuzzer(fuzz_messageparser messageparser)
zmail_add_fuzzer(fuzz_htmlsanitizer htmlsanitizer)
zmail_add_fuzzer(fuzz_calendarinvite calendarinvite)
zmail_add_fuzzer(fuzz_searchquery searchquery)
