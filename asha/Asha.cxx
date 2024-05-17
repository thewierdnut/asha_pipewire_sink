#include "Asha.hh"
#include "Bluetooth.hh"

#include <glib.h>
#include <set>

using namespace asha;


std::vector<Asha::Device> Asha::Devices() const
{
   std::vector<Asha::Device> ret;
   for (auto& kv: m_supported_devices)
   {
      ret.emplace_back(Device{kv.first, kv.second.name});
   }
   return ret;
}


void Asha::SelectDevice(uint64_t id)
{
   // TODO: clean up any old devices selected
   if (m_selected_id)
   {
      // TODO: If playing, stop

      // Close the data channels.
      auto it = m_supported_devices.find(m_selected_id);
      for (auto& side: it->second.devices)
         side->Disconnect();
      for (auto& side: it->second.devices)
         side->UpdateOtherConnected(false);
   }

   m_selected_id = 0;
   m_current_device = nullptr;
   auto it = m_supported_devices.find(id);
   if (it == m_supported_devices.end())
   {
      g_error("Unsupported device: %lu", id);
      return;
   }

   m_selected_id = id;
   m_current_device = &it->second;

   for (size_t i = 0; i < m_current_device->devices.size(); ++i)
   {
      auto& side = m_current_device->devices[i];

      // Set the connection parameters. 0x10 is 20ms (160 bytes of data per packet)
      side->UpdateConnectionParameters(0x10);

      if (!side->Connect())
      {
         g_error("Failed to connect to %s", side->Description().c_str());
         continue;
      }

      side->EnableStatusNotifications();

      // usleep(100000);

      // Doc says to do this, but android hci captures don't show this
      // happening...
      // Inform the others
      // for (size_t j = 0; j < m_current_device->devices.size(); ++j)
      // {
      //    if (i == j)
      //       continue;
      //    auto& side = m_current_device->devices[j];
      //    side->StatusUpdateOtherConnected(true);
      // }
   }

   // What I really want to do here is wait for the connection updates, but the
   // kernel does not appear to expose this anywhere we can wait for the event
   // or poll for the connection parameters.
   // My btmon captures seem to indicate it takes sometimes 700 ms to get the
   // LE Connection Update Complete event. Lets pause a full second here, then
   // our caller can trigger a connection parameter status update to both
   // sides.
   usleep(1000000);
   for (auto& side: m_current_device->devices)
      side->UpdateConnectionParameters(0x10);
}


void Asha::Start()
{
   for (auto& side: m_current_device->devices)
      side->Start(m_current_device->devices.size() > 1);
   m_audio_seq = 0;
}

void Asha::Stop()
{
   bool first = false;
   for (auto& side: m_current_device->devices)
      side->Stop();
}

bool Asha::SendAudio(uint8_t* left, uint8_t* right, size_t size)
{
   bool success = true;
   for (auto& side: m_current_device->devices)
   {
      if (right && side->Right())
         success = side->WriteAudioFrame(right, size, m_audio_seq) && success;
      if (left && side->Left())
         success = side->WriteAudioFrame(left, size, m_audio_seq) && success;
   }
   ++m_audio_seq;
   return success;
}

void Asha::SetVolume(int8_t v)
{
   for (auto& side: m_current_device->devices)
      side->SetVolume(v);
}


void Asha::EnumerateDevices()
{
   Bluetooth b;
   b.Create();
   b.EnumerateDevices();
   m_supported_devices.clear();

   std::map<uint64_t, std::shared_ptr<Side>> m_devices;
   for (auto& device: b.Devices())
   {
      auto side = Side::CreateIfValid(device);
      if (side && side->ReadProperties())
      {
         auto& device_set = m_supported_devices[side->GetProperties().hi_sync_id];
         device_set.alias = side->Alias();
         device_set.name = side->Name();
         device_set.devices.emplace_back(std::move(side));
      }
   }

   if (m_supported_devices.empty())
   {
      g_warning("No supported devices found");
   }
   else
   {
      for (auto& sd: m_supported_devices)
      {
         g_info("HiSyncId %lu", sd.first);
         for (auto& side: sd.second.devices)
         {
            g_info("  Name:      %s", side->Name().c_str());
            if (side->Name() != side->Alias())
               g_info("    Alias:     %s", side->Alias().c_str());
            g_info("    Side:      %s %s",
                   ((side->GetProperties().capabilities & 0x01) ? "right" : "left"),
                   ((side->GetProperties().capabilities & 0x02) ? "(binaural)" : "(monaural)"));
            g_info("    Delay:     %hu ms", side->GetProperties().render_delay);
            g_info("    Streaming: %s", (side->GetProperties().feature_map & 0x01 ? "supported" : "not supported" ));
            g_info("    Codecs:    %s", (side->GetProperties().codecs & 0x02 ? "G.722" : "" ));
         }
      }
   }
}