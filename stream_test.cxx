#include "asha/Bluetooth.hh"
#include "asha/Buffer.hh"
#include "asha/Device.hh"
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
      m_b(
         [this](const asha::Bluetooth::BluezDevice& d) { OnAddSide(d); },
         [this](const std::string& p) { OnRemoveDevice(p); }
      ),
      m_device(new asha::Device("Stream Test"))
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
   void OnAddSide(const asha::Bluetooth::BluezDevice& d)
   {
      // Called when we get a new device.
      auto side = asha::Side::CreateIfValid(d);
      if (side)
      {
         g_info("Adding %s", d.path.c_str());

         std::weak_ptr<asha::Side> weak_side = side;
         std::string path = d.path;
         m_sides.emplace(path, side);

         // TODO: Do we need to handle a timeout in case the device never becomes
         //       ready? For now, I'm assuming that a timeout means a missing
         //       device, and bluez will call OnRemoveDevice later.
         side->SetOnConnectionReady([this, path, weak_side](){
            auto side = weak_side.lock();
            if (side)
            {
               SideReady(path, side);
            }
         });
      }
      else
      {
         g_info("%s is not an asha-enabled device", d.name.c_str());
      }
   }

   void SideReady(const std::string& path, const std::shared_ptr<asha::Side>& side)
   {
      auto& props = side->GetProperties();

      g_info("%s", side->Description().c_str());
      g_info("    Name:      %s", side->Name().c_str());
      g_info("    Mac:       %s", side->Mac().c_str());
      g_info("    HiSyncId:  %lu", props.hi_sync_id);
      if (side->Name() != side->Alias())
         g_info("    Alias:     %s", side->Alias().c_str());
      g_info("    Side:      %s %s",
         ((props.capabilities & 0x01) ? "right" : "left"),
         ((props.capabilities & 0x02) ? "(binaural)" : "(monaural)"));
      g_info("    Delay:     %hu ms", props.render_delay);
      g_info("    Streaming: %s", (props.feature_map & 0x01 ? "supported" : "not supported" ));
      std::string codecs;
      if (props.codecs & 0x02)
         codecs += " G.722@16kHz";
      if (props.codecs & 0x04)
         codecs += " G.722@24kHz";
      g_info("    Codecs:   %s", codecs.c_str());

      // sockopts
      // BT_SECURITY? returns BT_SECURITY_* constants, plus key size.
      // BT_DEFER_SETUP? meant for listening sockets.
      // BT_FLUSHABLE? gets the FLAG_FLUSHABLE flag
      // BT_POWER? gets FLAG_FORCE_ACTIVE flag
      CheckPHY(side);

      m_device->AddSide(path, side);
   }
   void OnRemoveDevice(const std::string& path)
   {
      auto it = m_sides.find(path);
      if (it != m_sides.end())
      {
         g_info("Removing %s", it->second->Description().c_str());
         m_sides.erase(it);
         m_device->RemoveSide(path);
      }
   }

   void FeederThreadDeadline()
   {
      auto buffer = asha::Buffer::Create([this](const RawS16& s) { return m_device->SendAudio(s); });

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
            size_t new_dropped = buffer->RingDropped();
            size_t new_failed = buffer->FailedWrites();
            size_t new_silence = buffer->Silence();
            std::cout << "Ring Occupancy: " << buffer->Occupancy()
                << " High: " << buffer->OccupancyHigh()
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
            RawS16* samples = buffer->NextBuffer();
            if (!samples)
               break;

            if (!m_data_left.empty())
               memcpy(samples->l, m_data_left.data() + m_data_offset, MIN_SAMPLES_BYTES);
            if (!m_data_right.empty())
               memcpy(samples->r, m_data_right.data() + m_data_offset, MIN_SAMPLES_BYTES);
            
            buffer->SendBuffer();
            
            m_data_offset += MIN_SAMPLES_BYTES;
            if (!m_data_left.empty() && m_data_offset + MIN_SAMPLES_BYTES > m_data_left.size())
               m_data_offset = 0;
            else if (!m_data_right.empty() && m_data_offset + MIN_SAMPLES_BYTES > m_data_right.size())
               m_data_offset = 0;
            pos += ASHA_PACKET_TIME;
         }
      }
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

   
   std::map<std::string, std::shared_ptr<asha::Side>> m_sides;

   volatile bool m_running = false;
   std::thread m_thread;

   std::shared_ptr<asha::Device> m_device;
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
