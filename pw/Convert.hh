#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <memory>
#include <functional>

#include "../g722/g722_enc_dec.h"
#include "../asha/AudioPacket.hh"

// Class that converts PCM data from float to G.722@16Khz
class Convert
{
public:
   Convert();
   void Reset();

   // Input the number of float samples at the given sample rate.
   // Cals outfn for each 160 byte chunk ready to send.
   void In(const int16_t* left, const int16_t* right, size_t samples, std::function<void(AudioPacket&, AudioPacket&)> outfn);

private:
   g722_encode_state_t m_state_left{};
   g722_encode_state_t m_state_right{};
   size_t m_buffer_occupancy = 0;
   AudioPacket m_buffer_left;
   AudioPacket m_buffer_right;

   bool m_leftovers = false;
   int16_t m_leftover_left;
   int16_t m_leftover_right;
};