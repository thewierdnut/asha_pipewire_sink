#pragma once

#include "AudioPacket.hh"
#include "Buffer.hh"

#include <atomic>
#include <cassert>
#include <thread>

namespace asha
{

// No buffering. Just forward the packet immediately.
class BufferNone: public Buffer
{
public:
   BufferNone(DataCallback cb):Buffer(cb) {}
   virtual ~BufferNone() override {}

   virtual void Start() override {}
   virtual void Stop() override {}
   virtual RawS16* NextBuffer() override { return &m_buffer; }
   virtual void SendBuffer() override
   {
      if (!m_data_cb(m_buffer))
         ++m_failed_writes;
   }

private:
   RawS16 m_buffer;
};

}
