#include "BufferThreaded.hh"

#include "AudioPacket.hh"
#include "DeviceInterface.hh"

#include <atomic>
#include <cassert>
#include <functional>
#include <thread>

#include <glib.h>

using namespace asha;

namespace
{
   const RawS16 SILENCE = {};

   uint64_t Now()
   {
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      return ts.tv_sec * 1000000000 + ts.tv_nsec;
   }
}

BufferThreaded::BufferThreaded(const std::shared_ptr<DeviceInterface>& d):
   Buffer(d)
{
}

BufferThreaded::~BufferThreaded()
{
   if (m_running)
   {
      Stop();
      auto device = m_device.lock();
      if (device)
         device->StreamStop();
   }
}

void BufferThreaded::StreamStart()
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

void BufferThreaded::StreamStop()
{
   // Ignore. We only stop the stream when we quit.
}

void BufferThreaded::Start()
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


void BufferThreaded::Stop()
{
   if (m_thread.joinable())
   {
      g_info("Stopping asynchronous buffer thread");

      m_running = false;
      m_thread.join();
   }
}


RawS16* BufferThreaded::NextBuffer()
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

void BufferThreaded::SendBuffer()
{
   assert(m_write >= m_read);
   assert(m_write < m_read + RING_SIZE);
   ++m_write;
}


void BufferThreaded::DeliveryThread()
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
