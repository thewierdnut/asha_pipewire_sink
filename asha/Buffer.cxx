#include "Buffer.hh"

#include "Config.hh"
#include "BufferNone.hh"
#include "BufferThreaded.hh"
#include "BufferPoll.hh"
#include "BufferTimed.hh"

#include <stdexcept>

#include <glib.h>


using namespace asha;

// Create the appropriate derived class based on the user config.
std::shared_ptr<Buffer> Buffer::Create(const std::shared_ptr<DeviceInterface>& d)
{
   switch (Config::BufferAlgorithm())
   {
   case Config::NONE:
      g_info("Buffer Algorithm: NONE");
      return std::make_shared<BufferNone>(d);
   case Config::THREADED:
      g_info("Buffer algorithm: THREADED");
      return std::make_shared<BufferThreaded>(d);
   case Config::POLL4:
      g_info("Buffer algorithm: POLL4");
      return std::make_shared<BufferPoll<4>>(d);
   case Config::POLL8:
      g_info("Buffer algorithm: POLL8");
      return std::make_shared<BufferPoll<8>>(d);
   case Config::TIMED:
      g_info("Buffer algorithm: TIMED");
      return std::make_shared<BufferTimed>(d);
   default:
      throw std::logic_error("Missing buffer algorithm from factory.");
   }
}