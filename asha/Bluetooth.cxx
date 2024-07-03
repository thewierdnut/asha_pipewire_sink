#include "Bluetooth.hh"
#include "GVariantDump.hh"
//#include "GVariantDump.hh"


#include <cassert>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <sstream>

#include <gio/gio.h>

using namespace asha;

namespace
{
   constexpr char BLUEZ_DEVICE[] = "org.bluez.Device1";
   static constexpr char ASHA_SERVICE_UUID[]    = "0000fdf0-0000-1000-8000-00805f9b34fb";

   uint64_t g_next_notify_id = 0;
}



Bluetooth::Bluetooth(const AddCallback& add, const RemoveCallback& remove):
   m_add_cb{add},
   m_remove_cb{remove}
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
      g_error("Error getting org.bluez ObjectManager interface: %s", err->message);
      g_error_free(err);
      throw std::runtime_error("Unable to connect to dbus");
   }

   // Lambda doesn't work with G_CALLBACK
   struct Callback {
      static void Signal(GDBusProxy* p, gchar* sender, gchar* signal, GVariant* parameters, gpointer* user_data)
      {
         auto* self = (Bluetooth*)user_data;
         // std::stringstream ss;
         // GVariantDump(parameters, ss);
         // g_info("Signal %s::%s %s", sender, signal, ss.str().c_str());

         if (g_str_equal(signal, "InterfacesAdded"))
         {
            gchar* path = nullptr;
            GVariantIter* it{};
            g_variant_get(parameters, "(oa{sa{sv}})", &path, &it);
            std::shared_ptr<char> ppath(path, g_free);
            std::shared_ptr<GVariantIter> pit(it, g_variant_iter_free);
            self->ProcessInterfaceAdd(path, it);
         }
         else if (g_str_equal(signal, "InterfacesRemoved"))
         {
            gchar* path = nullptr;
            GVariantIter* it{};
            g_variant_get(parameters, "(oas)", &path, &it);
            std::shared_ptr<char> ppath(path, g_free);
            std::shared_ptr<GVariantIter> pit(it, g_variant_iter_free);
            self->ProcessInterfaceRemoved(path, it);
         }
         else
         {
            std::stringstream ss;
            GVariantDump(parameters, ss);
            g_info("Signal %s::%s %s", sender, signal, ss.str().c_str());
         }
      }
   };
   m_signal_id = g_signal_connect(m_bluez_objects.get(),
      "g-signal",
      G_CALLBACK(&Callback::Signal),
      this
   );
   if (!EnumerateDevices())
      throw std::runtime_error("Unable to enumerate devices");
}


Bluetooth::~Bluetooth()
{
   // Probably not necessary, as the signals get disconnected when
   // m_bluez_objects gets destroyed.
   if (m_signal_id != (uint64_t)-1)
      g_signal_handler_disconnect(m_bluez_objects.get(), m_signal_id);
}


bool Bluetooth::EnumerateDevices()
{
   // TODO: Remove any devices that currently exist. Probably none, since the
   //       only place we call this is the constructor.
   m_devices.clear();
   m_bluez_properties.clear();

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

   // result is a tuple with one item. Unpack an array iterator from it.
   GVariantIter* it_object{};
   g_variant_get(result.get(), "(a{oa{sa{sv}}})", &it_object);
   std::shared_ptr<GVariantIter> pit_object(it_object, g_variant_iter_free);
   gchar* path{};
   GVariantIter* it_interface{};
   while (g_variant_iter_loop(it_object, "{oa{sa{sv}}}", &path, &it_interface))
      ProcessInterfaceAdd(path, it_interface);

   return true;
}

void Bluetooth::ProcessDevice(const std::string& path, GVariantIter* property_dict)
{
   gchar* key{};
   GVariant* value{};
   BluezDevice& device = m_devices[path];
   device.path = path;
   while (g_variant_iter_loop(property_dict, "{sv}", &key, &value))
      ProcessDeviceProperty(device, key, value);

   auto& iface = m_bluez_properties[path];
   if (!iface)
   {
      GError* err = nullptr;
      auto* piface = g_dbus_proxy_new_for_bus_sync(
         G_BUS_TYPE_SYSTEM,
         G_DBUS_PROXY_FLAGS_NONE,
         nullptr,
         "org.bluez",
         path.c_str(),
         "org.freedesktop.DBus.Properties",
         nullptr,
         &err
      );
      if (err)
      {
         g_error("Error getting dbus org.bluez Properties interface for %s: %s", path.c_str(), err->message);
         g_error_free(err);
      }
      else
      {
         iface.reset(piface, g_object_unref);
         
         // Lambda doesn't work with G_CALLBACK
         struct Callback {
            static void Signal(GDBusProxy* p, gchar* sender, gchar* signal, GVariant* parameters, gpointer* user_data)
            {
               auto* self = (Bluetooth*)user_data;

               if (g_str_equal(signal, "PropertiesChanged"))
               {
                  GVariantIter* it{};
                  g_variant_get(parameters, "(sa{sv}as)", nullptr, &it, nullptr);
                  std::shared_ptr<GVariantIter> pit(it, g_variant_iter_free);
                  std::string path = g_dbus_proxy_get_object_path(p);
                  auto& device = self->m_devices[path];
                  gchar* key{};
                  GVariant* value{};
                  while (g_variant_iter_loop(it, "{sv}", &key, &value))
                     self->ProcessDeviceProperty(device, key, value);
               }
               else
               {
                  std::stringstream ss;
                  GVariantDump(parameters, ss);
                  g_info("Device Signal %s::%s %s", sender, signal, ss.str().c_str());
               }
            }
         };
         // Despite the documentation, this doesn't seem to ever fire. g-signal works though.
         // g_signal_connect(iface.get(),
         //    "g-properties-changed",
         //    G_CALLBACK(&Callback::PropertiesChanged),
         //    this
         // );

         g_signal_connect(iface.get(),
            "g-signal",
            G_CALLBACK(&Callback::Signal),
            this
         );
      }
   }
}


