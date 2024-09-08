#pragma once

#include "AudioPacket.hh"
#include "Buffer.hh"

#include <atomic>
#include <cassert>
#include <thread>

namespace asha
{

// Measure the gap between packets if its more than 160ms, then buffer three
// packets of silence before sending the traffic.
class BufferTimed: public Buffer
{
public:
   BufferTimed(DataCallback cb):Buffer(cb) {}
   virtual ~BufferTimed() override {}

   virtual RawS16* NextBuffer() override { return &m_buffer; }
   virtual void SendBuffer() override;

private:
   RawS16 m_buffer;
   uint64_t m_stamp = 0;
};

}
