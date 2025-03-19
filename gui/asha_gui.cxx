#include "MainWindow.hh"
#include <glibmm/miscutils.h>
#include <gtkmm/application.h>
#include <glibmm/init.h>

#include "../asha/Config.hh"

#include <fstream>
#include <iostream>

int main(int argc, char** argv)
{
   // glibmm overwrites the default locale for iostream formatting. This makes
   // string_stream operations behave unexpectedly.
   Glib::set_init_to_users_preferred_locale(false);

   // Spews messages to stdout... probably should remove this at some point.
   setenv("G_MESSAGES_DEBUG", "all", false);

   // Load settings config file first, then command line second, so that
   // command line takes priority.
   std::string configfile = Glib::get_user_config_dir() + "/com.github.thewierdnut.asha_gui/settings.conf";
   g_mkdir_with_parents((Glib::get_user_config_dir() + "/com.github.thewierdnut.asha_gui").c_str(), 0700);
   std::cout << configfile << '\n';
   std::ifstream in(configfile);
   if (in)
      asha::Config::Read(in);
   asha::Config::ReadArgs(argc, argv);

   // Run the app.
   auto app = Gtk::Application::create("com.github.thewierdnut.asha_gui");
   int retval = app->make_window_and_run<MainWindow>(0, nullptr);

   // Save any settings that have been changed.
   if (asha::Config::Modified())
   {
      std::ofstream out(configfile);
      if (out)
         asha::Config::Write(out);
   }
   return retval;
}