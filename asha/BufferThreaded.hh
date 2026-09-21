#pragma once

#include "AudioPacket.hh"
#include "Buffer.hh"
#include "DeviceInterface.hh"
#include "Now.hh"

#include <atomic>
#include <cassert>
#include <functional>
#include <thread>

#include <glib.h>
#include <pthread.h>

namespace asha
{

// The tested asha devices don't like the irregular packet delivery caused by
// the mismatch between the frame sizes between pipewire and asha. This class
// is designed to have a minimal 2~3 frame buffer and its own delivery thread
// that can regularly space out the frame delivery. If either side fails to
// write a packet, then we will drop a frame from both sides to try and let it
// catch up. If there is no packet ready, then we will write silence to keep
// the stream running.
template <size_t RING_SIZE>
class BufferThreaded: public Buffer
{
public:
   static_assert((RING_SIZE & (RING_SIZE - 1)) == 0, "RING_SIZE must be a power of two");
   static_assert(RING_SIZE > 1, "RING_SIZE must be at least 2");

   BufferThreaded(const std::shared_ptr<DeviceInterface>& d):
      Buffer(d)
   {
   }

   virtual ~BufferThreaded() override
   {
      if (m_running)
      {
         Stop();
         auto device = m_device.lock();
         if (device)
            device->StreamStop();
      }
   }

   virtual RawS16* NextBuffer() override
   {
      // ...RxxW...
      size_t idx = m_write.load(std::memory_order_relaxed);
      size_t read = m_read.load(std::memory_order_acquire);

      if (idx - read >= RING_SIZE)
      {
         __atomic_fetch_add(&m_buffer_full, 1, __ATOMIC_RELAXED);
         return nullptr;
      }
      return &m_buffer[idx & (RING_SIZE-1)];
   }

   virtual void SendBuffer() override
   {
      assert(m_write >= m_read);
      assert(m_write < m_read + RING_SIZE);
      ++m_write;
   }

   virtual void StreamStart() override
   {
      if (!m_running)
      {
         auto device = m_device.lock();
         if (device)
         {
            device->StreamStart();
            Start();
         }
      }
   }

   virtual void StreamStop() override
   {
      // Ignore. We only stop the stream when we quit.
   }

protected:
   void Start()
   {
      if (!m_thread.joinable())
      {
         g_info("Starting asynchronous buffer thread");
         m_startup = true;
         m_running = true;
         m_thread = std::thread(&BufferThreaded::DeliveryThread, this);
         pthread_setname_np(m_thread.native_handle(), "buffer_encode");
      }
   }

   void Stop()
   {
      if (m_thread.joinable())
      {
         g_info("Stopping asynchronous buffer thread");

         m_running = false;
         m_thread.join();
      }
   }

   void DeliveryThread()
   {
      // Need to deliver a packet every 20 ms. Wake up every 5 ms and check for
      // one.
      static const struct timespec SLEEP_INTERVAL{0, 5000000};
      static constexpr uint64_t INTERVAL = 20000000;
      static const RawS16 SILENCE = {};

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
               // Make sure we fill up our ring at least halfway before
               // starting, so that we can fill the buffers on the hearing
               // devices.
               if (m_startup)
               {
                  if (m_occupancy < RING_SIZE)
                     continue;
                  m_startup = false;
                  // Flush all available packets to start up.
                  auto device = m_device.lock();
                  if (device)
                  {
                     for (; idx < write; ++idx)
                     {
                        auto& buffer = m_buffer[idx & (RING_SIZE-1)];

                        if (device->SendAudio(buffer))
                        {
                           ++m_failed_writes;
                           if (write > idx + 1)
                              __atomic_fetch_add(&m_buffer_full, 1, __ATOMIC_RELAXED);
                           break;
                        }
                     }
                  }
                  m_read = idx;
               }
               else
               {
                  auto device = m_device.lock();
                  if (device)
                  {
                     auto& buffer = m_buffer[idx & (RING_SIZE-1)];
                     if (!device->SendAudio(buffer))
                     {
                        ++m_failed_writes;
                        // If we failed to send a packet, drop an extra from input
                        if (write > idx + 1)
                        {
                           ++m_read;
                           __atomic_fetch_add(&m_buffer_full, 1, __ATOMIC_RELAXED);
                        }
                     }
                     ++m_read;
                  }
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
               auto device = m_device.lock();
               if (device)
               {
                  if (!device->SendAudio(SILENCE))
                     ++m_failed_writes;
                  ++m_silence;
               }
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
   bool m_startup = true;
   volatile bool m_running = false;
   std::thread m_thread;

   // Use padding to force reader/writer vars to be on their own cache lines.
   uint8_t m_padding0[64 - 4 * sizeof(std::atomic<size_t>)];
   std::atomic<size_t> m_read{};
   uint8_t m_padding1[64 - 2 * sizeof(std::atomic<size_t>)];
   std::atomic<size_t> m_write{};
   uint8_t m_padding2[64];

   RawS16 m_buffer[RING_SIZE];
};

}