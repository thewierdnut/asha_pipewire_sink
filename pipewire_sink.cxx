#include "asha/Asha.hh"

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




int main()
{
   // To set the Buffer thread to realtime priority:
   // struct rlimit limit{150, 151}; // Don't consume more than 150us of cpu at a time
   // if (setrlimit(RLIMIT_RTTIME, &limit) < 0)
   //    g_info("Unable to set rlimit: %d %s", errno, strerror(errno));
   // // Now that we have set a reasonable rlimit, we can call org.freedesktop.RealtimeKit1
   // // /org/freedesktop/RealtimeKit1 org.freedesktop.RealtimeKit1 MakeThreadRealtimeWithPID(pid, tid, 1)
   // // of the buffer thread, and it will give us SCHED_RR
   
   g_info("Starting...");
   std::shared_ptr<GMainLoop> loop(g_main_loop_new(nullptr, true), g_main_loop_unref);
   auto quitter1 = g_unix_signal_add(SIGINT, Shutdown, loop.get());
   auto quitter2 = g_unix_signal_add(SIGTERM, Shutdown, loop.get());
   asha::Asha a;

   static size_t dropped = 0;
   static size_t retries = 0;

   guint stat_timer = g_timeout_add(10000, [](void* userdata)->gboolean {
      auto& a = *(asha::Asha*)userdata;

      size_t new_dropped = a.RingDropped();
      size_t new_retries = a.Retries();

      std::cout << "Ring Occupancy: " << a.Occupancy()
                << " High: " << a.OccupancyHigh()
                << " Ring Dropped: " << new_dropped - dropped
                << " Total: " << new_dropped
                << " Retries: " << new_retries - retries
                << " Total: " << new_retries
                << '\n';

      dropped = new_dropped;
      retries = new_retries;
      return G_SOURCE_CONTINUE;
   }, &a);

   g_main_loop_run(loop.get());
   g_source_remove(stat_timer);
   g_source_remove(quitter1);
   g_source_remove(quitter2);

   g_info("Stopping...");
   return 0;
}