#pragma once


#include "AudioPacket.hh"

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace asha
{

// The tested asha devices don't like the irregular packet delivery caused by
// the mismatch between the frame sizes between pipewire and asha. This class
// is designed to have a minimal 2~3 frame buffer and its own delivery thread
// that can regularly space out the frame delivery.
// TODO: If this class is going to contain a ring buffer anyways, we might as
//       well preemptively retrieve its buffers to fill them in directly,
//       instead of using mass memcpy's to transfer data.
template <size_t RING_SIZE>
class Buffer final
{
public:
   static_assert((RING_SIZE & (RING_SIZE - 1)) == 0, "RING_SIZE must be a power of two");
   static_assert(RING_SIZE > 1, "RING_SIZE must be at least 2");
   typedef std::function<bool(AudioPacket&, AudioPacket&)> DataCallback;

   Buffer(DataCallback cb):m_data_cb{cb}
   {
      m_running = true;
      m_thread = std::thread(&Buffer::DeliveryThread, this);
   }
   ~Buffer()
   {
      if (m_thread.joinable())
      {
         m_running = false;
         m_thread.join();
      }
   }

   size_t Occupancy() const { return m_occupancy; }
   size_t OccupancyHigh() const { return m_high_occupancy; }
   size_t RingDropped() const { return m_buffer_full; }
   size_t FailedWrites() const { return m_failed_writes; }

   std::pair<AudioPacket*, AudioPacket*> NextBuffer()
   {
      // ...RxxW...
      size_t idx = m_write.load(std::memory_order_relaxed);
      size_t read = m_read.load(std::memory_order_acquire);

      if (idx - read + 1 >= RING_SIZE) // Mind the sentry entry.
      {
         ++m_buffer_full;
         return {nullptr, nullptr};
      }
      auto& buffer = m_buffer[idx & (RING_SIZE-1)];
      return {&buffer.left, &buffer.right};
   }

   void SendBuffer()
   {
      assert(m_write >= m_read);
      ++m_write;
   }

protected:
   static uint64_t Now()
   {
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      return ts.tv_sec * 1000000000 + ts.tv_nsec;
   }

   void DeliveryThread()
   {
      // Need to deliver a packet every 20 ms. Wake up every 5 ms and check for
      // one.
      static const struct timespec SLEEP_INTERVAL{0, 5000000};
      static constexpr uint64_t INTERVAL = 20000000;
      uint64_t next = Now() + INTERVAL;
      while (m_running)
      {
         uint64_t now = Now();
         if (now > next)
         {
            // ...RxxW...
            size_t idx = m_read.load(std::memory_order_relaxed);
            size_t write = m_write.load(std::memory_order_acquire);
            m_occupancy = write - idx;
            if (m_occupancy > m_high_occupancy)
               m_high_occupancy = m_occupancy;
            if (write > idx)
            {
               auto& buffer = m_buffer[idx & (RING_SIZE-1)];
               if (!m_data_cb(buffer.left, buffer.right))
                  ++m_failed_writes;
               // TODO: if the send fails, should we drain the buffer, just to
               //       improve the odds on catching back up?
               ++m_read;
            }
            else
            {
               // Buffer was empty. This isn't necessarily unexpected, as
               // pipewire will stop streaming data if nobody is producing it.
               // TODO: should we continue to stream silence? My hearing aids
               //       tend to shut off one side for some reason if there is
               //       no more data, and then it takes it about a second to
               //       start playing data again when it arrives, leaving gaps
               //       in the audio. Its also possible that we have somehow
               //       overtaken pipewire, and it may be better to just skip
               //       the packet to allow the hearing devices to drain their
               //       buffers and catch up.
            }
            next += INTERVAL;
         }
         else
         {
            // Try and sleep until the next packet is needed. If this sleep
            // isn't reliable enough, we can switch to sleeping for 5ms or so.
            struct timespec sleep_interval {0, (uint32_t)(next - now)};
            nanosleep(&SLEEP_INTERVAL, nullptr);
         }
      }
   }

private:
   DataCallback m_data_cb;

   volatile bool m_running = false;
   std::thread m_thread;

   // Use padding to force reader/writer vars to be on their own cache lines.
   uint8_t m_padding0[64 - 3 * sizeof(size_t) - sizeof(std::atomic<size_t>)];
   std::atomic<size_t> m_read;
   size_t m_failed_writes = 0;
   size_t m_occupancy = 0;
   size_t m_high_occupancy = 0;
   uint8_t m_padding1[64 - sizeof(size_t) - sizeof(std::atomic<size_t>)];
   std::atomic<size_t> m_write;
   size_t m_buffer_full = 0;
   uint8_t m_padding2[64];

   // We use a sentry entry that is always unused, so that we can distinguish
   // between a full ring and an empty ring. This means that technically we
   // only have room for RING_SIZE - 1 buffers.
   struct {
      AudioPacket left;
      AudioPacket right;
   } m_buffer[RING_SIZE];
};

}