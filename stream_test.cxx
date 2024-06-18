#include "asha/Bluetooth.hh"
#include "asha/RawHci.hh"
#include "asha/Side.hh"

#include <poll.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>
#include <glib-2.0/glib.h>
#include <glib-2.0/glib-unix.h>

#include <cstdlib>
#include <iostream>
#include <vector>
#include <fstream>
#include <thread>


double now()
{
   struct timespec ts{};
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}

std::vector<uint8_t> ReadFile(const std::string& path)
{
   std::vector<uint8_t> ret;
   std::ifstream in(path, std::ios::binary);
   if (!in)
      throw std::runtime_error("Unable to read " + path);
   in.seekg(0, std::ios::end);
   ret.resize(in.tellg());
   in.seekg(0);
   in.read((char*)ret.data(), ret.size());

   if (ret.size() < 160)
      throw std::runtime_error("Not enough data in " + path);
   return ret;
}

class StreamTest
{
public:
   StreamTest(const std::string& left_path, const std::string& right_path):
      m_data_left{left_path.empty() ? std::vector<uint8_t>() : ReadFile(left_path)},
      m_data_right{right_path.empty() ? std::vector<uint8_t>() : ReadFile(right_path)},
      m_b(
         [this](const asha::Bluetooth::BluezDevice& d) { OnAddDevice(d); },
         [this](const std::string& p) { OnRemoveDevice(p); }
      )
   {
      
   }

   ~StreamTest()
   {
      Stop();
   }

   void SetVolume(int8_t volume)
   {
      for (auto& d: m_devices)
         d.second->SetStreamVolume(volume);
      m_volume = volume;
   }

   bool Start(const std::string& algorithm)
   {
      m_algorithm = algorithm;
      if (!m_running)
      {
         m_running = true;
         if (algorithm == "fixed")
         {
            std::cout << "Running with fixed rate frames\n";
            m_thread = std::thread(&StreamTest::FeederThreadFixed, this);
         }
         else if (algorithm == "poll")
         {
            std::cout << "Running with socket output polling\n";
            m_thread = std::thread(&StreamTest::FeederThreadPoll, this);
         }
         else if (algorithm == "deadline")
         {
            std::cout << "Running with variable rate frames on a deadline\n";
            m_thread = std::thread(&StreamTest::FeederThreadDeadline, this);
         }
         else
         {
            m_running = false;
            return false;
         }
      }

      return true;
   }

   void Stop()
   {
      if (m_running)
      {
         m_running = false;
         m_thread.join();
      }
   }

protected:
   void OnAddDevice(const asha::Bluetooth::BluezDevice& d)
   {
      std::shared_ptr<asha::Side> side = asha::Side::CreateIfValid(d);
      if (side && side->ReadProperties())
      {
         auto& props = side->GetProperties();

         std::cout << side->Description().c_str() << '\n';
         std::cout << "    Name:      " << side->Name().c_str() << '\n';
         std::cout << "    Mac:       " << side->Mac().c_str() << '\n';
         std::cout << "    HiSyncId:  " << props.hi_sync_id << '\n';
         if (side->Name() != side->Alias())
            std::cout << "    Alias:     " << side->Alias().c_str() << '\n';
         std::cout << "    Side:      "
            << ((props.capabilities & 0x01) ? "right" : "left") << " "
            << ((props.capabilities & 0x02) ? "(binaural)" : "(monaural)") << '\n';
         std::cout << "    Delay:     " << props.render_delay << " ms\n";
         std::cout << "    Streaming: " << (props.feature_map & 0x01 ? "supported" : "not supported" ) << '\n';
         std::string codecs;
         if (props.codecs & 0x02)
            codecs += " G.722@16kHz";
         if (props.codecs & 0x04)
            codecs += " G.722@24kHz";
         std::cout << "    Codecs:   " << codecs.c_str() << '\n';
         if (m_data_left.empty() && side->Left())
         {
            std::cout << "    Ignoring this device, since you didn't specify a left audio file\n";
            return;
         }
         if (m_data_right.empty() && side->Right())
         {
            std::cout << "    Ignoring this device, since you didn't specify a right audio file\n";
            return;
         }
         std::cout << "    Connected: " << (side->Connect() ? "true": "false") << '\n';
         CheckPHY(side);
         usleep(10000);
         side->SetStreamVolume(m_volume);

         // RawHci raw_hci(side->Mac(), side->Sock());
         // if (raw_hci.SendConnectionUpdate(8, 8, 10, 100))
         // {
         //    std::cout << "    Switched to connection interval 8 (10 ms)\n";
         // }
         // else
         // {
         //    std::cout << "    Unable to set connection interval (needs CAP_NET_RAW for this to work)\n";
         // }

         Stop();

         m_devices[d.path] = side;

         if (!m_algorithm.empty())
         {
            Start(m_algorithm);
         }
      }
      else
      {
         std::cout << d.name.c_str() << " is not an asha-enabled device\n";
      }
   }
   void OnRemoveDevice(const std::string& path)
   {
      auto it = m_devices.find(path);
      if (it != m_devices.end())
      {
         std::cout << "Removing " << it->second->Description().c_str() << '\n';
         Stop();
         m_devices.erase(it);
         if (!m_devices.empty())
         {
            Start(m_algorithm);
         }
      }
   }

