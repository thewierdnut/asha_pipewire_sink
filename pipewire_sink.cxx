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
   g_info("Starting...");
   std::shared_ptr<GMainLoop> loop(g_main_loop_new(nullptr, true), g_main_loop_unref);
   auto quitter1 = g_unix_signal_add(SIGINT, Shutdown, loop.get());
   auto quitter2 = g_unix_signal_add(SIGTERM, Shutdown, loop.get());
   asha::Asha a;

   g_main_loop_run(loop.get());
   g_source_remove(quitter1);
   g_source_remove(quitter2);

   g_info("Stopping...");
   return 0;
}