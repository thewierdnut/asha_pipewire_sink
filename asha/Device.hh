#pragma once


#include <cstdint>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>
#include <string>

#include "AudioPacket.hh"

namespace pw {
   class Stream;
   class Thread;
}

namespace asha {

class Side;

// Manage a pair of hearing devices.
class Device final
{
public:
   Device(uint64_t hisync, const std::string& name, const std::string& alias);
   ~Device();

   const std::string& Name() const { return m_name; }
   const std::string& Alias() const { return m_alias; }

   size_t SideCount() const { return m_sides.size(); }

protected:
   // These will be called by the asha management singleton. (main thread)
   void AddSide(const std::string& path, const std::shared_ptr<Side>& side);
   bool RemoveSide(const std::string& path);

   // These will be called by the pipewire stream. (pipewire thread)
   void Connect();
   void Disconnect();
   void Start();
   void Stop();
   bool SendAudio(AudioPacket& left, AudioPacket& right);
   void SetStreamVolume(bool left, int8_t v);
   void SetDeviceVolume(bool left, int8_t v);

private:
   uint64_t m_hisync;
   const std::string m_name;
   const std::string m_alias;
   enum {DISCONNECTED, CONNECTED, PAUSED, STREAMING} m_state = DISCONNECTED;

   // Access to this needs to be guarded by the pipewire thread lock.
   std::vector<std::pair<std::string, std::shared_ptr<Side>>> m_sides;

   std::shared_ptr<pw::Stream> m_stream;
   uint8_t m_audio_seq = 0;
   int8_t m_volume = -60;

   int m_skip_packets = 0; // Used to drain the packets to synchronize sides.

   friend class Asha;
};



}