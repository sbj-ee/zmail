cmake_minimum_required(VERSION 3.21)

# Single source of truth for the version: bump ONLY the VERSION here.
# configure_file() turns it into generated/version.hpp (window title, About,
# update checks); CPack names the .deb from PROJECT_VERSION.
project(zmail VERSION 0.6.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(ZMAIL_BUILD_TESTS "Build the Qt Test suite" ON)
# CI turns these on; see .github/workflows/ci.yml.
option(ZMAIL_WERROR "Treat compiler warnings in zmail's own code as errors" OFF)
option(ZMAIL_SANITIZE "Build everything with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(ZMAIL_SANITIZE)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "ZMAIL_SANITIZE needs GCC or Clang")
  endif()
  # Global, before any target: the app library, the tests and the mock must
  # all be instrumented. A UBSan report fails the test instead of only printing.
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# Ubuntu 24.04 ships Qt 6.4.2; that is the floor.
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Network Svg Sql Multimedia PrintSupport)
# Refresh tokens live in the OS keyring (Secret Service / KWallet) via QtKeychain.
# Ubuntu: qtkeychain-qt6-dev. The imported target is qt6keychain.
find_package(Qt6Keychain REQUIRED)
# Sending (0.3.0): md4c for Markdown -> HTML, libzip for "zip the attachments",
# and optionally Hunspell for spell check (shares zwriter's user dictionary).
# Ubuntu: libmd4c-dev libmd4c-html0-dev libzip-dev [libhunspell-dev hunspell-en-us]
find_package(PkgConfig REQUIRED)
pkg_check_modules(MD4C_HTML REQUIRED IMPORTED_TARGET md4c-html)
pkg_check_modules(LIBZIP REQUIRED IMPORTED_TARGET libzip)
option(ZMAIL_WITH_HUNSPELL "Spell check in the compose window (Hunspell)" ON)
if(ZMAIL_WITH_HUNSPELL)
  pkg_check_modules(HUNSPELL IMPORTED_TARGET hunspell)
endif()

set(ZMAIL_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/version.hpp.in"
  "${ZMAIL_GENERATED_DIR}/version.hpp"
  @ONLY)

# Everything but main() lives in a static library so the tests can link it.
add_library(zmail_app STATIC
  src/AboutDialog.cpp
  src/AboutDialog.hpp
  src/MainWindow.cpp
  src/MainWindow.hpp
  src/MainWindowDetail.h
  src/MainWindowMailboxes.cpp
  src/MainWindowMessageActions.cpp
  src/MainWindowSession.cpp
  src/UpdateChecker.cpp
  src/UpdateChecker.hpp
  src/core/AddressGuard.cpp
  src/core/AddressGuard.h
  src/core/AuthManager.cpp
  src/core/AuthManager.h
  src/core/ClientConfig.cpp
  src/core/ClientConfig.h
  src/core/ContactStore.cpp
  src/core/ContactStore.h
  src/core/GmailClient.cpp
  src/core/GmailClient.h
  src/core/Limits.h
  src/core/Log.cpp
  src/core/Log.h
  src/core/LoopbackServer.cpp
  src/core/LoopbackServer.h
  src/core/MailCache.cpp
  src/core/MailCache.h
  src/core/MailSession.cpp
  src/core/MailSession.h
  src/core/Mailto.cpp
  src/core/Mailto.h
  src/core/Markdown.cpp
  src/core/Markdown.h
  src/core/MessageParser.cpp
  src/core/MessageParser.h
  src/core/MimeBuilder.cpp
  src/core/MimeBuilder.h
  src/core/Pkce.cpp
  src/core/Rules.cpp
  src/core/Rules.h
  src/core/Pkce.h
  src/core/PeopleClient.cpp
  src/core/PeopleClient.h
  src/core/ReplyBuilder.cpp
  src/core/ReplyBuilder.h
  src/core/RichText.cpp
  src/core/RichText.h
  src/core/SearchQuery.cpp
  src/core/SearchQuery.h
  src/core/Sender.cpp
  src/core/Sender.h
  src/core/Signatures.cpp
  src/core/Signatures.h
  src/core/SnoozeTimes.cpp
  src/core/SnoozeTimes.h
  src/core/SizeFormat.h
  src/core/SpellChecker.cpp
  src/core/SpellChecker.h
  src/core/SyncEngine.cpp
  src/core/SyncEngine.h
  src/core/TokenStore.cpp
  src/core/TokenStore.h
  src/core/Zip.cpp
  src/core/Zip.h
  src/ui/ComposeWindow.cpp
  src/ui/ConnectDialog.cpp
  src/ui/ConnectDialog.h
  src/ui/ContactsWindow.cpp
  src/ui/ContactsWindow.h
  src/ui/ComposeWindow.h
  src/ui/Icons.cpp
  src/ui/Icons.h
  src/ui/MessageListModel.cpp
  src/ui/MessageListModel.h
  src/ui/SelectionAfterRemoval.h
  src/ui/NewMailSound.cpp
  src/ui/NewMailSound.h
  src/ui/HtmlFit.cpp
  src/ui/HtmlFit.h
  src/ui/MessageView.cpp
  src/ui/MessageView.h
  src/ui/MessageWindow.cpp
  src/ui/MessageWindow.h
  src/ui/SafeHtmlView.cpp
  src/ui/SafeHtmlView.h
  src/core/HtmlSanitizer.cpp
  src/core/HtmlSanitizer.h
  src/ui/SafeTextEdit.cpp
  src/ui/SafeTextEdit.h
  src/ui/PrivacyDialog.cpp
  src/ui/PrivacyDialog.h
  src/ui/SoundDialog.cpp
  src/ui/SoundDialog.h
  src/ui/RemoteImages.cpp
  src/ui/RemoteImages.h
  src/ui/SignaturesDialog.cpp
  src/ui/SignaturesDialog.h
  src/ui/SpellHighlighter.cpp
  src/ui/SpellHighlighter.h
  src/ui/RulesDialog.cpp
  src/ui/RulesDialog.h
  src/ui/ListDialog.cpp
  src/ui/ListDialog.h
  src/ui/StripesDialog.cpp
  src/ui/StripesDialog.h
  src/ui/BrandThemes.h
  src/ui/Theme.cpp
  src/ui/Theme.h
  src/ui/ThemeEditorDialog.cpp
  src/ui/ThemeEditorDialog.h
  src/ui/ThemeFile.cpp
  src/ui/ThemeFile.h
)

# Bundled Lucide icons (ISC/MIT; see assets/icons/LICENSES) as :/icons/lucide/*.svg
file(GLOB ZMAIL_ICON_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/lucide/*.svg")
qt_add_resources(zmail_app "zmail_icons"
  PREFIX "/"
  BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets"
  FILES ${ZMAIL_ICON_FILES})
# zmail's own app icon (original artwork, see assets/icons/LICENSES) as
# :/icons/zmail-<N>.png for QApplication::setWindowIcon (ui::appIcon()).
set(ZMAIL_APP_ICON_FILES)
foreach(_sz 16 24 32 48 64 128 256)
  list(APPEND ZMAIL_APP_ICON_FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/zmail-${_sz}.png")
endforeach()
qt_add_resources(zmail_app "zmail_appicon"
  PREFIX "/"
  BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets"
  FILES ${ZMAIL_APP_ICON_FILES})
# Bundled CC0 new-mail chime (see assets/sounds/LICENSES) as :/sounds/new-mail.wav
qt_add_resources(zmail_app "zmail_sounds"
  PREFIX "/"
  BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets"
  FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/sounds/new-mail.wav")
target_include_directories(zmail_app PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${ZMAIL_GENERATED_DIR}")
target_link_libraries(zmail_app PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Svg Qt6::Sql Qt6::Multimedia Qt6::PrintSupport
  qt6keychain PkgConfig::MD4C_HTML PkgConfig::LIBZIP)
if(HUNSPELL_FOUND)
  target_link_libraries(zmail_app PUBLIC PkgConfig::HUNSPELL)
  target_compile_definitions(zmail_app PUBLIC ZMAIL_HAVE_HUNSPELL)
  message(STATUS "zmail: spell check enabled (hunspell ${HUNSPELL_VERSION})")
else()
  message(STATUS "zmail: spell check disabled (hunspell not found)")
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(zmail_app PRIVATE -Wall -Wextra -Wpedantic)
  if(ZMAIL_WERROR)
    target_compile_options(zmail_app PRIVATE -Werror)
  endif()
endif()

add_executable(zmail src/main.cpp)
target_link_libraries(zmail PRIVATE zmail_app)

if(ZMAIL_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

include(cmake/Packaging.cmake)
