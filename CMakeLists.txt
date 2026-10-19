cmake_minimum_required(VERSION 3.22)

include(qpm_defines.cmake OPTIONAL)
if(NOT DEFINED MOD_ID)
  set(MOD_ID "hitbloq")
endif()
if(NOT DEFINED MOD_VERSION)
  set(MOD_VERSION "0.2.0")
endif()
if(NOT DEFINED COMPILE_ID)
  set(COMPILE_ID "hitbloq")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/build)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/build)

project(${COMPILE_ID})

add_compile_options(-frtti -fexceptions -fPIC -O2 -Wno-error)
add_compile_definitions(MOD_ID="${MOD_ID}" VERSION="${MOD_VERSION}")

file(GLOB_RECURSE cpp_file_list src/*.cpp)
add_library(${COMPILE_ID} SHARED ${cpp_file_list})
target_include_directories(${COMPILE_ID} PRIVATE src)

include(extern.cmake)

# Link every shared library qpm downloaded (includes libsl2.so, which provides A64HookFunction)
file(GLOB EXTERN_SOS ${CMAKE_CURRENT_SOURCE_DIR}/extern/libs/*.so)
message(STATUS "extern libs: ${EXTERN_SOS}")
target_link_libraries(${COMPILE_ID} PRIVATE ${EXTERN_SOS} -llog)

# Safety net: symbols provided by the modloader at runtime may stay unresolved at link time
target_link_options(${COMPILE_ID} PRIVATE -Wl,-z,undefs)
