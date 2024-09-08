#pragma once

#include "Bluetooth.hh"
#include "Device.hh"

#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <deque>
#include <functional>

namespace pw { class Stream; }


namespace asha
{

class Buffer;

class Asha
{
public:
   Asha();
   ~Asha();

   size_t Occupancy() const;
   size_t OccupancyHigh() const;
   size_t RingDropped() const;
   size_t FailedWrites() const;
   size_t Silence() const;

protected:
   void OnAddDevice(const Bluetooth::BluezDevice& d);
   void OnRemoveDevice(const std::string& path);

   void SideReady(const std::string& path, const std::shared_ptr<Side>& side);

private:
   // Maintain a reference to m_sides as long as it is valid
   std::vector<std::pair<std::string, std::shared_ptr<Side>>> m_sides;
   std::shared_ptr<Bluetooth> m_b;

   struct Pipeline
   {
      // In this order so that they will destruct in in the proper order
      std::shared_ptr<Device> device;        // ASHA audio output device.
      std::shared_ptr<Buffer> buffer;        // Buffer algorithm used to queue audio.
      std::shared_ptr<pw::Stream> stream;    // Pipewire stream to produce audio.
   };
   std::map<uint64_t, Pipeline> m_devices;
};

}