   void FeederThreadPoll()
   {
      bool other = false;
      for (auto& kv: m_devices)
      {
         kv.second->Start(other);
         other = true;
      }

      std::vector<struct pollfd> fds;
      double start = now();
      double next = start + 10;
      double pos = start ;
      uint8_t sequence = 0;
      while (m_running)
      {
         double t = now();
         while (next < t)
         {
            std::cout << "Stream latency: " << t - pos << '\n';
            next += 10;
         }
         for (auto& kv: m_devices)
         {
            fds.clear();
            if (kv.second->Ready())
            {
               fds.emplace_back(pollfd{
                  .fd = kv.second->Sock(),
                  .events = POLLOUT
               });
            }
         }
         poll(fds.data(), fds.size(), 20);
         

         bool ready_for_data = true;
         for (auto& fd: fds)
         {
            if ((fd.revents & POLLOUT) == 0)
               ready_for_data = false;
         }

         if (ready_for_data)
         {
            AudioPacket packet;
            packet.seq = sequence++;
            for (auto& kv: m_devices)
            {
               memcpy(packet.data, (kv.second->Left() ? m_data_left.data() : m_data_right.data()) + m_data_offset, 160);
               kv.second->WriteAudioFrame(packet);

            }
            m_data_offset += 160;
            if (!m_data_left.empty() && m_data_offset + 160 > m_data_left.size())
               m_data_offset = 0;
            else if (!m_data_right.empty() && m_data_offset + 160 > m_data_right.size())
               m_data_offset = 0;
            pos += .02;
         }
      }
      for (auto& kv: m_devices)
         kv.second->Stop();
   }


   void FeederThreadFixed()
   {
      bool other = false;
      for (auto& kv: m_devices)
      {
         kv.second->Start(other);
         other = true;
      }

      std::vector<struct pollfd> fds;
      double start = now();
      double next = start + 10;
      double pos = start ;
      uint8_t sequence = 0;
      while (m_running)
      {
         usleep(2000);
         double t = now();
         while (next < t)
         {
            std::cout << "Stream latency: " << t - pos << '\n';
            next += 10;
         }

         bool ready_for_data = t - pos > .02;

         if (ready_for_data)
         {
            AudioPacket packet;
            packet.seq = sequence++;
            for (auto& kv: m_devices)
            {
               memcpy(packet.data, (kv.second->Left() ? m_data_left.data() : m_data_right.data()) + m_data_offset, sizeof(packet.data));
               kv.second->WriteAudioFrame(packet);

            }
            m_data_offset += 160;
            if (!m_data_left.empty() && m_data_offset + 160 > m_data_left.size())
               m_data_offset = 0;
            else if (!m_data_right.empty() && m_data_offset + 160 > m_data_right.size())
               m_data_offset = 0;
            pos += .02;
         }
      }
      for (auto& kv: m_devices)
         kv.second->Stop();
   }

   void FeederThreadDeadline()
   {
      bool other = false;
      for (auto& kv: m_devices)
      {
         kv.second->Start(other);
         other = true;
      }

      std::vector<struct pollfd> fds;
      double start = now();
      double next = start + 10;
      double pos = start ;
      uint8_t sequence = 0;
      while (m_running)
      {
         usleep(30000 + rand() % 10000);
         double t = now();
         while (next < t)
         {
            std::cout << "Stream latency: " << t - pos << '\n';
            next += 10;
         }

         while (t - pos > .02)
         {
            AudioPacket packet;
            packet.seq = sequence++;
            for (auto& kv: m_devices)
            {
               memcpy(packet.data, (kv.second->Left() ? m_data_left.data() : m_data_right.data()) + m_data_offset, 160);
               kv.second->WriteAudioFrame(packet);

            }
            m_data_offset += 160;
            if (!m_data_left.empty() && m_data_offset + 160 > m_data_left.size())
               m_data_offset = 0;
            else if (!m_data_right.empty() && m_data_offset + 160 > m_data_right.size())
               m_data_offset = 0;
            pos += .02;
         }
      }
      for (auto& kv: m_devices)
         kv.second->Stop();
   }

