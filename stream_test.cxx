#include "asha/Bluetooth.hh"
#include "asha/Buffer.hh"
#include "asha/Now.hh"
#include "asha/Side.hh"
#include "asha/Config.hh"
#include "g722/g722_enc_dec.h"

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


namespace
{
   constexpr size_t MIN_SAMPLES = 320;
   constexpr size_t MIN_SAMPLES_BYTES = MIN_SAMPLES * 2;
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

   if (ret.size() < 640)
      throw std::runtime_error("Not enough data in " + path);
   return ret;
}

class StreamTest
{
public:
   StreamTest(const std::string& left_path, const std::string& right_path):
      m_data_left{left_path.empty() ? std::vector<uint8_t>() : ReadFile(left_path)},
      m_data_right{right_path.empty() ? std::vector<uint8_t>() : ReadFile(right_path)},
      m_buffer{asha::Buffer::Create([this](const RawS16& s) { return OnSendData(s); })},
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

   bool Start()
   {
      if (!m_running)
      {
         m_running = true;
         m_thread = std::thread(&StreamTest::FeederThreadDeadline, this);
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
         
         bool connected = side->Connect();
         std::cout << "    Connected: " << (connected ? "true": "false") << '\n';

         usleep(10000);
         CheckPHY(side);
         
         bool was_running = m_running;
         Stop();

         for (auto& others: m_devices)
            others.second->UpdateOtherConnected(true);

         m_devices[d.path] = side;

         if (was_running)
            Start();
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

         for (auto& others: m_devices)
            others.second->UpdateOtherConnected(false);

         if (!m_devices.empty())
            Start();
      }
   }


   void FeederThreadDeadline()
   {
      g722_encode_init(&m_state_left, 64000, G722_PACKED);
      g722_encode_init(&m_state_right, 64000, G722_PACKED);

      bool other = false;
      for (auto& kv: m_devices)
      {
         kv.second->Start(other);
         other = true;
      }
      m_buffer->Start();

      size_t dropped = 0;
      size_t failed = 0;
      size_t silence = 0;
      
      uint64_t start = Now();
      uint64_t next = start + 10*1000000000ull;
      uint64_t pos = start;
      while (m_running)
      {
         usleep(30000 + rand() % 10000);
         uint64_t t = Now();
         if (next < t)
         {
            size_t new_dropped = m_buffer->RingDropped();
            size_t new_failed = m_buffer->FailedWrites();
            size_t new_silence = m_buffer->Silence();
            std::cout << "Ring Occupancy: " << m_buffer->Occupancy()
                << " High: " << m_buffer->OccupancyHigh()
                << " Ring Dropped: " << new_dropped - dropped
                << " Total: " << new_dropped
                << " Adapter Dropped: " << new_failed - failed
                << " Total: " << new_failed
                << " Silence: " << new_silence - silence
                << " Total: " << new_silence
                << '\n';
            next = t + 10 * 1000000000ull;
         }

         while (t - pos > ASHA_PACKET_TIME)
         {
            RawS16* samples = m_buffer->NextBuffer();
            if (!samples)
               break;

            if (!m_data_left.empty())
               memcpy(samples->l, m_data_left.data() + m_data_offset, MIN_SAMPLES_BYTES);
            if (!m_data_right.empty())
               memcpy(samples->r, m_data_right.data() + m_data_offset, MIN_SAMPLES_BYTES);
            
            m_buffer->SendBuffer();
            
            m_data_offset += MIN_SAMPLES_BYTES;
            if (!m_data_left.empty() && m_data_offset + MIN_SAMPLES_BYTES > m_data_left.size())
               m_data_offset = 0;
            else if (!m_data_right.empty() && m_data_offset + MIN_SAMPLES_BYTES > m_data_right.size())
               m_data_offset = 0;
            pos += ASHA_PACKET_TIME;
         }
      }
      m_buffer->Stop();
      for (auto& kv: m_devices)
         kv.second->Stop();
   }

