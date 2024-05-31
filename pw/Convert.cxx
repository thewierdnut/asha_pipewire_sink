#include "Convert.hh"
#include "../g722/g722_enc_dec.h"

#include <cassert>
#include <cstring>


Convert::Convert()
{
   Reset();
}


void Convert::Reset()
{
   // Rate means bit/sec telephone bandwidth, not sample rate. 64000
   // just means "Use all 8 bits of each byte".
   g722_encode_init(&m_state_left, 64000, G722_PACKED);
   g722_encode_init(&m_state_right, 64000, G722_PACKED);
   m_buffer_occupancy = 0;
}


void Convert::In(const int16_t* left, const int16_t* right, size_t samples, std::function<void(AudioPacket&, AudioPacket&)> outfn)
{
   // Always need an even number of samples. If we have an odd number, then
   // cache it for next time.
   int16_t left_buffer[samples + 1];
   int16_t right_buffer[samples + 1];
   if (m_leftovers)
   {
      left_buffer[0] = m_leftover_left;
      right_buffer[0] = m_leftover_right;
      memcpy(&left_buffer[1], left, samples * 2);
      memcpy(&right_buffer[1], right, samples * 2);
      left = left_buffer;
      right = right_buffer;
      ++samples;
      m_leftovers = false;
   }
   if (samples % 2)
   {
      m_leftover_left = left[samples - 1];
      m_leftover_right = right[samples - 1];
      --samples;
      m_leftovers = true;
   }

   // We need to emit 320 samples/160 bytes at a time, but pipewire tends to
   // to feed us 340 samples at a time. Cache the extra 20 until we eventually
   // have enough to emit two frames instead of one.
   for(size_t sample = 0; sample < samples;)
   {
      size_t room_in_buffer = AudioPacket::MAX_SIZE_BYTES - m_buffer_occupancy;
       // Two input samples per output byte, but don't process more than we have.
      size_t samples_to_process = std::min(room_in_buffer * 2, samples - sample);
      g722_encode(&m_state_left, m_buffer_left.data + m_buffer_occupancy, left + sample, samples_to_process);
      g722_encode(&m_state_right, m_buffer_right.data + m_buffer_occupancy, right + sample, samples_to_process);
      m_buffer_occupancy += samples_to_process / 2;
      sample += samples_to_process;
      if (m_buffer_occupancy < AudioPacket::MAX_SIZE_BYTES)
         break; // We didn't produce a full buffer. Cache for later.
      outfn(m_buffer_left, m_buffer_right);
      m_buffer_occupancy = 0;
   }
}
