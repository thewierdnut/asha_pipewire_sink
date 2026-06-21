#include "BufferTimed.hh"
#include "Now.hh"
#include "DeviceInterface.hh"

using namespace asha;

namespace
{
   const RawS16 SILENCE{};
}

void BufferTimed::SendBuffer()
{
   uint64_t t = Now();
   auto device = m_device.lock();
   if (device)
   {
      if (t - m_stamp > ASHA_STREAM_DEPTH)
      {
         // We have missed 8 packets, so the stream should be empty. Preload with
         // six packets of silence, then send the available audio as the sixth.
         for (size_t i = 0; i < 6; ++i)
         {
            if (device->SendAudio(SILENCE))
               ++m_silence;
            else
               break;
         }
      }
      m_stamp = t;

      if (!device->SendAudio(m_buffer))
         ++m_failed_writes;
   }
}

void BufferTimed::StreamStart()
{
   auto device = m_device.lock();
   if (device)
   {
      device->StreamStart();
   }
};

void BufferTimed::StreamStop()
{
   auto device = m_device.lock();
   if (device)
   {
      device->StreamStop();
   }
};
