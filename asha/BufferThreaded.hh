#pragma once

#include "AudioPacket.hh"
#include "Buffer.hh"

#include <atomic>
#include <cassert>
#include <thread>

namespace asha
{


// The tested asha devices don't like the irregular packet delivery caused by
// the mismatch between the frame sizes between pipewire and asha. This class
// is designed to have a minimal 2~3 frame buffer and its own delivery thread
// that can regularly space out the frame delivery. If either side fails to
// write a packet, then we will drop a frame from both sides to try and let it
// catch up. If there is no packet ready, then we will write silence to keep
// the stream running.
class BufferThreaded: public Buffer
{
public:
   BufferThreaded(DataCallback cb):Buffer(cb) { Start(); }
   virtual ~BufferThreaded() override { Stop(); }

   void Start();
   void Stop();
   virtual RawS16* NextBuffer() override;
   virtual void SendBuffer() override;

protected:
   void DeliveryThread();

private:
   bool m_startup = true;
   volatile bool m_running = false;
   std::thread m_thread;

   // Use padding to force reader/writer vars to be on their own cache lines.
   uint8_t m_padding0[64 - 4 * sizeof(std::atomic<size_t>)];
   std::atomic<size_t> m_read{};
   uint8_t m_padding1[64 - 2 * sizeof(std::atomic<size_t>)];
   std::atomic<size_t> m_write{};
   uint8_t m_padding2[64];

   static constexpr size_t RING_SIZE = 4;
   static_assert((RING_SIZE & (RING_SIZE - 1)) == 0, "RING_SIZE must be a power of two");
   static_assert(RING_SIZE > 1, "RING_SIZE must be at least 2");
   RawS16 m_buffer[RING_SIZE];
};

}
