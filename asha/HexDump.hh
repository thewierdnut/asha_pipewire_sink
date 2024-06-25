#pragma once

#include <cstdint>
#include <cstddef>
#include <iomanip>
#include <ostream>

inline void HexDump(std::ostream& out, const uint8_t* p, size_t len)
{
   auto oldflags = out.flags();
   for (ssize_t i = 0; i < len; ++i)
   {
      if (i != 0) out << ' ';
      out << std::setfill('0') << std::setw(2) << std::hex << (uint32_t)p[i];
   }
   out.flags(oldflags);
}