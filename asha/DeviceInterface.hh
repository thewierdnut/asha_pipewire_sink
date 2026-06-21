#pragma once

#include "AudioPacket.hh"
#include <memory>

// Bare interface to Device that can be used by the buffering algorithm to
// send data and trigger start and stops.
class DeviceInterface: public std::enable_shared_from_this<DeviceInterface>
{
public:
   virtual ~DeviceInterface() = default;
   virtual bool SendAudio(const RawS16& samples) = 0;
   virtual void StreamStart() = 0;
   virtual void StreamStop() = 0;
};