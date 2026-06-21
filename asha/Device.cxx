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
   m_state = UNINITIALIZED;
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
   if (!SidesAreAll(Side::STREAMING))
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
         kv.second->SetMicrophoneVolume(v);
   }
}


// Called when a new asha bluetooth device is detected and connected.
void Device::AddSide(const std::string& path, const std::shared_ptr<Side>& side)
{
   // We shouldn't have received a side until after it has already connected to
   // the PSM
   assert(side->State() == Side::STOPPED);

   g_info("Adding %s device to %s", side->Left() ? "left" : "right", Name().c_str());
   side->SubscribeExtra();

   bool otherstate = !m_sides.empty();

   {
      std::lock_guard<std::mutex> lock(m_sides_mutex);
      m_sides.emplace_back(path, side);
   }

   std::weak_ptr<Side> ws = side;
   std::weak_ptr<Device> wt = std::dynamic_pointer_cast<Device>(shared_from_this());

   switch(m_state)
   {
   case UNINITIALIZED:
      // First device. Transition to STOPPED.
      assert(m_sides.size() == 1);
      m_state = STOPPED;
      break;
   case STOPPED:
      // Second device. Stay in STOPPED, but inform the others.
      assert(!m_sides.empty());
      for (auto& kv: m_sides) // Is this necessary?
      {
         if (kv.second != side)
            kv.second->UpdateOtherConnected(true);
      }
      break;
   case START_STREAMING:
      // We were requested to start audio streaming already, but the first
      // device hasn't responded yet. Go ahead and request the second device
      // start as well.
      side->Start(true, [ws, wt](bool status){
         auto t = wt.lock();
         if (t)
            t->OnStarted(ws, status);
      });
      break;
   case STREAMING:               // Already running. Need to shut down and restart.
      m_state = START_STREAMING;
      for (auto& kv: m_sides)
      {
         if (kv.second == side)
            continue;
         std::weak_ptr<Side> other = kv.second;
         kv.second->Stop([wt, other](bool status){
            auto t = wt.lock();
            if (t)
               t->OnRestart(other, status);
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
   case UNINITIALIZED:        // Nothing was happening.
      // In theory, we can't be in the stopped state unless we already had no devices.
      assert(0 && "Removed device while already in the stopped state. This shouldn't be possible.");
      break;
   case STOPPED:              // No streaming audio when it was removed.
      if (m_sides.empty())
         m_state = UNINITIALIZED;
      for (auto& kv: m_sides) // Is this necessary?
         kv.second->UpdateOtherConnected(false);
      break;
   case START_STREAMING:
      // TODO: Treating these as the same for now with a fallthrough... but we
      //       may want to instead wait until the remaining device begins
      //       streaming before we issue a stop and reset.
   case STREAMING:       // Device was disconnected while we were streaming.
      if (m_sides.empty())
         m_state = UNINITIALIZED;
      else
      {
         // Should we leave the existing devices streaming? I don't have the
         // greatest luck with that :(
         m_state = START_STREAMING;
         for (auto& kv: m_sides)
         {
            std::weak_ptr<Side> other = kv.second;
            std::weak_ptr<Device> wt = std::dynamic_pointer_cast<Device>(shared_from_this());
            // If we are just going to re-issue the start and indicate that
            // the other side isn't connected, this probably isn't necessary.
            kv.second->UpdateOtherConnected(false);

            kv.second->Stop([other, wt](bool success) {
               auto t = wt.lock();
               if (t)
                  t->OnRestart(other, success);
            });
         }
      }

      break;
   }

   return true;
}


Side* Device::Left()
{
   for (auto& s: m_sides)
   {
      if (s.second->Left())
      {
         return s.second.get();
      }
   }
   return nullptr;
}


Side* Device::Right()
{
   for (auto& s: m_sides)
   {
      if (s.second->Right())
      {
         return s.second.get();
      }
   }
   return nullptr;
}

// Called when a device acknowledges the start command, or it failed.
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
   
   assert(m_state == START_STREAMING);

   if (status)
   {
      if (SidesAreAll(Side::STREAMING))
      {
         Start();
         m_state = STREAMING;
      }
   }
   else
   {
      // Failed. Keep trying again until bluez invalidates the device.
      bool otherstate = m_sides.size() > 1;
      std::weak_ptr<Device> wt = std::dynamic_pointer_cast<Device>(shared_from_this());
      auto ws = side;
      s->Start(otherstate, [wt, ws](bool success){
         auto self = wt.lock();
         if (self)
            self->OnStarted(ws, success);
      });
   }
}


void Device::OnRestart(const std::weak_ptr<Side>& side, bool status)
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

   if (SidesAreAll(Side::STOPPED))
   {
      m_state = START_STREAMING;
      // Since the device is still valid, reinitialize it.
      bool otherstate = m_sides.size() > 1;
      for (auto& side: m_sides)
      {
         std::weak_ptr<Side> ws = side.second;
         std::weak_ptr<Device> wt = std::dynamic_pointer_cast<Device>(shared_from_this());
         side.second->Start(otherstate, [wt, ws](bool success){
            auto self = wt.lock();
            if (self)
               self->OnStarted(ws, success);
         });
      }
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

   // Once the devices are all stopped, transition to stopped so that we can
   // start again.
   if (SidesAreAll(Side::STOPPED))
   {
      Stop();
   }
}


void Device::Start()
{
   // Rate means bit/sec telephone bandwidth, not sample rate. 64000
   // just means "Use all 8 bits of each byte".

   g722_encode_init(&m_state_left, 64000, G722_PACKED);
   g722_encode_init(&m_state_right, 64000, G722_PACKED);
   m_audio_seq = 0;
   m_state = STREAMING;
   ProcessDeferred();
}

void Device::Stop()
{
   // Called once all sides have transitioned to stop.
   m_state = STOPPED;
   ProcessDeferred();
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



// void Device::OnConnect()
// {
//    g_info("Ignoring Connect event");
// }
//
// void Device::OnDisconnect()
// {
//    g_info("Ignoring Disconnect event");
// }


void Device::StartCallback(const std::shared_ptr<Side>& s, bool status)
{
   if (!s)
      return;

   if (status)
   {
      OnStarted(s, status);
   }
   else
   {
      // TODO: Cooldown?
      // Try again, call back here when finished.
      std::weak_ptr<Device> wt = std::dynamic_pointer_cast<Device>(shared_from_this());
      std::weak_ptr<Side> ws = s;

      s->Start(m_sides.size() > 1, [wt, ws](bool status) {
         auto t = wt.lock();
         if (t)
            t->StartCallback(ws.lock(), status);
      });
   }
}


void Device::StreamStart()
{
   g_info("Requesting Stream Start");
   // Called when we get a stream stop event from pipewire.
   if (m_state == STOPPED && m_deferred_state_changes.empty())
   {
      StreamStartImpl();
   }
   else
   {
      g_info("Deferring request stream start, because we are in state %s", StateStr());
      m_deferred_state_changes.push_back([this](){StreamStartImpl();});
   }
}

void Device::StreamStartImpl()
{
   std::weak_ptr<Device> wt = std::dynamic_pointer_cast<Device>(shared_from_this());
   m_state = START_STREAMING;
   bool otherstate = m_sides.size() > 1;
   for (auto& kv: m_sides)
   {
      std::weak_ptr<Side> ws = kv.second;
      kv.second->Start(otherstate, [wt, ws](bool status) {
         auto t = wt.lock();
         if (t)
            t->StartCallback(ws.lock(), status);
      });
   }
}

void Device::StreamStop()
{
   g_info("Requesting Stream Stop");
   // Called when we get a stream start event from pipewire.
   if (m_state != STOPPED && m_deferred_state_changes.empty())
   {
      StreamStopImpl();
   }
   else
   {
      g_info("Deferring stream stop, because we are in state %s", StateStr());
      m_deferred_state_changes.push_back([this](){StreamStop();});
   }
}

void Device::StreamStopImpl()
{
   std::weak_ptr<Device> wt = std::dynamic_pointer_cast<Device>(shared_from_this());
   m_state = UNINITIALIZED;
   for (auto& kv: m_sides)
   {
      std::weak_ptr<Side> ws = kv.second;
      kv.second->Stop([wt, ws](bool status)
      {
         if (status)
         {
            // True here means that we successfully sent the message. We don't
            // wait for a status update, because some hearing aids won't
            // respond to it anyways, and android doesn't wait either.
            auto self = wt.lock();
            if (self)
               self->OnStop(ws, status);
         }
      });
   }
}

void Device::ProcessDeferred()
{
   if (!m_deferred_state_changes.empty())
   {
      auto next = m_deferred_state_changes.front();
      m_deferred_state_changes.erase(m_deferred_state_changes.begin());
      g_info("Initiating deferred state change");
      next();
   }
}
