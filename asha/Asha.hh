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

// proprietary volume control?
//    service 7d74f4bd-c74a-4431-862c-cce884371592
//    characteristic 4603580D-3C15-4FEC-93BE-B86B243ADA64
//       Write value from 0x00 to 0xff

// "0000180d-0000-1000-8000-00805f9b34fb" heart rate?


class Asha
{
public:
   Asha();
   ~Asha();

   size_t Occupancy() const;
   size_t OccupancyHigh() const;
   size_t RingDropped() const;
   size_t Retries() const;
   size_t Silence() const;

protected:
   void OnAddDevice(const Bluetooth::BluezDevice& d);
   void OnRemoveDevice(const std::string& path);
   void OnReconnectDevice(const std::string& path);

   // Call something later. This is designed to allow the bluetooth stack time
   // to respond to events we post before we process the next step, since they
   // are processed as signals on the same thread.
   void Defer(std::function<void()> fn);
   int ProcessDeferred();

private:
   std::shared_ptr<Bluetooth> m_b;
   std::map<uint64_t, std::shared_ptr<Device>> m_devices;

   std::mutex m_async_queue_mutex;
   std::deque<std::function<void()>> m_async_queue;
};

}