cmake_minimum_required(VERSION 3.21)

# Single source of truth for the version: bump ONLY the VERSION here.
# configure_file() turns it into generated/version.hpp (window title, About,
# update checks); CPack names the .deb from PROJECT_VERSION.
project(zmail VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(ZMAIL_BUILD_TESTS "Build the Qt Test suite" ON)

# Ubuntu 24.04 ships Qt 6.4.2; that is the floor.
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Network Svg)

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
  src/UpdateChecker.cpp
  src/UpdateChecker.hpp
  src/core/Limits.h
  src/ui/ComposeWindow.cpp
  src/ui/ComposeWindow.h
  src/ui/Icons.cpp
  src/ui/Icons.h
  src/ui/MessageListModel.cpp
  src/ui/MessageListModel.h
  src/ui/Theme.cpp
  src/ui/Theme.h
)

# Bundled Lucide icons (ISC/MIT; see assets/icons/LICENSES) as :/icons/lucide/*.svg
file(GLOB ZMAIL_ICON_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/lucide/*.svg")
qt_add_resources(zmail_app "zmail_icons"
  PREFIX "/"
  BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets"
  FILES ${ZMAIL_ICON_FILES})
target_include_directories(zmail_app PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${ZMAIL_GENERATED_DIR}")
target_link_libraries(zmail_app PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Svg)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(zmail_app PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(zmail src/main.cpp)
target_link_libraries(zmail PRIVATE zmail_app)

if(ZMAIL_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

include(cmake/Packaging.cmake)
