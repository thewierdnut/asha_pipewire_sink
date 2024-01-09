cmake_minimum_required(VERSION 3.9)

project(asha_play)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 14)


find_package(PkgConfig REQUIRED)
pkg_check_modules(DBUS REQUIRED dbus-1)

include_directories(${DBUS_INCLUDE_DIRS})
link_directories(${DBUS_LIBRARY_DIRS})

add_executable(asha_play
   Asha.cxx
   main.cxx
)

target_link_libraries(asha_play ${DBUS_LIBRARIES})
