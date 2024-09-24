macro(unit_test testname)
   add_executable("${testname}"
      "${testname}.cxx"
      ../Characteristic.cxx
      ../Config.cxx
      ../Device.cxx
      ../GVariantDump.cxx
      ../Properties.cxx
      ../Side.cxx
      ../RawHci.cxx
      ../../g722/g722_encode.c
   )

   target_link_libraries("${testname}"
      PkgConfig::GLIB
   )
   add_test(NAME "${testname}" COMMAND "${testname}")
endmacro(unit_test)


unit_test(test_Device)