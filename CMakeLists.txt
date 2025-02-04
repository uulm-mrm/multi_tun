cmake_minimum_required(VERSION 3.5)

# try to build a static executable
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(CMAKE_FIND_LIBRARY_SUFFIXES ${CMAKE_STATIC_LIBRARY_SUFFIX})
set(BUILD_SHARED_LIBS OFF)

project(multi_tun)

include(FetchContent)

FetchContent_Declare(libtuntap URL ${CMAKE_CURRENT_SOURCE_DIR}/ext/libtuntap
                                   SYSTEM)
FetchContent_MakeAvailable(libtuntap)

set(BUILD_STATIC_LIBS ON)
FetchContent_Declare(libsocket URL ${CMAKE_CURRENT_SOURCE_DIR}/ext/libsocket
                                   SYSTEM)
FetchContent_MakeAvailable(libsocket)

FetchContent_Declare(argparse URL ${CMAKE_CURRENT_SOURCE_DIR}/ext/argparse
                                  SYSTEM)
FetchContent_MakeAvailable(argparse)

add_executable(multi_tun multi_tun.cpp)
target_compile_options(multi_tun PUBLIC -Wall -Wextra -pedantic -Werror)
target_include_directories(
  multi_tun SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ext/libsocket/headers)
target_link_libraries(multi_tun PRIVATE tuntap++ socket++ argparse -static)
