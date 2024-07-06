#include "Buffer.hh"

#include "Config.hh"
#include "BufferNone.hh"
#include "BufferThreaded.hh"
#include "BufferPoll.hh"
#include "BufferTimed.hh"

#include <stdexcept>

using namespace asha;

// Create a the appropriate derived class based on the user config.
std::shared_ptr<Buffer> Buffer::Create(std::function<bool(const RawS16&)> cb)
{
   switch (Config::BufferAlgorithm())
   {
   case Config::NONE:      return std::make_shared<BufferNone>(cb);
   case Config::THREADED:  return std::make_shared<BufferThreaded>(cb);
   case Config::POLL4:     return std::make_shared<BufferPoll<4>>(cb);
   case Config::POLL8:     return std::make_shared<BufferPoll<8>>(cb);
   case Config::TIMED:     return std::make_shared<BufferTimed>(cb);
   default:
      throw std::logic_error("Missing buffer algorithm from factory.");
   }
}