   bool OnSendData(const RawS16& s)
   {
      if (m_devices.empty()) return false;

      bool ready = true;
      for (auto& kv: m_devices)
      {
         if (!kv.second->Ready())
            return false;
      }

      bool success = false;

      // Do not send unless both streams will not block (because there are
      // l2cap credits available).
      // TODO: Also check for closed socket?
      struct pollfd fds[m_devices.size()];
      size_t i = 0;
      for (auto& kv: m_devices)
      {
         fds[i] = pollfd{
            .fd = kv.second->Sock(),
            .events = POLLOUT
         };
         ++i;
      }
      if (m_devices.size() != poll(fds, m_devices.size(), 0))
      {
         return false;
      }

      AudioPacket packets[2];
      AudioPacket* left;
      AudioPacket* right;

      g722_encode(&m_state_left, packets[0].data, s.l, s.SAMPLE_COUNT);
      g722_encode(&m_state_right, packets[1].data, s.r, s.SAMPLE_COUNT);

      left = &packets[0];
      right = &packets[1];
      if (m_data_left.empty())
         left = right;
      else if (m_data_right.empty())
         right = left;

      left->seq = right->seq = m_audio_seq;

      for (auto& kv: m_devices)
      {
         auto status = kv.second->WriteAudioFrame(kv.second->Right() ? *right : *left);
         switch(status)
         {
         case asha::Side::WRITE_OK:
            success = true;
            break;
         case asha::Side::DISCONNECTED:
            g_info("WriteAudioFrame returned DISCONNECTED");
            // m_reconnect_cb(kv.first);
            break;
         case asha::Side::BUFFER_FULL:
            g_info("WriteAudioFrame returned BUFFER_FULL");
            break;
         case asha::Side::NOT_READY:
            g_info("WriteAudioFrame returned NOT_READY");
            break;
         case asha::Side::TRUNCATED:
            g_info("WriteAudioFrame returned TRUNCATED");
            break;
         case asha::Side::OVERSIZED:
            g_info("WriteAudioFrame returned OVERSIZED");
            break;
         }
      }
      if (success)
         ++m_audio_seq;
      
      return success;
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

   uint8_t m_audio_seq = 0;

   g722_encode_state_t m_state_left{};
   g722_encode_state_t m_state_right{};

   std::shared_ptr<asha::Buffer> m_buffer;
   asha::Bluetooth m_b; // needs to be last
};

void HelpAndExit(const std::string& path)
{
   std::cout << "Utility to test streaming methods for raw g722 data.\n"
             << "Usage: " << path << " [arguments]\n"
             << "Arguments:\n"
             << "   --left  <raw g722 file>     File to feed to left or mono devices\n"
             << "   --right <raw g722 file>     File to feed to right devices\n"
             << "   --volume [-128 to 0]        Set the volume [default: -64]\n"
             << "   --algorithm (deadline|fixed|poll) Streaming algorithm to use [default: fixed]\n"
             << "   --celength                  Attempt to set the ce length [CAP_NET_RAW required]\n"
             << "   --timeout                   Attempt to set the timeout multiplier [CAP_NET_RAW required]\n"
             << "   --phy1m                     Attempt to enable 1M PHY [CAP_NET_RAW required]\n"
             << "   --phy2m                     Attempt to enable 2M PHY [CAP_NET_RAW required]\n";
   exit(1);
}

int main(int argc, char** argv)
{
   asha::Config::SetHelpDescription("Utility to test streaming methods for raw g722 data.");
   asha::Config::AddExtraStringOption("--left", "Raw S16LE File to feed to left or mono devices");
   asha::Config::AddExtraStringOption("--right", "Raw S16LE File to feed to right devices");
   asha::Config::ReadArgs(argc, argv);

   setenv("G_MESSAGES_DEBUG", "all", false);
   srand(time(nullptr));

   std::string left_path = asha::Config::Extra("--left");
   std::string right_path = asha::Config::Extra("--right");

   if (right_path.empty() && left_path.empty())
      asha::Config::HelpAndExit("Must specify --left or --right");
   if (!right_path.empty() && !std::ifstream(right_path))
      asha::Config::HelpAndExit("Cannot read right file");
   if (!left_path.empty() && !std::ifstream(left_path))
      asha::Config::HelpAndExit("Cannot read left file");

   if (left_path.size() < 4 && left_path.substr(left_path.length() - 4) == "g722")
      asha::Config::HelpAndExit("--left file has a g722 extension. You need to pass a raw s16le file instead");
   if (right_path.size() < 4 && right_path.substr(right_path.length() - 4) == "g722")
      asha::Config::HelpAndExit("--right file has a g722 extension. You need to pass a raw s16le file instead");

   StreamTest c(left_path, right_path);
   c.Start();
   
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