   void CheckPHY(const std::shared_ptr<asha::Side>& device)
   {
      uint32_t phys = 0;
      for (int i = 0; i < 60; ++i)
      {
         socklen_t size = sizeof(phys);
         int err = getsockopt(device->Sock(), SOL_BLUETOOTH, BT_PHY, &phys, &size);
         if (err < 0)
         {
            std::cout << "    Error retrieving BT_PHY: " << strerror(-err) << " (" <<  -err << ")\n";
            return;
         }

         if (phys & BT_PHY_LE_2M_TX)
            break;

         usleep(20000);
      }

      std::string phystr = std::to_string(phys);
      if (phys & BT_PHY_BR_1M_1SLOT) phystr += " BR_1M_1SLOT";
      if (phys & BT_PHY_BR_1M_3SLOT) phystr += " BR_1M_3SLOT";
      if (phys & BT_PHY_BR_1M_5SLOT) phystr += " BR_1M_5SLOT";
      if (phys & BT_PHY_EDR_2M_1SLOT) phystr += " EDR_2M_1SLOT";
      if (phys & BT_PHY_EDR_2M_3SLOT) phystr += " EDR_2M_3SLOT";
      if (phys & BT_PHY_EDR_2M_5SLOT) phystr += " EDR_2M_5SLOT";
      if (phys & BT_PHY_EDR_3M_1SLOT) phystr += " EDR_3M_1SLOT";
      if (phys & BT_PHY_EDR_3M_3SLOT) phystr += " EDR_3M_3SLOT";
      if (phys & BT_PHY_EDR_3M_5SLOT) phystr += " EDR_3M_5SLOT";
      if (phys & BT_PHY_LE_1M_TX) phystr += " LE_1M_TX";
      if (phys & BT_PHY_LE_1M_RX) phystr += " LE_1M_RX";
      if (phys & BT_PHY_LE_2M_TX) phystr += " LE_2M_TX";
      if (phys & BT_PHY_LE_2M_RX) phystr += " LE_2M_RX";
      if (phys & BT_PHY_LE_CODED_TX) phystr += " LE_CODED_TX";
      if (phys & BT_PHY_LE_CODED_RX) phystr += " LE_CODED_RX";
      std::cout << "    PHY:    " << phystr.c_str() << '\n';
   }

private:
   std::vector<uint8_t> m_data_left;
   std::vector<uint8_t> m_data_right;
   size_t m_data_offset = 0;

   std::map<std::string, std::shared_ptr<asha::Side>> m_devices;

   volatile bool m_running = false;
   std::thread m_thread;
   std::string m_algorithm;

   uint8_t m_seq = 0;
   int8_t m_volume = -64;

   asha::Bluetooth m_b; // needs to be last
};

void HelpAndExit(const std::string& path)
{
   std::cout << "Utility to test streaming methods for raw g722 data.\n"
             << "Usage: " << path << " [arguments]\n"
             << "Arguments:\n"
             << "   --left  <raw g722 file>     File to feed to left or mono devices\n"
             << "   --right <raw g722 file>     File to feed to right devices\n"
             << "   --algorithm (deadline|fixed|poll) Streaming algorithm to use [default: deadline]\n"
             << "   --volume [-128 to 0]        Set the volume [default: -64]\n";
   exit(1);
}

int main(int argc, char** argv)
{
   setenv("G_MESSAGES_DEBUG", "all", false);
   srand(time(nullptr));

   std::string left_path;
   std::string right_path;
   std::string algorithm = "deadline";
   int8_t volume = -64;

   for (int i = 1; i < argc; ++i)
   {
      if (0 == strcmp(argv[i], "--left") && i + 1 < argc)
         left_path = argv[++i];
      else if (0 == strcmp(argv[i], "--right") && i + 1 < argc)
         right_path = argv[++i];
      else if (0 == strcmp(argv[i], "--algorithm") && i + 1 < argc)
         algorithm = argv[++i];
      else if (0 == strcmp(argv[i], "--volume") && i + 1 < argc)
      {
         int v = atoi(argv[++i]);
         if (v < -128) v = -128;
         if (v > 0)    v = 0;
         volume = v;
      }
      else
      {
         std::cout << "Don't understand argument\n";
         HelpAndExit(argv[0]);
      }
   }

   if (right_path.empty() && left_path.empty())
   {
      std::cout << "Must specify --left or --right\n";
      HelpAndExit(argv[0]);
   }
   if (!right_path.empty() && !std::ifstream(right_path))
   {
      std::cout << "Cannot read right file\n";
      HelpAndExit(argv[0]);
   }
   if (!left_path.empty() && !std::ifstream(left_path))
   {
      std::cout << "Cannot read left file\n";
      HelpAndExit(argv[0]);
   }

   StreamTest c(left_path, right_path);
   c.SetVolume(volume);

   if (!c.Start(algorithm))
   {
      std::cout << "Not a supported algorithm\n";
      HelpAndExit(argv[0]);
   }

   std::shared_ptr<GMainLoop> loop(g_main_loop_new(nullptr, true), g_main_loop_unref);
   auto quitter = g_unix_signal_add(SIGINT, [](void* ml) {
      g_main_loop_quit((GMainLoop*)ml);
      return (int)G_SOURCE_CONTINUE;
   }, loop.get());

   g_main_loop_run(loop.get());
   g_source_remove(quitter);

   std::cout << "Stopping...\n";

   return 0;
}
