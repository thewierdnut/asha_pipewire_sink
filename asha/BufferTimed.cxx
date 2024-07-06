#include "BufferTimed.hh"
#include "Now.hh"


using namespace asha;

namespace
{
   const RawS16 SILENCE{};
}

void BufferTimed::SendBuffer()
{
   uint64_t t = Now();
   if (t - m_stamp > ASHA_STREAM_DEPTH)
   {
      // We have missed 8 packets, so the stream should be empty. Preload with
      // six packets of silence, then send the available audio as the fifth.
      for (size_t i = 0; i < 6; ++i)
      {
         if (m_data_cb(SILENCE))
            ++m_silence;
         else
            break;
      }
   }
   m_stamp = t;

   if (!m_data_cb(m_buffer))
      ++m_failed_writes;
}