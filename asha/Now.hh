#pragma once


#include <time.h>
#include <cstdint>


// Retrieve the monotonic time in nanoseconds.
// This is preferred over std::chrono because std::chrono is absurdly slow
// for critical sections, and clock_gettime is (mostly) realtime safe on
// modern kernels.
inline uint64_t Now()
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec * 1000000000ull + ts.tv_nsec;
}


// The amount of time represented by a 160 byte packet.
static constexpr uint64_t ASHA_PACKET_TIME = 20000000;
// The amount of audio data that an ASHA device should be able to buffer.
static constexpr uint64_t ASHA_STREAM_DEPTH = ASHA_PACKET_TIME * 8;