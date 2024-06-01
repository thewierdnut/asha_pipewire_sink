#pragma once

#include "Bluetooth.hh"
#include "Device.hh"

#include <cstdint>
#include <map>
#include <string>
#include <vector>


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

   // std::vector<Device> Devices() const;
   // bool Ready() const;
   // void SelectDevice(uint64_t id);
   // const Device& SelectedDevice() const;

   // void Process(int timeout_ms);

protected:
   void OnAddDevice(const Bluetooth::BluezDevice& d);
   void OnRemoveDevice(const std::string& path);

private:
   std::shared_ptr<Bluetooth> m_b;
   std::map<uint64_t, std::shared_ptr<Device>> m_devices;
};

}