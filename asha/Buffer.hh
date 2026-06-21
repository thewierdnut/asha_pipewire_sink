#pragma once


#include "AudioPacket.hh"

#include <functional>
#include <memory>

class DeviceInterface;

namespace asha
{


// The tested ASHA-enabled devices don't all respond equally well to the same
// buffering algorithm. This is an abstract interface that allows us to
// implement multiple algorithms under the hood.
class Buffer
{
public:
   // Create a the appropriate derived class based on the user config.
   static std::shared_ptr<Buffer> Create(const std::shared_ptr<DeviceInterface>& d);
   virtual ~Buffer() { }

   virtual RawS16* NextBuffer() = 0;
   virtual void SendBuffer() = 0;
   virtual void StreamStart() = 0;
   virtual void StreamStop() = 0;

   size_t Occupancy() const { return m_occupancy; }
   size_t OccupancyHigh() const { return m_high_occupancy; }
   size_t RingDropped() const { return m_buffer_full; }
   size_t FailedWrites() const { return m_failed_writes; }
   size_t Silence() const { return m_silence; }

protected:
   Buffer(const std::shared_ptr<DeviceInterface>& d):m_device{d} {}

   std::weak_ptr<DeviceInterface> m_device;

   size_t m_failed_writes = 0;
   size_t m_occupancy = 0;
   size_t m_high_occupancy = 0;
   size_t m_silence = 0;
   size_t m_buffer_full = 0;
};

}
