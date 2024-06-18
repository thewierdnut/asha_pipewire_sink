#include "Asha.hh"
#include "Bluetooth.hh"
#include "RawHci.hh"
#include "Side.hh"

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
      ret += kv.second->Occupancy();
   return ret;
}


size_t Asha::OccupancyHigh() const
{
   size_t ret = 0;
   for (auto& kv: m_devices)
   {
      if (ret < kv.second->OccupancyHigh())
         ret = kv.second->OccupancyHigh();
   }
   return ret;
}


size_t Asha::RingDropped() const
{
   size_t ret = 0;
   for (auto& kv: m_devices)
      ret += kv.second->RingDropped();
   return ret;
}


size_t Asha::FailedWrites() const
{
   size_t ret = 0;
   for (auto& kv: m_devices)
      ret += kv.second->FailedWrites();
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
      ret += kv.second->Silence();
   return ret;
}



void Asha::OnAddDevice(const Bluetooth::BluezDevice& d)
{
   // Called when we get a new device.
   auto side = Side::CreateIfValid(d);
   if (side && side->ReadProperties())
   {
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
         it = m_devices.emplace(props.hi_sync_id, std::make_shared<Device>(
            props.hi_sync_id,
            side->Name(),
            side->Alias(),
            [this](const std::string& path) { OnReconnectDevice(path); }
         )).first;
         g_info("Adding Sink %lu %s", it->first, it->second->Name().c_str());
      }
      auto local_path = d.path;
      auto local_side_ptr = side;
      auto local_device = it->second;
      Defer([=]() {
         local_device->AddSide(local_path, local_side_ptr);
      });
   }
}


void Asha::OnRemoveDevice(const std::string& path)
{
   // We don't know which device has the bluetooth device, but in all
   // likelyhood, there will only be one anyways, so just check them
   // all.
   // This is deferred so that we don't have race conditions during creation,
   // since the creation is also deferred.
   auto local_path = path;
   Defer([=]() {
      for (auto it = m_devices.begin(); it != m_devices.end(); ++it)
      {
         if (it->second->RemoveSide(local_path))
         {
            if (it->second->SideCount() == 0)
            {
               // Both left and right sides have disconnected, so erase the
               // device. This will also remove it from the set of available
               // pipewire sinks.
               g_info("Removing Sink %lu %s", it->first, it->second->Name().c_str());
               m_devices.erase(it);
            }
            break;
         }
      }
   });
}


void Asha::OnReconnectDevice(const std::string& path)
{
   // We don't know which audio device has the bluetooth device, but in all
   // likelyhood, there will only be one anyways, so just check them all.
   auto local_path = path;
   Defer([=]() {
      for (auto it = m_devices.begin(); it != m_devices.end(); ++it)
      {
         it->second->Stop();
         usleep(10000);
         it->second->Start();
      }
   });
}


void Asha::Defer(std::function<void()> fn)
{
   std::lock_guard<std::mutex> lock(m_async_queue_mutex);
   m_async_queue.emplace_back(fn);
   if (m_async_queue.size() == 1)
   {
      g_timeout_add(DEFER_INTERVAL_MS, [](void* user_data) {
         return ((Asha*)user_data)->ProcessDeferred();
      }, this);
   }
}

int Asha::ProcessDeferred()
{
   std::lock_guard<std::mutex> lock(m_async_queue_mutex);

   m_async_queue.front()();
   m_async_queue.pop_front();
   return m_async_queue.empty() ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
}