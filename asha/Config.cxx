#include "Config.hh"
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>


using namespace asha;

// Default values defined here.
std::string Config::s_prog_name = "asha_pipewire_sink";
Config::BufferAlgorithmEnum Config::s_buffer_algorithm = Config::THREADED;
uint16_t Config::s_interval = 16;   // Units of 1.25 ms
uint16_t Config::s_timeout = 100;   // Units of 10 ms
uint16_t Config::s_celength = 12;   // Units of 0.625ms
int8_t Config::s_left_volume = -64;       // -128 (muted) to 0
int8_t Config::s_right_volume = -64;      // -128 (muted) to 0
uint8_t Config::s_left_microphone = 0;
uint8_t Config::s_right_microphone = 0;
bool Config::s_phy1m = false;
bool Config::s_phy2m = false;
bool Config::s_reconnect = false;
bool Config::s_modified = false;
int16_t Config::s_rssi_paired = 0;
int16_t Config::s_rssi_unpaired = 0;

// Managing extra options (mostly for stream_test)
std::string Config::s_description = "Implementation of ASHA streaming protocol for pipewire.";
std::map<std::string, Config::ExtraOption> Config::s_extra;

static const char* BUFFER_ALGORITHM_ENUM_STR[] = {"none", "threaded", "poll4", "poll8", "timed"};
static_assert(sizeof(BUFFER_ALGORITHM_ENUM_STR) / sizeof(*BUFFER_ALGORITHM_ENUM_STR) == Config::BufferAlgorithmEnum::BUFFER_ALGORITHM_ENUM_SIZE);


void Config::Read(std::istream& in)
{
   std::string line;
   size_t line_number = 0;
   while (std::getline(in, line))
   {
      ++line_number;
      if (line.empty())
         continue;

      // Space separate key value. Key cannot have spaces, value can.
      auto space_pos = line.find(' ');
      if (space_pos == std::string::npos)
      {
         std::cerr << "Invalid config entry at line #" << line_number;
         continue;
      }
      auto key = line.substr(0, space_pos);
      auto value = line.substr(space_pos + 1);

      try
      {
         ParseConfigItem(key, value);
      }
      catch (const std::runtime_error& e)
      {
         std::cerr << e.what() << '\n';
      }
   }
}

void Config::Write(std::ostream& out)
{
   if (s_buffer_algorithm < BUFFER_ALGORITHM_ENUM_SIZE)
   {
      out << "buffer_algorithm " << BUFFER_ALGORITHM_ENUM_STR[s_buffer_algorithm] << '\n';
   }
   out << "left_volume " << (int)s_left_volume << '\n';
   out << "right_volume " << (int)s_right_volume << '\n';
   out << "left_microphone " << (unsigned)s_left_microphone << '\n';
   out << "right_microphone " << (unsigned)s_right_microphone << '\n';
   out << "interval " << s_interval << '\n';
   out << "timeout " << s_timeout << '\n';
   out << "celength " << s_celength << '\n';
   if (s_phy2m)
      out << "phy2m\n";
   if (s_phy1m)
      out << "phy1m\n";
   if (s_reconnect)
      out << "reconnect\n";
   out << "rssi_paired " << s_rssi_paired << '\n';
   out << "rssi_unpaired " << s_rssi_unpaired << '\n';
   for (auto& kv: s_extra)
      out << kv.first << " " << kv.second.value << '\n';
}

