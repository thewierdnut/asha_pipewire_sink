#pragma once

#include "AudioPacket.hh"
#include "Buffer.hh"
#include "DeviceInterface.hh"

#include <atomic>
#include <cassert>
#include <thread>

namespace asha
{

// No buffering. Just forward the packet immediately.
class BufferNone: public Buffer
{
public:
   BufferNone(const std::shared_ptr<DeviceInterface>& d):Buffer(d) {}
   virtual ~BufferNone() override {}

   virtual RawS16* NextBuffer() override { return &m_buffer; }
   virtual void SendBuffer() override
   {
      auto device = m_device.lock();
      if (device)
      {
         if (!device->SendAudio(m_buffer))
            ++m_failed_writes;
      }
   }

   virtual void StreamStart() override
   {
      auto device = m_device.lock();
      if (device)
      {
         device->StreamStart();
      }
   };

   virtual void StreamStop() override
   {
      auto device = m_device.lock();
      if (device)
      {
         device->StreamStop();
      }
   };

private:
   RawS16 m_buffer;
};

}
