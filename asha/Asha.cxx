#include "Asha.hh"
#include "Bluetooth.hh"
#include "Side.hh"

#include <glib.h>
#include <set>

using namespace asha;


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
      it->second->AddSide(d.path, side);

   }
}


void Asha::OnRemoveDevice(const std::string& path)
{
   // We don't know which device has the bluetooth device, but in all
   // likelyhood, there will only be one anyways, so just check them
   // all.
   for (auto it = m_devices.begin(); it != m_devices.end(); ++it)
   {
      if (it->second->RemoveSide(path))
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
}