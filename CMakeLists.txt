cmake_minimum_required(VERSION 3.15)

# Manual version. Change this value for releases.
set(BOOKR_MANUAL_VERSION "0.1.0" CACHE STRING "Manual Bookr version")
project(bookr VERSION "${BOOKR_MANUAL_VERSION}" LANGUAGES CXX)

# -----------------------------------------
# Build configuration
# -----------------------------------------

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(BOOKR_ENABLE_WARNINGS "Enable stricter compiler warnings" ON)
option(BOOKR_ENABLE_SANITIZERS "Enable ASan/UBSan in Debug (GCC/Clang)" OFF)

add_executable(bookr)
target_sources(bookr PRIVATE src/main.cpp)
target_compile_features(bookr PRIVATE cxx_std_17)
target_compile_definitions(bookr PRIVATE BOOKR_VERSION_STR=\"${PROJECT_VERSION}\")

if(BOOKR_ENABLE_WARNINGS)
  if(MSVC)
    target_compile_options(bookr PRIVATE /W4 /permissive-)
  else()
    target_compile_options(bookr PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion
      -Wshadow
    )
  endif()
endif()

if(BOOKR_ENABLE_SANITIZERS
   AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU"
   AND CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_compile_options(bookr PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(bookr PRIVATE -fsanitize=address,undefined)
endif()

include(GNUInstallDirs)
install(TARGETS bookr RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES HELP.txt DESTINATION ${CMAKE_INSTALL_DOCDIR})
