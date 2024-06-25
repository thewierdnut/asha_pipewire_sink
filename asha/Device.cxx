#include "Device.hh"

#include "Buffer.hh"
#include "../pw/Stream.hh"
#include "../pw/Thread.hh"
#include "Side.hh"

#include <poll.h>
#include <glib.h>

using namespace asha;

Device::Device(uint64_t hisync, const std::string& name, const std::string& alias):
   m_hisync{hisync},
   m_name{name},
   m_alias{alias},
   m_buffer{new Buffer<RING_BUFFER_SIZE>([this](const RawS16& samples) { return SendAudio(samples); })}
{
   auto lock = pw::Thread::Get()->Lock();
   m_stream = std::make_shared<pw::Stream>(
      name, alias,
      [this]() { Connect(); },
      [this]() { Disconnect(); },
      [this]() { Start(); /* m_buffer->FlushAndReset(); */ },
      [this]() { Stop(); },
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
   m_buffer->Stop();
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
}


// Called when node is disconnected. I don't actually ever expect this to get
// called, as our node will probably have a conversion node in front of it.
void Device::Disconnect()
{
   // Already holding pipewire thread lock.
   m_state = DISCONNECTED;
   Stop();
}


// Called when a stream starts receiving data.
void Device::Start()
{
   // Already holding pipewire thread lock.

   // Rate means bit/sec telephone bandwidth, not sample rate. 64000
   // just means "Use all 8 bits of each byte".
   g722_encode_init(&m_state_left, 64000, G722_PACKED);
   g722_encode_init(&m_state_right, 64000, G722_PACKED);
   m_buffer->Start();

   // Asha docs says otherstate here is "whether the other side of the
   // binaural devices is connected", but the android source checks if
   // the other side is *streaming* (ie connected, and already started
   m_state = STREAMING;
   m_audio_seq = 0;
   for (auto& kv: m_sides)
   {
      // This is what android does...
      // bool otherstate = false;
      // for (auto& okv: m_sides)
      // {
      //    if (kv.second == okv.second) continue;
      //    otherstate |= okv.second->Ready();
      // }
      // This seems more stable. The hearing devices seem to fight for control
      // of the stream for a moment if you don't just tell them beforehand that
      // they are both present.
      bool otherstate = m_sides.size() > 1;
      kv.second->Start(otherstate);
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
   bool ready = true;
   for (auto& kv: m_sides)
   {
      if (!kv.second->Ready())
         return false;
   }

   bool success = false;
   if (m_state == STREAMING)
   {
      AudioPacket left;
      AudioPacket right;
      left.seq = right.seq = m_audio_seq;

      bool left_encoded = false;
      bool right_encoded = false;

      // TODO: Also check for closed socket?
      struct pollfd fds[m_sides.size()];
      for (size_t i = 0; i < m_sides.size(); ++i)
      {
         fds[i] = pollfd{
            .fd = m_sides[i].second->Sock(),
            .events = POLLOUT
         };
      }
      if (m_sides.size() != poll(fds, m_sides.size(), 0))
      {
         return false;
      }

      for (auto& kv: m_sides)
      {
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
         switch(status)
         {
         case Side::WRITE_OK:
            success = true;
            break;
         case Side::DISCONNECTED:
            g_info("WriteAudioFrame returned DISCONNECTED");
            // m_reconnect_cb(kv.first);
            break;
         case Side::BUFFER_FULL:
            g_info("WriteAudioFrame returned BUFFER_FULL");
            break;
         case Side::NOT_READY:
            g_info("WriteAudioFrame returned NOT_READY");
            break;
         case Side::TRUNCATED:
            g_info("WriteAudioFrame returned TRUNCATED");
            break;
         case Side::OVERSIZED:
            g_info("WriteAudioFrame returned OVERSIZED");
            break;
         }
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
   side->SubscribeExtra();

   // Called from dbus thread, needs to hold pw lock while modifying m_sides.
   auto lock = pw::Thread::Get()->Lock();
   auto old_state = m_state;
   if (m_state == STREAMING)
      Stop();

   if (side->Connect())
   {
      for (auto& kv: m_sides)
      {
         kv.second->UpdateConnectionParameters(16); // TODO: get interval from the side that just connected and verify that they are the smame.
         kv.second->UpdateOtherConnected(true);
      }
      m_sides.emplace_back(path, side);
   }
   else
      g_error("Failed to connect to %s", side->Description().c_str());

   if (old_state == STREAMING)
      Start();
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
         // We need to stop the streaming thread so that it doesn't attempt
         // buffer delivery after we have deleted the side. First though, lets
         // remove it from m_sides so that we don't set a Stop() to it.
         std::shared_ptr<asha::Side> to_delete(std::move(it->second));
         m_sides.erase(it);

         auto old_state = m_state;
         if (m_state == STREAMING)
            Stop();

         for (auto& side: m_sides)
            side.second->UpdateOtherConnected(false);

         if (old_state == STREAMING)
            Start();
         return true;
      }
   }
   return false;
}