#pragma once

#include "Characteristic.hh"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct _GDBusProxy;

namespace asha
{

// Abstraction of bluez managed objects interface.
// TODO: We could theoretically watch the properties-changed signal to see
//       when devices are attached and removed.
class Bluetooth final
{
public:
   Bluetooth();
   ~Bluetooth();

   bool Create();

   bool EnumerateDevices();

   struct Device
   {
      std::string path;
      std::string name;
      std::string alias;
      std::string mac;

      std::vector<Characteristic> characteristics;
   };

   const std::vector<Device>& Devices() const { return m_devices; }

private:
   std::shared_ptr<_GDBusProxy> m_bluez_objects;

   std::vector<Device> m_devices;
};

}