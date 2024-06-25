#pragma once

#include "Bluetooth.hh"
#include "Device.hh"

#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <deque>
#include <functional>


namespace asha
{

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

private:
   std::shared_ptr<Bluetooth> m_b;
   std::map<uint64_t, std::shared_ptr<Device>> m_devices;
};

}
