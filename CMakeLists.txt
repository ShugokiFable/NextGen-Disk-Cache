cmake_minimum_required(VERSION 3.21)
project(NextGenDiskCache VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(NextGenDiskCache SHARED src/main.cpp)

target_include_directories(NextGenDiskCache PRIVATE
  ${CMAKE_SOURCE_DIR}/deps/include
)

target_link_directories(NextGenDiskCache PRIVATE
  ${CMAKE_SOURCE_DIR}/deps/libs/detours
)

target_link_libraries(NextGenDiskCache PRIVATE detours.lib)

# Static CRT to avoid redistributable dependency on user machines
set_property(TARGET NextGenDiskCache PROPERTY
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

target_compile_definitions(NextGenDiskCache PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _CRT_SECURE_NO_WARNINGS
)

if(MSVC)
  target_compile_options(NextGenDiskCache PRIVATE /W3 /EHsc /Zi)
  target_link_options(NextGenDiskCache PRIVATE /DEBUG /OPT:REF /OPT:ICF)
endif()

set(OUT_DIR "${CMAKE_SOURCE_DIR}/package/SKSE/Plugins")
set_target_properties(NextGenDiskCache PROPERTIES
  OUTPUT_NAME "NextGenDiskCache"
  SUFFIX ".dll"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OUT_DIR}"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${OUT_DIR}"
  LIBRARY_OUTPUT_DIRECTORY_RELEASE "${OUT_DIR}"
  ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${OUT_DIR}"
  PDB_OUTPUT_DIRECTORY_RELEASE "${OUT_DIR}"
)
