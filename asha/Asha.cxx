#include "Asha.hh"
#include "Bluetooth.hh"
#include "Side.hh"

#include <glib.h>
#include <set>

using namespace asha;

namespace
{
   static constexpr uint32_t DEFER_INTERVAL_MS = 10; // How long to delay between deferred items.
}


Asha::Asha()
{
   m_b.reset(new Bluetooth(
      [this](const Bluetooth::BluezDevice& d) { OnAddDevice(d); },
      [this](const std::string& path) { OnRemoveDevice(path); }
   ));
}

Asha::~Asha()
{

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
            side->Alias()
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


void Asha::Defer(std::function<void()> fn)
{
   // TODO: Its a cool idea, but this doesn't seem to be working.
   // TODO: Lock? I think this all happens in the main thread...
   m_async_queue.emplace_back(fn);
   if (m_async_queue.size() == 1)
   {
      g_timeout_add_once(DEFER_INTERVAL_MS, [](void* user_data) {
         ((Asha*)user_data)->ProcessDeferred();
      }, this);
   }
}

void Asha::ProcessDeferred()
{
   m_async_queue.front()();
   m_async_queue.pop_front();
   if (!m_async_queue.empty())
   {
      g_timeout_add_once(DEFER_INTERVAL_MS, [](void* user_data) {
         ((Asha*)user_data)->ProcessDeferred();
      }, this);
   }
}