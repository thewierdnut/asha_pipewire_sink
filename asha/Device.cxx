#include "Device.hh"

#include "Buffer.hh"
#include "Buffer.hh"
#include "Side.hh"

#include <cassert>
#include <poll.h>
#include <glib.h>

using namespace asha;


Device::Device(const std::string& name):
   m_name(name)
{
   m_state = STOPPED;
}


Device::~Device()
{
   m_sides.clear();
}


// Called whenever another 320 samples are ready.
bool Device::SendAudio(const RawS16& samples)
{
   if (m_state != STREAMING)
      return false;

   // Called from pipewire thread, don't let sides disappear while we are using
   // them.
   std::lock_guard<std::mutex> lock(m_sides_mutex);
   if (!SidesAreAll(Side::READY))
      return false;

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

   AudioPacket packets[2];
   AudioPacket* left;
   AudioPacket* right;

   if (m_sides.size() == 1)
   {
      // Mix the two sides together.
      int16_t mono_samples[RawS16::SAMPLE_COUNT];
      for (size_t i = 0; i < RawS16::SAMPLE_COUNT; ++i)
         mono_samples[i] = ((int32_t)samples.l[i] + (int32_t)samples.r[i]) / 2;
      left = right = &packets[0];
      g722_encode(&m_state_left, left->data, mono_samples, samples.SAMPLE_COUNT);
   }
   else
   {
      left = &packets[0];
      right = &packets[1];

      g722_encode(&m_state_left, left->data, samples.l, samples.SAMPLE_COUNT);
      g722_encode(&m_state_right, right->data, samples.r, samples.SAMPLE_COUNT);
   }
   assert(left);
   assert(right);

   left->seq = right->seq = m_audio_seq;
   bool success = false;
   for (auto& kv: m_sides)
   {
      Side::WriteStatus status = kv.second->WriteAudioFrame(kv.second->Right() ? *right : *left);
      switch(status)
      {
      case Side::WRITE_OK:
         success = true;
         break;
      case Side::DISCONNECTED:
         g_info("WriteAudioFrame returned DISCONNECTED");
         // Kick to stopping state, and retry?
         break;
      case Side::BUFFER_FULL: // Shoudn't hit this, we already validated that we can write.
         g_info("WriteAudioFrame returned BUFFER_FULL");
         break;
      case Side::NOT_READY:   // Shouldn't hit this, we already validated Ready().
         g_info("WriteAudioFrame returned NOT_READY");
         break;
      case Side::TRUNCATED:   // This is just an O/S l2cap stack error.
         g_info("WriteAudioFrame returned TRUNCATED");
         break;
      case Side::OVERSIZED:   // This is just an O/S l2cap stack error.
         g_info("WriteAudioFrame returned OVERSIZED");
         break;
      }
   }
   if (success)
      ++m_audio_seq;

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
void Device::SetExternalVolume(bool left, int8_t v)
{
   // Already holding pipewire thread lock.
   m_volume = v;
   for (auto& kv: m_sides)
   {
      if (left == kv.second->Left())
         kv.second->SetExternalVolume(v);
   }
}


// Called when a new asha bluetooth device is detected and connected.
void Device::AddSide(const std::string& path, const std::shared_ptr<Side>& side)
{
   g_info("Adding %s device to %s", side->Left() ? "left" : "right", Name().c_str());
   side->SubscribeExtra();

   bool otherstate = !m_sides.empty();

   {
      std::lock_guard<std::mutex> lock(m_sides_mutex);
      m_sides.emplace_back(path, side);
   }

   std::weak_ptr<Side> ws = side;
   std::weak_ptr<Device> wt = weak_from_this();

   side->Start(otherstate, [ws,wt](bool status) {
      auto t = wt.lock();
      if (t)
         t->OnStarted(ws, status);
   });

   switch(m_state)
   {
   case STOPPED:                 // First one, get ready to fire it up.
      assert(m_sides.size() == 1);
      m_state = STREAM_INIT;
      break;
   case STREAM_INIT:             // Second device, already in correct state.
      break;
   case STREAMING:               // Already running. Need to shut down and restart.
      m_state = STREAM_INIT;
      for (auto& kv: m_sides)
      {
         if (kv.second == side)
            continue;
         std::weak_ptr<Side> other = kv.second;
         kv.second->Stop([wt, other](bool status){
            auto t = wt.lock();
            if (t)
               t->OnStop(other, status);
         });
      }
      break;
   }
}


// Called when an asha bluetooth device is removed.
bool Device::RemoveSide(const std::string& path)
{
   // Is this meant for us?
   auto it = m_sides.begin();
   for (; it != m_sides.end(); ++it)
   {
      if (it->first == path)
         break;
   }
   if (it == m_sides.end())
      return false;

   // Meant for us. Lock, and remove, but don't delete just yet.
   std::shared_ptr<Side> to_delete = it->second;
   {
      std::lock_guard<std::mutex> lock(m_sides_mutex);
      m_sides.erase(it);
   }

   // The side we are removing is no longer present, and will not respond to
   // any further bluetooth requests from us.

   switch(m_state)
   {
   case STOPPED:        // Nothing was happening.
      // In theory, we can't be in the stopped state unless we already had no devices.
      assert(0 && "Removed device while already in the stopped state. This shouldn't be possible.");
      break;
   case STREAM_INIT:    // Got removed on startup.
      if (m_sides.empty())
         Stop();
      else if (SidesAreAll(Side::READY))
         m_state = STREAMING;
      break;
   case STREAMING:      // Device was disconnected while we were streaming.
      if (m_sides.empty())
         Stop();
      else
      {
         m_state = STREAM_INIT;
         for (auto& kv: m_sides)
         {
            std::weak_ptr<Side> other = kv.second;
            std::weak_ptr<Device> wt = weak_from_this();
            kv.second->Stop([other, wt](bool success) {
               auto t = wt.lock();
               if (t)
                  t->OnStop(other, success);
            });
         }
      }

      break;
   }

   return true;
}


void Device::OnStarted(const std::weak_ptr<Side>& side, bool status)
{
   // Make sure this callback isn't stale.
   auto s = side.lock();
   if (!s)
      return;
   auto it = m_sides.begin();
   for (; it != m_sides.end(); ++it)
   {
      if (it->second == s)
         break;
   }
   if (it == m_sides.end())
      return;
   
   assert(m_state == STREAM_INIT);

   if (SidesAreAll(Side::READY))
   {
      Start();
      m_state = STREAMING;
   }
}


void Device::OnStop(const std::weak_ptr<Side>& side, bool status)
{
   // Make sure this callback isn't stale.
   auto s = side.lock();
   if (!s)
      return;
   auto it = m_sides.begin();
   for (; it != m_sides.end(); ++it)
   {
      if (it->second == s)
         break;
   }
   if (it == m_sides.end())
      return;

   assert(m_state == STREAM_INIT);

   // Since the device is still valid, reinitialize it.
   bool otherstate = m_sides.size() > 1;
   std::weak_ptr<Side> ws = s;
   std::weak_ptr<Device> wt = weak_from_this();
   s->Start(otherstate, [wt, ws](bool success){
      auto self = wt.lock();
      if (self)
         self->OnStarted(ws, success);
   });
}


void Device::Start()
{
   // Rate means bit/sec telephone bandwidth, not sample rate. 64000
   // just means "Use all 8 bits of each byte".
   if (SidesAreAll(Side::READY))
   {
      g722_encode_init(&m_state_left, 64000, G722_PACKED);
      g722_encode_init(&m_state_right, 64000, G722_PACKED);
      m_audio_seq = 0;
      m_state = STREAMING;
   }
}

void Device::Stop()
{
   assert(m_sides.empty());
   m_state = STOPPED;
}


bool Device::SidesAreAll(int state) const
{
   if (m_sides.empty())
      return state == Side::STOPPED;
   for (auto& kv: m_sides)
   {
      if (kv.second->State() != state)
         return false;
   }
   return true;
}