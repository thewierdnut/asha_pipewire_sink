#include "Device.hh"

#include "../pw/Stream.hh"
#include "../pw/Thread.hh"
#include "Side.hh"
#include <glib.h>

using namespace asha;

Device::Device(uint64_t hisync, const std::string& name, const std::string& alias):
   m_hisync{hisync},
   m_name{name},
   m_alias{alias}
{
   m_stream = std::make_shared<pw::Stream>(
      name, alias,
      [this]() { Connect(); Start(); },
      [this]() { Stop(); Disconnect(); },
      [this]() { /* Start(); */ }, // I'm seeing about a one second latency before
      [this]() { /* Stop(); */ },  // I actually hear audio, so lets start immediately.
      [this](AudioPacket& l, AudioPacket& r) { SendAudio(l, r); }
   );
}

Device::~Device()
{

}

// Called by pipewire when a node has been connected. (Since it is a stream
// object, this will happen immediately, as it attaches a conversion node in
// front of it).
void Device::Connect()
{
   // Already holding pipewire thread lock.
   m_state = CONNECTED;
   for (auto& kv: m_sides)
   {
      auto& side = kv.second;
      // TODO: Investigate whether this is really needed. I think the answer is
      //       yes-ish, unless we are configured with the correct parameters in
      //       /etc/bluetooth/main.conf
      // side->UpdateConnectionParameters(0x10);
      if (!side->Connect())
      {
         g_error("Failed to connect to %s", side->Description().c_str());
         continue;
      }
      // side->UpdateConnectionParameters(0x10); // Ditto.

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
}


// Called when node is disconnected. I don't actually ever expect this to get
// called, as our node will probably have a conversion node in front of it.
void Device::Disconnect()
{
   // Already holding pipewire thread lock.
   m_state = DISCONNECTED;
   Stop();
   for (auto& kv: m_sides)
   {
      if (!kv.second->Disconnect())
      {
         g_error("Failed to disconnect from %s", kv.second->Description().c_str());
         continue;
      }

      // Doc says to do this, but android hci captures don't show this
      // happening...
      // Inform the others
      // for (size_t j = 0; j < m_current_device->devices.size(); ++j)
      // {
      //    if (i == j)
      //       continue;
      //    auto& side = m_current_device->devices[j];
      //    side->StatusUpdateOtherConnected(false);
      // }
   }
}


// Called when a stream starts receiving data.
void Device::Start()
{
   // Already holding pipewire thread lock.

   // Asha docs says otherstate here is "whether the other  side of the
   // binaural devices is connected", but the android source checks if
   // the other side is *streaming* (ie connected, and already started

   m_state = STREAMING;
   m_audio_seq = 0;
   for (auto& kv: m_sides)
   {
      bool otherstate = false;
      for (auto& okv: m_sides)
      {
         if (kv.second == okv.second) continue;
         otherstate |= okv.second->Ready();
      }
      kv.second->Start(m_sides.size() > 1);
   }
}


// Called when a stream stops receiving data.
void Device::Stop()
{
   // Already holding pipewire thread lock.
   if (m_state == STREAMING)
      for (auto& kv: m_sides)
         kv.second->Stop();
   m_state = PAUSED;
}


// Called whenever another 160 bytes of g722 audio is ready.
bool Device::SendAudio(AudioPacket& left, AudioPacket& right)
{
   // Already holding pipewire thread lock.
   bool success = false;
   if (m_state == STREAMING)
   {
      left.seq = right.seq = m_audio_seq;
      for (auto& kv: m_sides)
      {
         if (!kv.second->Ready())
            continue;
         if (kv.second->Left())
            success |= kv.second->WriteAudioFrame(left);
         else
            success |= kv.second->WriteAudioFrame(right);
      }
      if (success)
         ++m_audio_seq;
   }
   return success;
}


// Called when audio property is adjusted.
void Device::SetStreamVolume(bool left, int8_t v)
{
   // Already holding pipewire thread lock.
   m_volume = v;
   for (auto& kv: m_sides)
   {
      if (left == kv.second->Left())
         kv.second->SetStreamVolume(v);
   }
}


// Called when audio property is adjusted.
void Device::SetDeviceVolume(bool left, int8_t v)
{
   // Already holding pipewire thread lock.
   m_volume = v;
   for (auto& kv: m_sides)
   {
      if (left == kv.second->Left())
         kv.second->SetDeviceVolume(v);
   }
}


// Called when a new asha bluetooth device is detected.
void Device::AddSide(const std::string& path, const std::shared_ptr<Side>& side)
{
   g_info("Adding %s device to %s", side->Left() ? "left" : "right", Name().c_str());
   // Called from dbus thread, needs to hold pw lock while modifying m_sides.
   auto lock = pw::Thread::Get()->Lock();

   m_sides.emplace_back(path, side);
   if (m_state == CONNECTED || m_state == PAUSED || m_state == STREAMING)
   {
      g_info("   Stream already running. Connecting new device.");
      // side->UpdateConnectionParameters(0x10);
      if (!side->Connect())
      {
         g_error("Failed to connect to %s", side->Description().c_str());
         return;
      }
      // side->UpdateConnectionParameters(0x10); // Ditto.

      if (m_state == PAUSED || m_state == STREAMING)
      {
         bool otherstate = false;
         for (auto& kv: m_sides)
         {
            if (side == kv.second) continue;
            otherstate |= kv.second->Ready();
         }
         side->Start(otherstate);
      }
      for (auto& s: m_sides)
      {
         if (s.second != side)
            s.second->UpdateOtherConnected(true);
      }
   }
}


// Called when an asha bluetooth device is removed.
bool Device::RemoveSide(const std::string& path)
{
   // Called from dbus thread, needs to hold pw lock while modifying m_sides.
   auto it = m_sides.begin();
   for (; it != m_sides.end(); ++it)
   {
      if (it->first == path)
      {
         auto lock = pw::Thread::Get()->Lock();
         m_sides.erase(it);

         for (auto& side: m_sides)
            side.second->UpdateOtherConnected(false);
         return true;
      }
   }
   return false;
}