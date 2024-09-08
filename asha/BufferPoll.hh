#pragma once


#include "AudioPacket.hh"
#include "Buffer.hh"
#include "Now.hh"

#include <cstddef>
#include <cassert>
#include <iostream>

namespace asha
{

// This keeps a buffer of packets ready, and once it is full, it will keep
// sending traffic until epoll says that there are no more slots ready.
// This will rely on the socket's epoll to tell us if the sockets are in
// sync, but it requires us to intentionally keep the audio latency high.
template <size_t RING_SIZE>
class BufferPoll: public Buffer
{
public:
   static_assert((RING_SIZE & (RING_SIZE - 1)) == 0, "RING_SIZE must be a power of two");
   static_assert(RING_SIZE > 1, "RING_SIZE must be at least 2");

   BufferPoll(DataCallback cb): Buffer(cb) {}
   virtual ~BufferPoll() override {}

   virtual RawS16* NextBuffer() override
   {
      if (!m_startup)
         Flush();

      if (m_write - m_read >= RING_SIZE)
      {
         assert(!m_startup);
         ++m_buffer_full;
         return nullptr;
      }
      return &m_buffer[m_write & (RING_SIZE-1)];
   }

   virtual void SendBuffer() override
   {
      assert(m_write >= m_read);
      assert(m_write < m_read + RING_SIZE);
      static const RawS16 SILENCE{};
      ++m_write;

      // If we don't deliver any traffic for a while, kick back into startup
      // mode.
      uint64_t t = Now();
      if (t - m_stamp > ASHA_STREAM_DEPTH)
         m_startup = true;
      m_stamp = t;
      
      if (m_startup)
      {
         // Wait until the ring is full.
         if (m_write - m_read < RING_SIZE)
            return;

         m_startup = false;

         // Now that the ring is full, flush 6 packets of silence.
         for (size_t i = 0; i < 6; ++i)
         {
            if (!m_data_cb(SILENCE))
               return; // We know we can't send any real data, so quit;
            ++m_silence;
         }
      }
   }

private:
   void Flush()
   {
      m_occupancy = m_write - m_read;
      if (m_occupancy > m_high_occupancy)
         m_high_occupancy = m_occupancy;

      // Write however much traffic it lets you each time.
      while (m_write > m_read)
      {
         if (!m_data_cb(m_buffer[m_read & (RING_SIZE - 1)]))
            break;
         ++m_read;
      }
   }


   bool m_startup = true;
   uint64_t m_stamp = 0;

   size_t m_read{};
   size_t m_write{};
   RawS16 m_buffer[RING_SIZE];
};

}
