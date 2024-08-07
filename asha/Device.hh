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
class Device final
{
public:
   Device(uint64_t hisync, const std::string& name, const std::string& alias);
   ~Device();

   const std::string& Name() const { return m_name; }
   const std::string& Alias() const { return m_alias; }

   size_t SideCount() const { return m_sides.size(); }

   size_t Occupancy() const;
   size_t OccupancyHigh() const;
   size_t RingDropped() const;
   size_t FailedWrites() const;
   size_t Silence() const;

protected:
   // These will be called by the asha management singleton. (main thread)
   void AddSide(const std::string& path, const std::shared_ptr<Side>& side);
   bool RemoveSide(const std::string& path);

   // These will be called by the pipewire stream. (pipewire thread)
   void Connect();
   void Disconnect();
   void Start();
   void Stop();
   bool SendAudio(const RawS16& samples);
   void SetStreamVolume(bool left, int8_t v);
   void SetExternalVolume(bool left, int8_t v);

private:
   uint64_t m_hisync;
   const std::string m_name;
   const std::string m_alias;
   enum {DISCONNECTED, CONNECTED, PAUSED, STREAMING} m_state = DISCONNECTED;


   std::shared_ptr<Buffer> m_buffer;
   g722_encode_state_t m_state_left{};
   g722_encode_state_t m_state_right{};

   // Access to this needs to be guarded by the pipewire thread lock.
   std::vector<std::pair<std::string, std::shared_ptr<Side>>> m_sides;

   std::shared_ptr<pw::Stream> m_stream;
   uint8_t m_audio_seq = 0;
   int8_t m_volume = -60;

   friend class Asha;
};



}
