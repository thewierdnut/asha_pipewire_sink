#include "Config.hh"
#include <cstring>
#include <iomanip>
#include <iostream>


using namespace asha;

// Default values defined here.
std::string Config::s_prog_name = "asha_pipewire_sink";
Config::BufferAlgorithmEnum Config::s_buffer_algorithm = Config::THREADED;
uint16_t Config::s_interval = 16;   // Units of 1.25 ms
uint16_t Config::s_timeout = 100;   // Units of 10 ms
uint16_t Config::s_celength = 12;   // Units of 0.625ms
int8_t Config::s_volume = -64;      // -128 (muted) to 0
bool Config::s_phy1m = false;
bool Config::s_phy2m = false;
bool Config::s_reconnect = false;

// Managing extra options (mostly for stream_test)
std::string Config::s_description = "Implementation of ASHA streaming protocol for pipewire.";
std::map<std::string, Config::ExtraOption> Config::s_extra;



void Config::ReadArgs(int argc, char** argv)
{
   int i;
   auto ReadString = [&]()
   {
      std::string argname = argv[i];
      if (i + 1 >= argc)
         HelpAndExit("Argument required for " + argname);
      return argv[++i];
   };

   auto ReadInt = [&](int min, int max)
   {
      std::string argname = argv[i];
      std::string s = ReadString();
      int ret;
      try
      {
         ret = std::stoi(s);
      }
      catch (...)
      {
         HelpAndExit("Invalid " + argname + " specified on command line.");
      }
      if (ret < -128 || ret > 0)
         HelpAndExit(argname + " must be in the range " + std::to_string(min) + " to " +std::to_string(max));
      return ret;
   };

   if (argc > 0)
      s_prog_name = argv[0];
   for (i = 1; i < argc; ++i)
   {
      if (0 == strcmp(argv[i], "--buffer_algorithm"))
      {
         std::string algo = ReadString();
         if (algo == "none")
            s_buffer_algorithm = NONE;
         else if (algo == "threaded")
            s_buffer_algorithm = THREADED;
         else if (algo == "poll4")
            s_buffer_algorithm = POLL4;
         else if (algo == "poll8")
            s_buffer_algorithm = POLL8;
         else if (algo == "timed")
            s_buffer_algorithm = TIMED;
         else
            HelpAndExit("Unknown buffer algorithm");
      }
      else if (0 == strcmp(argv[i], "--volume"))
         s_volume = ReadInt(-128, 0);
      else if (0 == strcmp(argv[i], "--interval"))
         s_interval = ReadInt(6, 16);
      else if (0 == strcmp(argv[i], "--timeout"))
         s_timeout = ReadInt(10, 3200);
      else if (0 == strcmp(argv[i], "--celength"))
         s_celength = ReadInt(0, 65536);
      else if (0 == strcmp(argv[i], "--phy2m"))
         s_phy2m = true;
      else if (0 == strcmp(argv[i], "--phy1m"))
         s_phy1m = true;
      else if (0 == strcmp(argv[i], "--reconnect"))
         s_reconnect = true;
      else if (s_extra.count(argv[i]))
      {
         auto& extra = s_extra[argv[i]];
         extra.value = ReadString();
      }
      else if (0 == strcmp(argv[i], "--help") || 0 == (strcmp(argv[i], "-h")))
         HelpAndExit("");
      else
         HelpAndExit("Unknown argument");
   }
}


void Config::HelpAndExit(const std::string& error)
{
   if (!error.empty())
      std::cout << error << '\n';

   std::cout << s_description << '\n'
             << "Usage: " << s_prog_name << " [options]\n"
             << "Options:\n"
             << "  --buffer_algorithm   One of (none, threaded, poll4, poll8, timed)\n"
             << "                       [Default: threaded]\n"
             << "  --volume             Stream volume from -128 to 0 [Default: -64]\n"
             << "  --reconnect          Enable the auto-reconnection mechanism. This uses the\n"
             << "                       bluez gatt profile registration to auto-reconnect, which\n"
             << "                       may require a bluetoothd restart to disable.\n";
   auto oldflags = std::cout.flags();
   for (auto& extra: s_extra)
   {
      std::cout << "  " << std::setw(20) << std::left << extra.first
                << " " << extra.second.description << '\n';
   }
   std::cout.flags(oldflags);

   std::cout << "  --help               Shows this message\n"
             << "\n"
             << "Options requiring CAP_NET_RAW to be effective:\n"
             << "  --interval           How often the peripheral should check for incoming\n"
             << "                       traffic in units of 1.25ms. This value needs to be low\n"
             << "                       enough to account for 20ms of incoming audio data at the\n"
             << "                       selected PHY. Most devices will not work with anything\n"
             << "                       other than the default. [Default 16]\n"
             << "  --celength           CE Length in units of 0.625ms. This requests that the\n"
             << "                       peripheral wake and listen for traffic for at least this\n"
             << "                       amount of time each cycle. The ASHA spec recommends that\n"
             << "                       this get set to at least 4 times the connection interval\n"
             << "                       multiplied by the PHY data rate. Please note the unit\n"
             << "                       difference between the two settings.\n"
             << "                       [Default 12 with CAP_NET_RAW, 0 without CAP_NET_RAW]\n"
             << "  --timeout            How long in units of 10ms a device is silent before it\n"
             << "                       gets disconnected. [Default 100]\n"
             << "  --phy1m              Request 1M PHY. Requires longer celength, but more stable\n"
             << "                       and longer range [Default enabled]\n"
             << "  --phy2m              Request 2M PHY. Better battery life, shorter bursts work\n"
             << "                       better in busy bluetooth environments. [Default enabled\n"
             << "                       for kernel 6.8 or newer if the peripheral supports it]\n"
             ;

   std::exit(1);
}


void Config::AddExtraStringOption(const std::string& name, const std::string& description)
{
   s_extra.emplace(name, ExtraOption{description});
}


const std::string& Config::Extra(const std::string& s)
{
   static std::string EMPTY;
   auto it = s_extra.find(s);
   return it == s_extra.end() ? EMPTY : it->second.value;
}

