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

static RawS16 SILENCE = {};


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
   typedef std::function<bool(const RawS16&, bool&, bool&)> DataCallback;

   Buffer(DataCallback cb):m_data_cb{cb}
   {
      
   }
   ~Buffer()
   {
      Stop();
   }

   void Start()
   {
      if (!m_thread.joinable())
      {
         m_running = true;
         m_thread = std::thread(&Buffer::DeliveryThread, this);
      }
   }

   void Stop()
   {
      if (m_thread.joinable())
      {
         m_running = false;
         m_thread.join();
      }
   }

   size_t Occupancy() const { return m_occupancy; }
   size_t OccupancyHigh() const { return m_high_occupancy; }
   size_t RingDropped() const { return m_buffer_full.load(std::memory_order_relaxed); }
   size_t Retries() const { return m_retries; }

   RawS16* NextBuffer()
   {
      // ...RxxW...
      size_t idx = m_write.load(std::memory_order_relaxed);
      size_t read = m_read.load(std::memory_order_acquire);

      if (idx - read >= RING_SIZE)
      {
         m_buffer_full.fetch_add(1, std::memory_order_relaxed);
         return nullptr;
      }
      m_send_status[idx & (RING_SIZE-1)] = std::make_pair(false, false);
      return &m_buffer[idx & (RING_SIZE-1)];
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
      pthread_setname_np(m_thread.native_handle(), "buffer_encode");

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
               auto& status = m_send_status[idx & (RING_SIZE-1)];
               m_data_cb(buffer, status.first, status.second);

               if (status.first && status.second)
               {
                  m_read.fetch_add(1, std::memory_order_relaxed);
                  next += INTERVAL;
               }
               else
               {
                  // One or the other failed. Do not increment the read pointer
                  // and re-attempt delivery quickly.
                  if (!status.first)
                     ++m_retries;
                  if (!status.second)
                     ++m_retries;
                  next += INTERVAL / 20;
               }

               // If we ran out of space in the ring waiting for the hearing
               // devices, then just clear out the ring to keep from
               // accumulating too much latency.
               if (m_occupancy == RING_SIZE)
               {
                  m_read = write;
                  m_buffer_full.fetch_add(RING_SIZE - 1, std::memory_order_relaxed);
               }
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
               bool left = false;
               bool right = false;
               if (!m_data_cb(SILENCE, left, right))
                  ++m_retries;
               next += INTERVAL;
            }
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
   std::atomic<size_t> m_read{};
   size_t m_retries = 0;
   size_t m_occupancy = 0;
   size_t m_high_occupancy = 0;
   uint8_t m_padding1[64 - 2 * sizeof(std::atomic<size_t>)];
   std::atomic<size_t> m_write{};
   std::atomic<size_t> m_buffer_full{};
   uint8_t m_padding2[64];

   // We use a sentry entry that is always unused, so that we can distinguish
   // between a full ring and an empty ring. This means that technically we
   // only have room for RING_SIZE - 1 buffers.
   RawS16 m_buffer[RING_SIZE];
   std::pair<bool, bool> m_send_status[RING_SIZE];
};

}