cmake_minimum_required(VERSION 3.9)

project(asha_play)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 14)

if (ENABLE_ASAN)
   set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-omit-frame-pointer -fsanitize=address")
   set (CMAKE_LINKER_FLAGS "${CMAKE_LINKER_FLAGS} -fno-omit-frame-pointer -fsanitize=address")
endif()

enable_testing()
find_package(PkgConfig REQUIRED)
# pkg_check_modules(DBUS REQUIRED dbus-1)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0 gio-2.0)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3 libspa-0.2)


add_subdirectory(asha/unit/)

add_executable(asha_connection_test
   asha/Bluetooth.cxx
   asha/Characteristic.cxx
   asha/Config.cxx
   asha/Device.cxx
   asha/GVariantDump.cxx
   asha/Properties.cxx
   asha/RawHci.cxx
   asha/Side.cxx

   g722/g722_encode.c

   connect_test.cxx
)
target_link_libraries(asha_connection_test PkgConfig::GLIB)

add_executable(asha_stream_test
   asha/Bluetooth.cxx
   asha/Buffer.cxx
   asha/BufferThreaded.cxx
   asha/BufferTimed.cxx
   asha/Characteristic.cxx
   asha/Config.cxx
   asha/Device.cxx
   asha/GVariantDump.cxx
   asha/Properties.cxx
   asha/RawHci.cxx
   asha/Side.cxx

   g722/g722_encode.c

   stream_test.cxx
)
target_link_libraries(asha_stream_test PkgConfig::GLIB)


add_executable(asha_pipewire_sink
   asha/Asha.cxx
   asha/Bluetooth.cxx
   asha/Buffer.cxx
   asha/BufferThreaded.cxx
   asha/BufferTimed.cxx
   asha/Config.cxx
   asha/Characteristic.cxx
   asha/Device.cxx
   asha/GVariantDump.cxx
   asha/Side.cxx
   asha/RawHci.cxx
   asha/GattProfile.cxx
   asha/ObjectManager.cxx
   asha/Properties.cxx

   g722/g722_encode.c

   pw/Stream.cxx
   pw/Thread.cxx

   pipewire_sink.cxx
)
target_link_libraries(asha_pipewire_sink PkgConfig::GLIB PkgConfig::PIPEWIRE)


add_executable(snoop_analyze snoop_analyze.cxx)

# add_executable(monitor_test
#    asha/BluetoothMonitor.cxx
#    asha/Discover.cxx
#    asha/Profile.cxx
#    asha/GattProfile.cxx
#    asha/GVariantDump.cxx
#    asha/ObjectManager.cxx
#
#    monitor_test.cxx
# )
# target_link_libraries(monitor_test PkgConfig::GLIB)
