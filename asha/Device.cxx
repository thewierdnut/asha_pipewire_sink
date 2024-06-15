#include "Device.hh"

#include "Buffer.hh"
#include "../pw/Stream.hh"
#include "../pw/Thread.hh"
#include "Side.hh"
#include <glib.h>

using namespace asha;

Device::Device(uint64_t hisync, const std::string& name, const std::string& alias, ReconnectCallback cb):
   m_hisync{hisync},
   m_name{name},
   m_alias{alias},
   m_buffer{new Buffer<RING_BUFFER_SIZE>([this](const RawS16& samples) { return SendAudio(samples); })},
   m_reconnect_cb{cb}
{
   auto lock = pw::Thread::Get()->Lock();
   m_stream = std::make_shared<pw::Stream>(
      name, alias,
      [this]() { Connect(); Start(); },
      [this]() { Stop(); Disconnect(); },
      [this]() {  },
      [this]() {  },
      [this](const RawS16& samples) {
         // TODO: redesign this api so that we can retrieve the pointer and
         //       have the pipewire stream fill it in.
         auto buffer = m_buffer->NextBuffer();
         if (buffer)
         {
            *buffer = samples;
            m_buffer->SendBuffer();
         }
      }
   );
}

Device::~Device()
{
   auto lock = pw::Thread::Get()->Lock();
   m_stream.reset();
   m_sides.clear();
}


size_t Device::Occupancy() const
{
   return m_buffer->Occupancy();
}


size_t Device::OccupancyHigh() const
{
   return m_buffer->OccupancyHigh();
}


size_t Device::RingDropped() const
{
   return m_buffer->RingDropped();
}


size_t Device::FailedWrites() const
{
   return m_buffer->FailedWrites();
}


size_t Device::Silence() const
{
   return m_buffer->Silence();
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
      if (!side->Connect())
      {
         g_error("Failed to connect to %s", side->Description().c_str());
         continue;
      }
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
   }
}


// Called when a stream starts receiving data.
void Device::Start()
{
   // Already holding pipewire thread lock.

   // Rate means bit/sec telephone bandwidth, not sample rate. 64000
   // just means "Use all 8 bits of each byte".
   g722_encode_init(&m_state_left, 64000, G722_PACKED);
   g722_encode_init(&m_state_right, 64000, G722_PACKED);

   // Asha docs says otherstate here is "whether the other side of the
   // binaural devices is connected", but the android source checks if
   // the other side is *streaming* (ie connected, and already started
   m_buffer->Start();

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
   m_buffer->Stop();
   if (m_state == STREAMING)
      for (auto& kv: m_sides)
         kv.second->Stop();
   m_state = PAUSED;
}


// Called whenever another 320 samples are ready.
bool Device::SendAudio(const RawS16& samples)
{
   bool ready = false;
   for (auto& kv: m_sides)
   {
      if (kv.second->Ready())
      {
         ready = true;
         break;
      }
   }
   if (!ready)
      return false;

   bool success = false;
   if (m_state == STREAMING)
   {
      AudioPacket left;
      AudioPacket right;
      left.seq = right.seq = m_audio_seq;

      bool left_encoded = false;
      bool right_encoded = false;

      for (auto& kv: m_sides)
      {
         if (!kv.second->Ready())
            continue;
         if (kv.second->Right())
         {
            if (!right_encoded)
            {
               g722_encode(&m_state_right, right.data, samples.r, samples.SAMPLE_COUNT);
               right_encoded = true;
            }
         }
         else
         {
            if (!left_encoded)
            {
               g722_encode(&m_state_left, left.data, samples.l, samples.SAMPLE_COUNT);
               left_encoded = true;
            }
         }

         Side::WriteStatus status = kv.second->WriteAudioFrame(kv.second->Right() ? right : left);
         if (status == Side::DISCONNECTED)
            m_reconnect_cb(kv.first);
         else
            success |= status == Side::WRITE_OK;
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
   if (m_state == STREAMING)
   {
      Stop();

      // side->UpdateConnectionParameters(0x10);
      if (side->Connect())
         m_sides.emplace_back(path, side);
      else
         g_error("Failed to connect to %s", side->Description().c_str());

      Start();
   }
}


// Called when an asha bluetooth device is removed.
bool Device::RemoveSide(const std::string& path)
{
   // Called from dbus thread, needs to hold pw lock while modifying m_sides.
   auto lock = pw::Thread::Get()->Lock();
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


// Called when an asha bluetooth device is removed.
bool Device::Reconnect(const std::string& path)
{
   // Called from dbus thread
   auto lock = pw::Thread::Get()->Lock();
   auto it = m_sides.begin();
   for (; it != m_sides.end(); ++it)
   {
      if (it->first == path)
      {
         bool otherstate = false;
         for (auto& side: m_sides)
         {
            if (side.second == it->second)
               continue;
            side.second->UpdateOtherConnected(false);
            otherstate |= side.second->Ready();
         }
         it->second->Stop();
         it->second->Reconnect();
         it->second->Start(otherstate);

         for (auto& side: m_sides)
         {
            if (side.second == it->second)
               continue;
            side.second->UpdateOtherConnected(true);
         }
         return true;
      }
   }
   return false;
}