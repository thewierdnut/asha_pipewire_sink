#pragma once


#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include <string>

#include "AudioPacket.hh"
#include "../g722/g722_enc_dec.h"

namespace pw {
   class Stream;
   class Thread;
}

namespace asha {

class Side;
class Buffer;

// Manage a pair of hearing devices.
class Device: public std::enable_shared_from_this<Device>
{
public:
   Device(const std::string& name);
   virtual ~Device();

   const std::string& Name() const { return m_name; }

   size_t SideCount() const { return m_sides.size(); }

   // These will be called by the asha management singleton. (main thread)
   void AddSide(const std::string& path, const std::shared_ptr<Side>& side);
   bool RemoveSide(const std::string& path);
   void SetStreamVolume(bool left, int8_t v);
   void SetExternalVolume(bool left, int8_t v);

   // These will be called by the pipewire stream. (pipewire thread)
   bool SendAudio(const RawS16& samples);

   enum AudioState{STOPPED, STREAM_INIT, STREAMING };
   AudioState State() const { return m_state; }

protected:
   // State management callbacks
   void OnStarted(const std::weak_ptr<Side>& side, bool success);
   void OnStop(const std::weak_ptr<Side>& side, bool success);

   void Start();
   void Stop();

   bool SidesAreAll(int state) const;

private:
   AudioState m_state = STOPPED;
   std::string m_name;

   g722_encode_state_t m_state_left{};
   g722_encode_state_t m_state_right{};

   // Accessed from both pipewire and main, but only modified from main thread.
   // This means main thread only needs to lock it when modifying.
   std::vector<std::pair<std::string, std::shared_ptr<Side>>> m_sides;
   std::mutex m_sides_mutex;

   std::shared_ptr<pw::Stream> m_stream;
   uint8_t m_audio_seq = 0;
   int8_t m_volume = -60;
};



}
