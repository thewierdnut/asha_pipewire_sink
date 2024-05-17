cmake_minimum_required(VERSION 3.9)

project(asha_play)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 14)

if (ENABLE_ASAN)
   set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-omit-frame-pointer -fsanitize=address")
   set (CMAKE_LINKER_FLAGS "${CMAKE_LINKER_FLAGS} -fno-omit-frame-pointer -fsanitize=address")
endif()

find_package(PkgConfig REQUIRED)
# pkg_check_modules(DBUS REQUIRED dbus-1)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0 gio-2.0)



add_executable(asha_connection_test
   asha/Asha.cxx
   asha/Bluetooth.cxx
   asha/Characteristic.cxx
   asha/RawHci.cxx
   asha/Side.cxx

   asha/GVariantDump.cxx

   connect_test.cxx
)

target_link_libraries(asha_connection_test PkgConfig::GLIB)


add_executable(asha_play_test
   asha/Asha.cxx
   asha/Bluetooth.cxx
   asha/Characteristic.cxx
   asha/RawHci.cxx
   asha/Side.cxx

   asha/GVariantDump.cxx

   play_test.cxx
)

target_link_libraries(asha_play_test PkgConfig::GLIB)

add_executable(encode_test g722/encode_test.cxx g722/g722_encode.c)