void Bluetooth::ProcessDeviceProperty(BluezDevice& device, const char* key, struct _GVariant* value)
{
   bool was_ready = device.resolved && device.connected;
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
   // else if (g_str_equal("UUIDs", key))
   // {
   //    GVariantIter it{};
   //    g_variant_iter_init(&it, value);
   //    gchar* uuid{};
   //    while (g_variant_iter_loop(&it, "s", &uuid))
   //    {
   //       if (g_str_equal(GATT_SERVICE_UUID, uuid))
   //       {
   //          has_gatt = true;
   //       }
   //    }
   // }
   else if (g_str_equal("Connected", key))
   {
      // This gets set when somebody intentionally attaches the device.
      device.connected = g_variant_get_boolean(value);
   }
   else if (g_str_equal("ServicesResolved", key))
   {
      // This gets set when all the services and characteristics have been
      // enumerated. Presumably we won't see any more changes now.
      device.resolved = g_variant_get_boolean(value);
   }
   else if (g_str_equal("RSSI", key))
   {
      // These only ever get set by advertisements.
      // Aaaaand... they don't get unset when the device goes out of range.
      // Seeing this set only indicates that the device has been seen at some
      // time in the past.
   }

   bool now_ready = device.resolved && device.connected;
   if (!was_ready && now_ready)
   {
      g_info("Adding bluetooth device %s", device.name.c_str());
      PrepareAndAddDevice(device);
   }
   else if (was_ready && !now_ready)
   {
      g_info("Removing bluetooth device %s", device.name.c_str());
      m_remove_cb(device.path);
   }
}


void Bluetooth::ProcessInterfaceAdd(const std::string& path, struct _GVariantIter* iface_dict)
{
   gchar* interface{};
   GVariantIter* it_properties{};
   while (g_variant_iter_loop(iface_dict, "{sa{sv}}", &interface, &it_properties))
   {
      if (g_str_equal(BLUEZ_DEVICE, interface))
      {
         ProcessDevice(path, it_properties);
      }
   }
}


void Bluetooth::ProcessInterfaceRemoved(const std::string& path, struct _GVariantIter* iface_dict)
{
   auto it = m_devices.find(path);
   if (it != m_devices.end())
   {
      gchar* interface{};
      while (g_variant_iter_loop(iface_dict, "s", &interface))
      {
         if (g_str_equal(BLUEZ_DEVICE, interface))
         {
            bool active = it->second.connected && it->second.resolved;
            if (active)
            {
               g_info("Removing bluetooth device %s", it->second.name.c_str());
               m_remove_cb(path);
            }
            m_bluez_properties.erase(path);
            m_devices.erase(it);
         }
      }
   }
}


void Bluetooth::ProcessCharacteristic(BluezDevice& device, const std::string& path, GVariantIter* property_dict)
{

}


void Bluetooth::PrepareAndAddDevice(BluezDevice& device)
{
   assert(device.connected);
   assert(device.resolved);

   device.characteristics.clear();

   // Fill out the device characteristics before we forward it to the callback.
   // TODO: Is there a more efficient way of doing this?
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

   if (err)
   {
      g_error("Error making org.bluez GetManagedObjects call: %s", err->message);
      g_error_free(err);
      return;
   }

   // dbus calls always return tuples. Unpack the array iterator from it.
   GVariantIter* it_object{};
   g_variant_get(result.get(), "(a{oa{sa{sv}}})", &it_object);
   std::shared_ptr<GVariantIter> pit_object(it_object, g_variant_iter_free);
   gchar* path{};
   GVariantIter* it_interface{};
   while (g_variant_iter_loop(it_object, "{oa{sa{sv}}}", &path, &it_interface))
   {
      if (std::string(path).substr(0, device.path.size()) != device.path)
         continue;
      gchar* interface{};
      GVariantIter* it_properties{};
      while (g_variant_iter_loop(it_interface, "{sa{sv}}", &interface, &it_properties))
      {
         if (g_str_equal(CHARACTERISTIC_INTERFACE, interface))
         {
            gchar* key{};
            GVariant* value{};
            while (g_variant_iter_loop(it_properties, "{sv}", &key, &value))
            {
               if (g_str_equal("UUID", key))
                  device.characteristics.emplace_back(g_variant_get_string(value, nullptr), path);
            }
         }
      }
   }

   m_add_cb(device);
}
