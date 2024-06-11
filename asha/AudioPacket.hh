#pragma once

#include <cstdint>
#include <cstddef>

struct AudioPacket
{
   static constexpr size_t SIZE_BYTES = 160;

   uint8_t seq;
   uint8_t data[SIZE_BYTES];
} __attribute__((packed));

struct RawS16
{
   static constexpr size_t SAMPLE_COUNT = 320;
   int16_t l[SAMPLE_COUNT];
   int16_t r[SAMPLE_COUNT];
};