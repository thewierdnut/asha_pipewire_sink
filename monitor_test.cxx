#include "asha/BluetoothMonitor.hh"
#include "asha/Config.hh"
#include "asha/GattProfile.hh"
// #include "asha/Profile.hh"
// #include "asha/Discover.hh"

#include <glib-2.0/glib.h>
#include <glib-2.0/glib-unix.h>

#include <cstdlib>
#include <iostream>
#include <vector>
#include <fstream>


int main(int argc, char** argv)
{
   asha::Config::ReadArgs(argc, argv);

   setenv("G_MESSAGES_DEBUG", "all", false);

   std::shared_ptr<GMainLoop> loop(g_main_loop_new(nullptr, true), g_main_loop_unref);
   auto quitter = g_unix_signal_add(SIGINT, [](void* ml) {
      g_main_loop_quit((GMainLoop*)ml);
      return (int)G_SOURCE_CONTINUE;
   }, loop.get());

   // Sets up a passive advertisement monitor and will auto-connect devices if
   // hit the configured rssi levels.
   asha::BluetoothMonitor monitor;
   monitor.EnableRssiLogging(true);
   
   
   // asha::Profile profile;
   
   // Wow... This kind of works, but it actively pelts the airwaves with scan requests.
   // asha::Discover discover;
   // discover.SetMinRssi(-85);
   // discover.AddUUID("0xfdf0");
   // discover.AddUUID("7d74f4bd-c74a-4431-862c-cce884371592");
   // discover.StartDiscovery();

   // asha::GattProfile gatt_profile;

   g_main_loop_run(loop.get());
   g_source_remove(quitter);

   std::cout << "Stopping...\n";

   return 0;
}