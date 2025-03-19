pkg_check_modules(GTKMM IMPORTED_TARGET gtkmm-4.0)


if (NOT GTKMM_FOUND)
   message("Gtk gui will not be built unless you install gtkmm-4.0 development libraries")
else()
   add_executable(asha_gui
      asha_gui.cxx

      MainWindow.cxx
      DeviceWidget.cxx

      # TODO: pack all of this in a variable or a library or something
      ../asha/Asha.cxx
      ../asha/Bluetooth.cxx
      ../asha/Buffer.cxx
      ../asha/BufferThreaded.cxx
      ../asha/BufferTimed.cxx
      ../asha/Config.cxx
      ../asha/Characteristic.cxx
      ../asha/Device.cxx
      ../asha/GVariantDump.cxx
      ../asha/Side.cxx
      ../asha/RawHci.cxx
      ../asha/GattProfile.cxx
      ../asha/ObjectManager.cxx
      ../asha/Properties.cxx

      ../g722/g722_encode.c

      ../pw/Stream.cxx
      ../pw/Thread.cxx
   )
   target_link_libraries(asha_gui
      PkgConfig::GTKMM
      PkgConfig::GLIB
      PkgConfig::PIPEWIRE
   )
endif()