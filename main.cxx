#include "Asha.hh"

#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

std::vector<uint8_t> ReadFile(const std::string& path)
{
   std::vector<uint8_t> ret;
   std::ifstream in(path, std::ios::binary);
   if (!in)
   {
      std::cout << "Unable to read " << path << '\n';
      exit(1);
   }
   in.seekg(0, std::ios::end);
   ret.resize(in.tellg());
   in.seekg(0);
   in.read((char*)ret.data(), ret.size());
   return ret;
}


int main(int argc, char** argv)
{
   // Potential command line arguments:
   //    * Which device (by id or by name) to use. Default to first device
   //      (Probably only going to be one anyways).
   //    * left/right/both (default both)
   //    * file to play? stdin?
   //    * logging and verbosity
   std::string left_file;
   std::string right_file;
   for (int i = 1; i < argc; ++i)
   {
      const std::string arg = argv[i];
      const std::string param = i + 1 < argc ? argv[i+1] : "";
      if (arg == "--left")
      {
         if (param.empty())
         {
            std::cout << "--left requires a file argument\n";
            return 1;
         }
         left_file = param;
         ++i;
      }
      else if (arg == "--right")
      {
         if (param.empty())
         {
            std::cout << "--right requires a file argument\n";
            return 1;
         }
         right_file = param;
         ++i;
      }
      else if (arg.substr(0, 1) == "-")
      {
         std::cout << "Unknown argument: " << arg << '\n';
         return 1;
      }
      else
      {
         right_file = left_file = arg;
      }
   }

   // Load the g.722 files into memory
   auto left_data = ReadFile(left_file);
   auto right_data = ReadFile(right_file);

   if (left_data.size() != right_data.size())
   {
      std::cout << "Left and right files are not the same size\n";
      return 1;
   }

   Asha a;

   uint64_t device_id = 0;
   for (auto& device: a.Devices())
   {
      std::cout << device.id << " " << device.name << '\n';
      device_id = device.id;
   }

   if (device_id == 0)
   {
      std::cout << "No devices found\n";
      return 1;
   }

   a.SelectDevice(device_id);
   a.Start();

   size_t offset = 0;
   size_t frame_size = 160;

   // Send some frames immediately, then send the rest every 20 ms
   // TODO: std::chrono is horribly slow compared to directly calling
   //       clock_gettime. Do we care?
   auto stamp = std::chrono::steady_clock::now() - std::chrono::milliseconds(-60);
   while (offset < left_data.size() && offset < right_data.size())
   {
      auto now = std::chrono::steady_clock::now();
      while (now - stamp > std::chrono::milliseconds(0))
      {
         if (!a.SendAudio(left_data.data() + offset, right_data.data() + offset, frame_size))
         {
            std::cout << "Disconnected\n";
            return 1;
         }
         offset += frame_size;
         stamp += std::chrono::milliseconds(20);
         if (offset >= left_data.size() || offset >= right_data.size())
            break;
      }
      // Process data for up to 10 ms
      a.Process(10);
      //std::this_thread::sleep_for(std::chrono::milliseconds(10));
   }

   return 0;
}