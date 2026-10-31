cmake_minimum_required(VERSION 3.10)
project(nds-rom-reader C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(ndsrom src/ndsrom.c src/ndscomp.c src/ndsgfx.c src/ndspng.c)
target_include_directories(ndsrom PUBLIC include)
if(NOT MSVC)
  target_compile_options(ndsrom PRIVATE -Wall -Wextra)
endif()

add_executable(ndsviewer viewer/main.c)
target_link_libraries(ndsviewer PRIVATE ndsrom)
if(NOT MSVC)
  target_compile_options(ndsviewer PRIVATE -Wall -Wextra)
endif()

enable_testing()
add_executable(test_gfx tests/test_gfx.c)
target_link_libraries(test_gfx PRIVATE ndsrom)
add_test(NAME gfx COMMAND test_gfx)
