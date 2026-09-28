cmake_minimum_required(VERSION 3.20)

project(wdx_fonttrace LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "wdx_fonttrace is a Windows Total Commander plugin")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

function(fonttrace_configure_target target)
  target_compile_definitions(${target} PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
  )
  target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc /utf-8)
endfunction()

add_library(fonttrace MODULE
  native/plugin.cpp
  native/sfnt_reader.cpp
)

target_include_directories(fonttrace PRIVATE native)
fonttrace_configure_target(fonttrace)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set_target_properties(fonttrace PROPERTIES PREFIX "" SUFFIX ".wdx64")
else()
  set_target_properties(fonttrace PROPERTIES PREFIX "" SUFFIX ".wdx")
endif()

add_executable(sfnt_reader_tests
  native/sfnt_reader_tests.cpp
  native/sfnt_reader.cpp
)

target_include_directories(sfnt_reader_tests PRIVATE native)
fonttrace_configure_target(sfnt_reader_tests)

enable_testing()
add_test(NAME sfnt_reader_tests COMMAND sfnt_reader_tests)

add_executable(wdx_probe native/wdx_probe.cpp)
fonttrace_configure_target(wdx_probe)

add_executable(wdx_exports_tests native/wdx_exports_tests.cpp)
fonttrace_configure_target(wdx_exports_tests)
add_dependencies(wdx_exports_tests fonttrace)
add_test(
  NAME wdx_exports_tests
  COMMAND wdx_exports_tests $<TARGET_FILE:fonttrace>
)
