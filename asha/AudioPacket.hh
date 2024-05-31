#pragma once

#include <cstdint>
#include <cstddef>

struct AudioPacket
{
   static constexpr size_t MAX_SIZE_BYTES = 160;
   static constexpr size_t MAX_SIZE_SAMPLES = MAX_SIZE_BYTES * 2;

   uint8_t seq;
   uint8_t data[160];
} __attribute__((packed));