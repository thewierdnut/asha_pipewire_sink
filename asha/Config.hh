#pragma once

#include <cstdint>
#include <string>
#include <map>


namespace asha
{

// Keep track of configuration options. Can be read from command line or
// a config file.
class Config final
{
public:
   static void Read(std::istream& in);
   static void Write(std::ostream& out);
   static void ReadArgs(int argc, char** argv);
   static void HelpAndExit(const std::string& error);
   static void AddExtraStringOption(const std::string& name, const std::string& description);
   static void SetHelpDescription(const std::string& s) { s_description = s; }

   enum BufferAlgorithmEnum { NONE, THREADED, POLL4, POLL8, TIMED, BUFFER_ALGORITHM_ENUM_SIZE };
   static BufferAlgorithmEnum BufferAlgorithm() { return s_buffer_algorithm; }
   static uint16_t Interval() { return s_interval; }
   static uint16_t Timeout() { return s_timeout; }
   static uint16_t Celength() { return s_celength; }
   static int8_t LeftVolume() { return s_left_volume; }
   static int8_t RightVolume() { return s_right_volume; }
   static bool Phy1m() { return s_phy1m; }
   static bool Phy2m() { return s_phy2m; }
   static bool Reconnect() { return s_reconnect; }

   static const std::string& Extra(const std::string& s);

   static bool Modified() { return s_modified; }

   static bool SetConfigItem(const std::string& key, const std::string& value);
   static bool SetConfigItem(const std::string& key, const BufferAlgorithmEnum& value);
   static bool SetConfigItem(const std::string& key, int8_t value) { return SetConfigItem(key, std::to_string((int)value));}
   static bool SetConfigItem(const std::string& key, uint8_t value) { return SetConfigItem(key, std::to_string((unsigned)value));}
   static bool SetConfigItem(const std::string& key, bool value);

private:
   static void ParseConfigItem(const std::string& key, const std::string& value);

   static std::string s_prog_name;
   static BufferAlgorithmEnum s_buffer_algorithm;
   static uint16_t s_interval;
   static uint16_t s_timeout;
   static uint16_t s_celength;
   static int8_t s_left_volume;
   static int8_t s_right_volume;
   static uint8_t s_left_microphone;
   static uint8_t s_right_microphone;
   static bool s_phy1m;
   static bool s_phy2m;
   static bool s_reconnect;

   static bool s_modified; // Whether or not the configuration was modified by the user.

   static std::string s_description;
   struct ExtraOption
   {
      const std::string description;
      std::string value;
   };
   static std::map<std::string, ExtraOption> s_extra;

   // Other potential config items:
   //    * pdu length
   //    * external volume/mute
};

}