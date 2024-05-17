#include "asha/Asha.hh"

#include <glib-2.0/glib.h>
#include <glib-2.0/glib-unix.h>

#include <iostream>


int main()
{
   asha::Asha a;
   a.EnumerateDevices();
   uint64_t id = 0;
   for (auto& d: a.Devices())
   {
      std::cout << d.name << ": " << d.id << '\n';
      id = d.id;
   }

   a.SelectDevice(id);
   usleep(1000000);
   a.SetVolume(-120);

   std::shared_ptr<GMainLoop> loop(g_main_loop_new(nullptr, true), g_main_loop_unref);
   auto quitter = g_unix_signal_add(SIGINT, [](void* ml) {
      g_main_loop_quit((GMainLoop*)ml);
      return (int)G_SOURCE_CONTINUE;
   }, loop.get());

   g_main_loop_run(loop.get());
   g_source_remove(quitter);

   std::cout << "Stopping...\n";
   a.Stop();

   return 0;
}