#include "g722_enc_dec.h"

#include <iostream>


int main()
{
   g722_encode_state_t state{};
   g722_encode_init(&state, 64000, G722_PACKED);

   int16_t in_data[320];
   uint8_t out_data[160];
   while(std::cin)
   {
      int sample_count = 320;
      if (!std::cin.read((char*)in_data, sizeof(in_data)))
         sample_count = std::cin.gcount() / 2;

      int out_size = g722_encode(&state, out_data, in_data, sample_count);
      std::cout.write((char*)out_data, out_size);
   }
   return 0;
}