#pragma once

#include "Characteristic.hh"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <map>

struct _GDBusProxy;
struct _GVariantIter;

namespace asha
{

// Abstraction of bluez managed objects interface.
// TODO: We could theoretically watch the properties-changed signal to see
//       when devices are attached and removed.
class Bluetooth final
{
public:
   struct BluezDevice
   {
      std::string path;
      std::string name;
      std::string alias;
      std::string mac;

      bool connected = false;
      bool resolved = false;

      std::vector<Characteristic> characteristics;
   };

   typedef std::function<void(const BluezDevice&)> AddCallback;
   typedef std::function<void(const std::string&)> RemoveCallback;
   Bluetooth(const AddCallback& add, const RemoveCallback& remove);
   ~Bluetooth();

private:
   bool EnumerateDevices();
   void ProcessDevice(const std::string& path, struct _GVariantIter* property_dict);
   void ProcessDeviceProperty(BluezDevice& device, const char* key, struct _GVariant* value);
   void ProcessInterfaceAdd(const std::string& path, struct _GVariantIter* iface_dict);
   void ProcessInterfaceRemoved(const std::string& path, struct _GVariantIter* iface_dict);

   void PrepareAndAddDevice(BluezDevice& device);
   void ProcessCharacteristic(BluezDevice& device, const std::string& path, struct _GVariantIter* property_dict);

   void OnInterfaceAdded();

   std::shared_ptr<_GDBusProxy> m_bluez_objects;
   std::shared_ptr<_GDBusProxy> m_bluez_object_properties;
   std::map<std::string, std::shared_ptr<_GDBusProxy>> m_bluez_properties;

   std::map<std::string, BluezDevice> m_devices;

   uint64_t m_signal_id = -1;
   uint64_t m_properties_changed_id = -1;

   AddCallback m_add_cb;
   RemoveCallback m_remove_cb;
};

}