void Config::ReadArgs(int argc, char** argv)
{
   if (argc > 0)
      s_prog_name = argv[0];

   for (size_t i = 1; i < argc; ++i)
   {
      if (0 == strcmp(argv[i], "--help"))
      {
         HelpAndExit("");
      }
      else if (0 == strncmp(argv[i], "--", 2))
      {
         std::string key = argv[i] + 2;
         std::string value = "";
         
         if (i + 1 < argc && 0 != strncmp(argv[i+1], "--", 2))
         {
            ++i;
            value = argv[i];
         }
         else if (key.size() > 3 && key.substr(0, 3) == "no-")
         {
            value = "false";
         }
         else
         {
            value = "true";
         }
         try
         {
            ParseConfigItem(key, value);
         }
         catch (const std::runtime_error& e)
         {
            HelpAndExit(e.what());
         }
      }
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
             // This doesn't work right.
             // << "  --reconnect          Enable the auto-reconnection mechanism. This uses the\n"
             // << "                       bluez gatt profile registration to auto-reconnect, which\n"
             // << "                       may require a bluetoothd restart to disable.\n"
             << "  --rssi_paired        Minimum rssi from (-127 to -1, 0 to disable) which will\n"
             << "                       trigger a reconnection for a previously paired asha\n"
             << "                       device. A value around -80 should work for normal use.\n"
             << "  --rssi_unpaired      Minimum rssi from (-127 to -1, 0 to disable) which will\n"
             << "                       trigger pairing and connection for a previously unseen\n"
             << "                       device. A value around -50 would indicate close proximity\n"
             << "                       to the transmitter.\n";
   auto oldflags = std::cout.flags();
   for (auto& extra: s_extra)
   {
      std::cout << "  " << std::setw(20) << std::left << "--" + extra.first
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

void Config::AddExtraFlagOption(const std::string& name, const std::string& description)
{
   s_extra.emplace(name, ExtraOption{description, "", true});
}

const std::string& Config::Extra(const std::string& s)
{
   static std::string EMPTY;
   auto it = s_extra.find(s);
   return it == s_extra.end() ? EMPTY : it->second.value;
}

bool Config::ExtraBool(const std::string& s)
{
   static std::string EMPTY;
   auto it = s_extra.find(s);
   return it == s_extra.end() ? false: it->second.value == "true";
}

bool Config::SetConfigItem(const std::string& key, const std::string& value)
{
   try
   {
      ParseConfigItem(key, value);
      s_modified = true;
      return true;
   }
   catch (const std::runtime_error& e)
   {
      std::cerr << "Unable to set " << key << ": " << e.what() << '\n';
      return false;
   }
}

bool Config::SetConfigItem(const std::string& key, const BufferAlgorithmEnum& value)
{
   if (value < BUFFER_ALGORITHM_ENUM_SIZE)
      return SetConfigItem(key, BUFFER_ALGORITHM_ENUM_STR[value]);

   return false;
}

bool Config::SetConfigItem(const std::string& key, bool value)
{
   return SetConfigItem(key, value ? "true" : "false");
}

void Config::ParseConfigItem(const std::string& key, const std::string& value)
{
   auto ReadBool = [&]()
   {
      if (!value.empty())
      {
         switch (value.front())
         {
         case 't':
         case 'T':
         case 'y':
         case 'Y':
         case '1':
            return true;
         }
      }
      return false;
   };
   auto ReadString = [&]()
   {
      if (value.empty())
         throw std::runtime_error("Argument required for " + key);
      return value;
   };
   auto ReadInt = [&](int min, int max)
   {
      std::string s = ReadString();
      int ret;
      try
      {
         ret = std::stoi(s);
      }
      catch (...)
      {
         throw std::runtime_error("Invalid argument specified for '" + key + "'.");
      }
      if (ret < min || ret > max)
         throw std::runtime_error(key + " must be in the range " + std::to_string(min) + " to " +std::to_string(max));
      return ret;
   };
   if (key == "buffer_algorithm")
   {
      if (value == "none")
         s_buffer_algorithm = NONE;
      else if (value == "threaded")
         s_buffer_algorithm = THREADED;
      else if (value == "poll4")
         s_buffer_algorithm = POLL4;
      else if (value == "poll8")
         s_buffer_algorithm = POLL8;
      else if (value == "timed")
         s_buffer_algorithm = TIMED;
      else
         throw std::runtime_error("Unknown buffer algorithm");
   }
   else if (key == "volume")
      s_left_volume = s_right_volume = ReadInt(-128, 0);
   else if (key == "left_volume")
      s_left_volume = ReadInt(-128, 0);
   else if (key == "right_volume")
      s_right_volume = ReadInt(-128, 0);
   else if (key == "left_microphone")
      s_left_microphone = ReadInt(-128, 0);
   else if (key == "right_microphone")
      s_right_microphone = ReadInt(-128, 0);
   else if (key == "interval")
      s_interval = ReadInt(6, 16);
   else if (key == "timeout")
      s_timeout = ReadInt(10, 3200);
   else if (key == "celength")
      s_celength = ReadInt(0, 65536);
   else if (key == "phy2m")
      s_phy2m = ReadBool();
   else if (key == "phy1m")
      s_phy1m = ReadBool();
   else if (key == "reconnect")
      s_reconnect = ReadBool();
   else if (key == "rssi_paired")
      s_rssi_paired = ReadInt(-127, 0);
   else if (key == "rssi_unpaired")
      s_rssi_unpaired = ReadInt(-127, 0);
   else if (s_extra.count(key))
   {
      auto& extra = s_extra[key];
      if (extra.is_flag)
         extra.value = value != "false" ? "true" : "false"; // so that an absence of a value is interpreted as "true"
      else
         extra.value = ReadString();
   }
   else
   {
      throw std::runtime_error("Unknown key " + key);
   }
}
