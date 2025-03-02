#include "asha/Asha.hh"
#include "asha/Config.hh"
#include "asha/GattProfile.hh"

#include <glib-2.0/glib.h>
#include <glib-2.0/glib-unix.h>

#include <cassert>
#include <fstream>
#include <iostream>
#include <memory>

int Shutdown(void* ml)
{
   g_main_loop_quit((GMainLoop*)ml);
   return G_SOURCE_CONTINUE;
}




int main(int argc, char** argv)
{
   asha::Config::ReadArgs(argc, argv);

   setenv("G_MESSAGES_DEBUG", "all", false);

   g_info("Starting...");
   std::shared_ptr<GMainLoop> loop(g_main_loop_new(nullptr, true), g_main_loop_unref);
   guint quitter1 = g_unix_signal_add(SIGINT, Shutdown, loop.get());
   guint quitter2 = g_unix_signal_add(SIGTERM, Shutdown, loop.get());
   asha::Asha a;

   // Auto-reconnection logic can be "annoyingly persistent". Make it optional.
   std::unique_ptr<asha::GattProfile> profile;
   if (asha::Config::Reconnect())
      profile.reset(new asha::GattProfile);

   static size_t dropped = 0;
   static size_t failed = 0;
   static size_t silence = 0;

   guint stat_timer = g_timeout_add(1000, [](void* userdata)->gboolean {
      auto& a = *(asha::Asha*)userdata;
      if (a.HasDevice())
      {
         size_t new_dropped = a.RingDropped();
         size_t new_failed = a.FailedWrites();
         size_t new_silence = a.Silence();

         std::cout << "Ring Occupancy: " << a.Occupancy()
                  << " High: " << a.OccupancyHigh()
                  << " Ring Dropped: " << new_dropped - dropped
                  << " Total: " << new_dropped
                  << " Adapter Dropped: " << new_failed - failed
                  << " Total: " << new_failed
                  << " Silence: " << new_silence - silence
                  << " Total: " << new_silence
                  << '\n';

         dropped = new_dropped;
         failed = new_failed;
         silence = new_silence;
      }
      return G_SOURCE_CONTINUE;
   }, &a);

   g_main_loop_run(loop.get());
   g_source_remove(stat_timer);
   g_source_remove(quitter1);
   g_source_remove(quitter2);

   g_info("Stopping...");
   return 0;
}
