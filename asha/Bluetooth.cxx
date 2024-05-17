#include "Bluetooth.hh"
//#include "GVariantDump.hh"
#include "Characteristic.hh"


#include <iostream>
#include <memory>
#include <stdexcept>

#include <gio/gio.h>

using namespace asha;

namespace
{
   constexpr char BLUEZ_DEVICE[] = "org.bluez.Device1";
   static constexpr char GATT_SERVICE_UUID[]    = "0000fdf0-0000-1000-8000-00805f9b34fb";

   uint64_t g_next_notify_id = 0;
}



Bluetooth::Bluetooth()
{
}

Bluetooth::~Bluetooth()
{

}

bool Bluetooth::Create()
{
   GError* err = nullptr;
   m_bluez_objects.reset(g_dbus_proxy_new_for_bus_sync(
      G_BUS_TYPE_SYSTEM,
      G_DBUS_PROXY_FLAGS_NONE,
      nullptr,
      "org.bluez",
      "/",
      "org.freedesktop.DBus.ObjectManager",
      nullptr,
      &err
   ), g_object_unref);

   if (err)
   {
      g_error("Error getting dbus GetManagedObjects proxy: %s", err->message);
      g_error_free(err);
      return false;
   }

   // TODO: I can monitor properties-changed on this object to see when new
   //       devices are attached or removed.

   return EnumerateDevices();
}

bool Bluetooth::EnumerateDevices()
{
   GError* err = nullptr;
   std::shared_ptr<GVariant> result(g_dbus_proxy_call_sync(
      m_bluez_objects.get(),
      "GetManagedObjects",
      nullptr,
      G_DBUS_CALL_FLAGS_NONE,
      -1,
      nullptr,
      &err
   ), g_variant_unref);

   // std::cout << result.get() << '\n';

   if (err)
   {
      g_error("Error making org.bluez GetManagedObjects call: %s", err->message);
      g_error_free(err);
      return false;
   }

   // The result should have a signature of a{oa{sa{sv}}}. It should be full of
   // results that look like this:
   //    "/org/bluez/hci0/dev_MA_CA_DD_RE_SS_00": {
   //       "org.bluez.Device1": {
   //          "Address": "MA:CA:DD:RE:SS:00",
   //          "AddressType": "public",
   //          "Name": "Cool Device",
   //          "Alias": "Cool Device",
   //          "Paired": true,
   //          "Bonded": true,
   //          "Trusted": true,
   //          "Blocked": false,
   //          "LegacyPairing": false,
   //          "Connected": true,
   //          "UUIDs": [
   //             ...
   //             "0000fdf0-0000-1000-8000-00805f9b34fb", // <-- asha gatt service {0xfdf0}
   //          ],
   //          "Modalias": "bluetooth:vGIBBERISH",
   //          "Adapter": "/org/bluez/hci0",
   //          "ServicesResolved": true
   //       }
   //    },

   // Specifically, we are searching fo all the devices that have the asha
   // service uuid (0xfdf0), and then enumerating all of the characteristics
   // that go with those devices.

   m_devices.clear();

   // result is a tuple with one item. Unpack an array iterator from it.
   GVariantIter* it_object{};
   g_variant_get(result.get(), "(a{oa{sa{sv}}})", &it_object);
   std::shared_ptr<GVariantIter> pit_object(it_object, g_variant_iter_free);
   gchar* path{};
   GVariantIter* it_interface{};
   while (g_variant_iter_loop(it_object, "{oa{sa{sv}}}", &path, &it_interface))
   {
      gchar* interface{};
      GVariantIter* it_properties{};
      while (g_variant_iter_loop(it_interface, "{sa{sv}}", &interface, &it_properties))
      {
         if (g_str_equal(BLUEZ_DEVICE, interface))
         {
            gchar* key{};
            GVariant* value{};
            Device device;
            bool has_gatt = false;
            bool connected = false;
            while (g_variant_iter_loop(it_properties, "{sv}", &key, &value))
            {
               if (g_str_equal("Name", key))
               {
                  device.name = g_variant_get_string(value, nullptr);
               }
               else if (g_str_equal("Alias", key))
               {
                  device.alias = g_variant_get_string(value, nullptr);
               }
               else if (g_str_equal("Address", key))
               {
                  device.mac = g_variant_get_string(value, nullptr);
               }
               else if (g_str_equal("UUIDs", key))
               {
                  GVariantIter it{};
                  g_variant_iter_init(&it, value);
                  gchar* uuid{};
                  while (g_variant_iter_loop(&it, "s", &uuid))
                  {
                     if (g_str_equal(GATT_SERVICE_UUID, uuid))
                     {
                        has_gatt = true;
                     }
                  }
               }
               else if (g_str_equal("Connected", key))
               {
                  connected = g_variant_get_boolean(value);
               }
            }
            if (has_gatt && connected)
            {
               device.path = path;
               m_devices.emplace_back(device);
            }
         }
      }
   }

   // Iterate through the list a second time, looking for anything with a
   // gatt characteristic interface.
   g_variant_get(result.get(), "(a{oa{sa{sv}}})", &it_object);
   pit_object.reset(it_object, g_variant_iter_free);
   while (g_variant_iter_loop(it_object, "{oa{sa{sv}}}", &path, &it_interface))
   {
      gchar* interface{};
      GVariantIter* it_properties{};
      while (g_variant_iter_loop(it_interface, "{sa{sv}}", &interface, &it_properties))
      {
         if (g_str_equal(CHARACTERISTIC_INTERFACE, interface))
         {
            gchar* key{};
            GVariant* value{};
            std::string uuid;
            while (g_variant_iter_loop(it_properties, "{sv}", &key, &value))
            {
               if (g_str_equal("UUID", key))
               {
                  // TODO: is this nested? or is my parsing code broken?
                  uuid = g_variant_get_string(value, nullptr);
               }
            }
            if (!uuid.empty())
            {
               for (auto& device: m_devices)
               {
                  if (std::string(path).substr(0, device.path.size()) == device.path)
                  {
                     device.characteristics.emplace_back(uuid, path);
                     break;
                  }
               }
            }
         }
      }
   }

   return true;
}
