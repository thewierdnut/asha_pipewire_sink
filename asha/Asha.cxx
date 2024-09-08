#include "Asha.hh"
#include "Bluetooth.hh"
#include "Buffer.hh"
#include "RawHci.hh"
#include "Side.hh"
#include "../pw/Stream.hh"

#include <cassert>
#include <glib.h>
#include <set>

using namespace asha;

namespace
{
   static constexpr uint32_t DEFER_INTERVAL_MS = 50; // How long to delay between deferred items.
}


Asha::Asha()
{
   RawHci hci;
   RawHci::SystemConfig config;
   // Rather than reading this config from the file, pull it out of the kernel,
   // so that we know what is actually set.
   if (hci.ReadSysConfig(config))
   {
      if (config.max_conn_interval != config.min_conn_interval)
      {
         g_error("Your configured MinConnectionInterval (%hu) and MaxConnectionInterval (%hu) are not the same. "
                 "You need to adjust your /etc/bluetooth/main.conf file and restart the bluetooth service.",
                 config.min_conn_interval, config.max_conn_interval);
      }
      else if (config.min_conn_interval > 16)
      {
         g_error("Your configured MinConnectionInterval and MaxConnectionInterval are not set to 16. "
                 "Please fix your /etc/bluetooth/main.conf and restart the bluetooth service.");
      }
      else if (config.min_conn_interval < 16)
      {
         g_info("The connection interval is set to %hu, and is unlikely to be supported. "
                "If you encounter distorted audio or an unstable connection, it is probably "
                "best to set it back to 16", config.min_conn_interval);
      }
   }


   m_b.reset(new Bluetooth(
      [this](const Bluetooth::BluezDevice& d) { OnAddDevice(d); },
      [this](const std::string& path) { OnRemoveDevice(path); }
   ));
}

Asha::~Asha()
{

}


size_t Asha::Occupancy() const
{
   size_t ret = 0;
   for (auto& kv: m_devices)
      ret += kv.second.buffer->Occupancy();
   return ret;
}


size_t Asha::OccupancyHigh() const
{
   size_t ret = 0;
   for (auto& kv: m_devices)
   {
      if (ret < kv.second.buffer->OccupancyHigh())
         ret = kv.second.buffer->OccupancyHigh();
   }
   return ret;
}


size_t Asha::RingDropped() const
{
   size_t ret = 0;
   for (auto& kv: m_devices)
      ret += kv.second.buffer->RingDropped();
   return ret;
}


size_t Asha::FailedWrites() const
{
   size_t ret = 0;
   for (auto& kv: m_devices)
      ret += kv.second.buffer->FailedWrites();
   return ret;
}


size_t Asha::Silence() const
{
   // Summing these together has the side effect that if we remove a side, then
   // when we add them together as if they were a single counter, then we get
   // a counter value that appears to have gone backwards, and can print
   // garbage to the screen.
   size_t ret = 0;
   for (auto& kv: m_devices)
      ret += kv.second.buffer->Silence();
   return ret;
}



void Asha::OnAddDevice(const Bluetooth::BluezDevice& d)
{
   // Called when we get a new device.
   auto side = Side::CreateIfValid(d);
   if (side)
   {
      std::weak_ptr<Side> weak_side = side;
      std::string path = d.path;
      m_sides.emplace_back(path, side);

      // TODO: Do we need to handle a timeout in case the device never becomes
      //       ready? For now, I'm assuming that a timeout means a missing
      //       device, and bluez will call OnRemoveDevice later.
      side->SetOnConnectionReady([this, path, weak_side](){
         g_debug("Connection Ready Callback Called");

         auto side = weak_side.lock();
         if (side)
         {
            SideReady(path, side);
         }
      });
   }
}


void Asha::OnRemoveDevice(const std::string& path)
{
   // We don't know which device has the bluetooth device, but in all
   // likelyhood, there will only be one anyways, so just check them
   // all.
   for (auto it = m_devices.begin(); it != m_devices.end(); ++it)
   {
      if (it->second.device->RemoveSide(path))
      {
         if (it->second.device->SideCount() == 0)
         {
            // Both left and right sides have disconnected, so erase the
            // device. This will also remove it from the set of available
            // pipewire sinks.
            g_info("Removing Sink %lu %s", it->first, it->second.device->Name().c_str());
            m_devices.erase(it);
         }
         break;
      }
   }

   for (auto it = m_sides.begin(); it != m_sides.end(); ++it)
   {
      if (it->first == path)
      {
         m_sides.erase(it);
         break;
      }
   }
}


void Asha::SideReady(const std::string& path, const std::shared_ptr<Side>& side)
{
   assert(side->State() == Side::STOPPED);
   g_debug("Side ready: %s", side->Description().c_str());
   

   auto& props = side->GetProperties();

   g_info("Name:      %s", side->Name().c_str());
   g_info("    HiSyncId %lu", props.hi_sync_id);
   if (side->Name() != side->Alias())
      g_info("    Alias:     %s", side->Alias().c_str());
   g_info("    Side:      %s %s",
      ((props.capabilities & 0x01) ? "right" : "left"),
      ((props.capabilities & 0x02) ? "(binaural)" : "(monaural)"));
   g_info("    Delay:     %hu ms", props.render_delay);
   g_info("    Streaming: %s", (props.feature_map & 0x01 ? "supported" : "not supported" ));
   g_info("    Codecs:    %s", (props.codecs & 0x02 ? "G.722" : "" ));
   
   // Insert, or find the existing one.
   auto it = m_devices.find(props.hi_sync_id);
   if (it == m_devices.end())
   {
      auto device = std::make_shared<Device>(side->Name());
      auto buffer = Buffer::Create([device](const RawS16& samples) {
          return device->SendAudio(samples);
      });
      auto stream = std::make_shared<pw::Stream>(
         "asha_"+std::to_string(props.hi_sync_id), side->Name(),
         []() { },
         []() { },
         []() { },
         []() { },
         [buffer](const RawS16& samples) {
            // TODO: redesign this api so that we can retrieve the pointer and
            //       have the pipewire stream fill it in.
            auto p = buffer->NextBuffer();
            if (p)
            {
               *p = samples;
               buffer->SendBuffer();
            }
         }
      );
      it = m_devices.emplace(props.hi_sync_id, Pipeline{device, buffer, stream}).first;
      g_info("Adding Sink %lu %s", it->first, side->Name().c_str());
   }

   it->second.device->AddSide(path, side);
}