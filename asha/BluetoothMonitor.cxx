#include "BluetoothMonitor.hh"

#include "Config.hh"
#include "GVariantDump.hh"
#include "Properties.hh"

#include <gio/gio.h>

#include <cassert>
#include <sstream>
#include <stdexcept>
#include <vector>

using namespace asha;

namespace
{
   constexpr char ASHA_SERVICE_UUID[] = "0000fdf0-0000-1000-8000-00805f9b34fb";
   constexpr char BASE_MONITOR_PATH[] = "/org/bluez/asha/monitor";
   constexpr char BLUEZ_DEVICE[] = "org.bluez.Device1";
}

// Manage the BASE_MONITOR_PATH, and respond to queries about it.
class BluetoothMonitor::Path final
{
public:
   Path(const std::string& path);
   ~Path();

   GDBusConnection* Connection() { return g_dbus_proxy_get_connection(m_monitor_manager.get()); }

   void AddMonitor(const std::shared_ptr<Monitor>& monitor);
   void RemoveMonitor(const std::string& path);

   void SetRssiPaired(int16_t rssi);
   void SetRssiUnpaired(int16_t rssi);
   void EnableRssiLogging(bool e);

protected:
   void Register();
   void Unregister();

   static void Call(
      GDBusConnection*,
      const gchar* sender,
      const gchar* path,
      const gchar* interface,
      const gchar* method,
      GVariant* parameters,
      GDBusMethodInvocation* invocation,
      gpointer user_data);

private:
   std::shared_ptr<GDBusProxy> m_monitor_manager;
   const std::string m_path;
   uint32_t m_object_id = -1;

   std::vector<std::shared_ptr<Monitor>> m_monitors;
};

class BluetoothMonitor::Monitor final
{
public:
   Monitor(GDBusConnection* connection, const std::string& path);
   ~Monitor();

   const std::string& Path() { return m_path; }
   GVariant* GetAllInterfacesAndProperties();
   GVariant* GetAllInterfaces();

   void SetRssiPaired(int16_t rssi);
   void SetRssiUnpaired(int16_t rssi);

   void EnableRssiLogging(bool e) { m_rssi_logging = e; }

protected:
   static void Call(
      GDBusConnection* connection,
      const gchar* sender,
      const gchar* path,
      const gchar* interface,
      const gchar* method,
      GVariant* parameters,
      GDBusMethodInvocation* invocation,
      gpointer user_data);

   static GVariant* GetProperty(
      GDBusConnection* connection,
      const gchar* sender,
      const gchar* object_path,
      const gchar* interface_name,
      const gchar* property_name,
      GError** error,
      gpointer user_data);

   void DeviceFound(const std::string& path);
   void DeviceLost(const std::string& path);
   void PropertyUpdated(const std::string& path, const std::string& key, const std::shared_ptr<_GVariant>& value);

   void ConnectToDevice(const std::string& path, bool already_paired);

private:
   const std::string m_path;
   uint32_t m_object_id = -1;
   GDBusConnection* const m_connection;

   struct DeviceInfo
   {
      Properties props;
      bool paired = false;
      bool connected = false;
   };

   std::map<std::string, DeviceInfo> m_devices;

   int m_rssi_paired = 0;
   int m_rssi_unpaired = 0;

   bool m_rssi_logging = false;
};


BluetoothMonitor::Path::Path(const std::string& path):
   m_path(path)
{
   // TODO: iterate through adapters, register for each one.
   GError* err = nullptr;
   m_monitor_manager.reset(g_dbus_proxy_new_for_bus_sync(
      G_BUS_TYPE_SYSTEM,
      G_DBUS_PROXY_FLAGS_NONE,
      nullptr,
      "org.bluez",
      "/org/bluez/hci0",
      "org.bluez.AdvertisementMonitorManager1",
      nullptr,
      &err
   ), g_object_unref);

   if (err)
   {
      g_error("Error getting org.bluez AdvertisementMonitorManager1 interface: %s", err->message);
      g_error_free(err);
      throw std::runtime_error("Unable to connect to dbus");
   }


   static GDBusArgInfo ARG_INTERFACES_AND_PROPERTIES = {
      .ref_count = -1, .name = (char*)"objpath_interfaces_and_properties", .signature = (char*)"a{oa{sa{sv}}}"
   };

   static GDBusArgInfo* ARGS_GET_MANAGED_OBJECTS[] = {
      &ARG_INTERFACES_AND_PROPERTIES,
      nullptr
   };

   static GDBusMethodInfo METHOD_GET_MANAGED_OBJECTS = {
      .ref_count = -1,
      .name = (char*)"GetManagedObjects",
      .out_args = ARGS_GET_MANAGED_OBJECTS
   };

   static GDBusMethodInfo* INTERFACE_METHODS[] = {
      &METHOD_GET_MANAGED_OBJECTS,
      nullptr
   };

   static GDBusArgInfo ARG_INTERFACES_ADDED[] = {
      { .ref_count = -1, .name = (char*)"object_path", .signature = (char*)"o" },
      { .ref_count = -1, .name = (char*)"interfaces_and_properties", .signature = (char*)"a{sa{sv}}" },
   };
   static GDBusArgInfo* ARGS_INTERFACES_ADDED[] = {
      ARG_INTERFACES_ADDED + 0,
      ARG_INTERFACES_ADDED + 1,
      nullptr
   };
   static GDBusArgInfo ARG_INTERFACES_REMOVED[] = {
      { .ref_count = -1, .name = (char*)"object_path", .signature = (char*)"o" },
      { .ref_count = -1, .name = (char*)"interfaces", .signature = (char*)"as" },
   };
   static GDBusArgInfo* ARGS_INTERFACES_REMOVED[] = {
      ARG_INTERFACES_REMOVED + 0,
      ARG_INTERFACES_REMOVED + 1,
      nullptr
   };

   static GDBusSignalInfo SIGNALS[] = {
      {-1, (char*)"InterfacesAdded", ARGS_INTERFACES_ADDED },
      {-1, (char*)"InterfacesRemoved", ARGS_INTERFACES_REMOVED }
   };
   static GDBusSignalInfo* INTERFACE_SIGNALS[] = {
      SIGNALS + 0,
      SIGNALS + 1,
      nullptr
   };

   static GDBusInterfaceInfo OBJECT_MANAGER_INTERFACE = {
      .ref_count = -1,
      .name = (char*)"org.freedesktop.DBus.ObjectManager",
      .methods = INTERFACE_METHODS,
      .signals = INTERFACE_SIGNALS,
   };

   static const GDBusInterfaceVTable OBJECT_MANAGER_VTABLE = {
      .method_call = &Path::Call,
   };
   m_object_id = g_dbus_connection_register_object(
      g_dbus_proxy_get_connection(m_monitor_manager.get()),
      m_path.c_str(),
      &OBJECT_MANAGER_INTERFACE,
      &OBJECT_MANAGER_VTABLE,
      this,
      nullptr,
      &err
   );
   if (err)
   {
      m_monitor_manager.reset();
      g_error("Error registering org.bluez.profile interface: %s", err->message);
      g_error_free(err);
      throw std::runtime_error("Unable to create bluez profile");
   }

   // Register a base path with bluez. Bluez should see any objects created
   // with this prefix.
   Register();
}


BluetoothMonitor::Path::~Path()
{
   Unregister();
   if (m_object_id != (uint32_t)-1)
      g_dbus_connection_unregister_object(g_dbus_proxy_get_connection(m_monitor_manager.get()), m_object_id);
}


void BluetoothMonitor::Path::Register()
{
   GVariantBuilder b = G_VARIANT_BUILDER_INIT(G_VARIANT_TYPE("(o)"));
   g_variant_builder_add_value(&b, g_variant_new_object_path(m_path.c_str()));
   
   // Can't call this synchronously. After I call this, bluez will query
   // me for any objects before it responds, but I would respond on this
   // thread.
   g_info("<-- monitor_manager.RegisterMonitor(%s)", m_path.c_str());
   g_dbus_proxy_call(m_monitor_manager.get(),
      "RegisterMonitor",
      g_variant_builder_end(&b),
      G_DBUS_CALL_FLAGS_NONE,
      -1,
      nullptr,
      [](GObject* proxy, GAsyncResult* async, gpointer user_data) {
         auto self = (BluetoothMonitor::Path*)user_data;
         GError* e = nullptr;
         GVariant* result = g_dbus_proxy_call_finish((GDBusProxy*)proxy, async, &e);

         if (e)
         {
            g_warning("Error calling RegisterMonitor: %s", e->message);
            g_error_free(e);
         }

         if (result)
         {
            std::shared_ptr<GVariant> presult(result, g_variant_unref);
            if (!g_variant_check_format_string(result, "()", false))
            {
               std::stringstream ss;
               GVariantDump(result, ss);
               g_warning("Incorrect response from RegisterMonitor with type %s: %s", g_variant_get_type_string(result), ss.str().c_str());
            }
            else
               g_info("--> Finished Registering monitor base path with bluez");
         }
         else
         {
            g_warning("Null result when calling RegisterMonitor");
         }
      },
      this
   );


   
   // GVariant* result = g_dbus_proxy_call_sync(m_monitor_manager.get(),
   //    "RegisterMonitor",
   //    g_variant_builder_end(&b),
   //    G_DBUS_CALL_FLAGS_NONE,
   //    -1,
   //    nullptr,
   //    &e
   // );
}


void BluetoothMonitor::Path::Unregister()
{
   GVariantBuilder b = G_VARIANT_BUILDER_INIT(G_VARIANT_TYPE("(o)"));
   g_variant_builder_add_value(&b, g_variant_new_object_path(m_path.c_str()));

   g_info("<-- monitor_manager.UnregisterMonitor(%s)", m_path.c_str());

   GError* e = nullptr;
   GVariant* result = g_dbus_proxy_call_sync(m_monitor_manager.get(),
      "UnregisterMonitor",
      g_variant_builder_end(&b),
      G_DBUS_CALL_FLAGS_NONE,
      -1,
      nullptr,
      &e
   );
   if (e)
   {
      g_warning("Error calling UnregisterMonitor: %s", e->message);
      g_error_free(e);
   }

   if (result)
   {
      std::shared_ptr<GVariant> presult(result, g_variant_unref);
      if (!g_variant_check_format_string(result, "()", false))
      {
         std::stringstream ss;
         GVariantDump(result, ss);
         g_warning("Incorrect response from UnregisterMonitor with type %s: %s", g_variant_get_type_string(result), ss.str().c_str());
      }
      else
         g_info("Unregistered monitor base path with bluez");
   }
   else
   {
      g_warning("Null result when calling UnregisterMonitor");
   }
}


void BluetoothMonitor::Path::Call(
   GDBusConnection*,
   const gchar* sender,
   const gchar* path,
   const gchar* interface,
   const gchar* method,
   GVariant* parameters,
   GDBusMethodInvocation* invocation,
   gpointer user_data)
{
   auto self = (BluetoothMonitor::Path*)user_data;
   assert(path == self->m_path);
   g_info("--> Path::%s", method);

   if (g_str_equal(method, "GetManagedObjects"))
   {
      GVariantBuilder b = G_VARIANT_BUILDER_INIT(G_VARIANT_TYPE("a{oa{sa{sv}}}"));
      for (auto& monitor: self->m_monitors)
      {
         GVariantBuilder b2 = G_VARIANT_BUILDER_INIT(G_VARIANT_TYPE("{oa{sa{sv}}}"));
         g_variant_builder_add(&b2, "o", monitor->Path().c_str());
         g_variant_builder_add_value(&b2, monitor->GetAllInterfacesAndProperties());

         g_variant_builder_add_value(&b, g_variant_builder_end(&b2));
      }
      GVariant* ret = g_variant_new("(a{oa{sa{sv}}})", &b);
      
      g_info("    <--- Returning %s", GVariantDump(ret).c_str());
      g_dbus_method_invocation_return_value(invocation, ret);

   }
   else
   {
      g_dbus_method_invocation_return_error(invocation, g_dbus_error_quark(), G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown Method");
   }
}


void BluetoothMonitor::Path::AddMonitor(const std::shared_ptr<Monitor>& monitor)
{
   m_monitors.push_back(monitor);

   GVariantBuilder b = G_VARIANT_BUILDER_INIT(G_VARIANT_TYPE("(oa{sa{sv}})"));
   g_variant_builder_add(&b, "o", monitor->Path().c_str());
   g_variant_builder_add_value(&b, monitor->GetAllInterfacesAndProperties());
   g_info("<-- Path::InterfacesAdded(%s {})", monitor->Path().c_str());

   GError* err{};
   g_dbus_connection_emit_signal(
      g_dbus_proxy_get_connection(m_monitor_manager.get()),
      nullptr,
      m_path.c_str(),
      "org.freedesktop.DBus.ObjectManager",
      "InterfacesAdded",
      g_variant_builder_end(&b), // (oa{sa{sv}})
      &err
   );
   if (err)
   {
      m_object_id = -1;
      m_monitor_manager.reset();
      g_warning("Error emitting InterfacesAdded for the new monitor: %s", err->message);
      g_error_free(err);
   }
}


void BluetoothMonitor::Path::RemoveMonitor(const std::string& path)
{
   for (auto it = m_monitors.begin(); it != m_monitors.end(); ++it)
   {
      if ((*it)->Path() == path)
      {
         g_info("<-- Path::InterfacesRemoved(%s)", path.c_str());
         GVariantBuilder b = G_VARIANT_BUILDER_INIT(G_VARIANT_TYPE("(oas)"));
         g_variant_builder_add(&b, "o", path.c_str());
         g_variant_builder_add_value(&b, (*it)->GetAllInterfaces());

         GError* err{};
         g_dbus_connection_emit_signal(
            g_dbus_proxy_get_connection(m_monitor_manager.get()),
            nullptr,
            m_path.c_str(),
            "org.freedesktop.DBus.ObjectManager",
            "InterfacesRemoved",
            g_variant_builder_end(&b),
            &err
         );
         if (err)
         {
            m_object_id = -1;
            m_monitor_manager.reset();
            g_warning("Error emitting InterfacesRemoved for deleted monitor: %s", err->message);
            g_error_free(err);
         }

         m_monitors.erase(it);
         break;
      }
   }
}

void BluetoothMonitor::Path::SetRssiPaired(int16_t rssi)
{
   for (auto& m: m_monitors)
      m->SetRssiPaired(rssi);
}


void BluetoothMonitor::Path::SetRssiUnpaired(int16_t rssi)
{
   for (auto& m: m_monitors)
      m->SetRssiUnpaired(rssi);
}


void BluetoothMonitor::Path::EnableRssiLogging(bool e)
{
   for (auto& m: m_monitors)
      m->EnableRssiLogging(e);
}


BluetoothMonitor::Monitor::Monitor(GDBusConnection* connection, const std::string& path):
   m_connection(connection),
   m_path(path),
   m_rssi_paired(Config::RssiPaired()),
   m_rssi_unpaired(Config::RssiUnpaired())

{
   static GDBusMethodInfo METHOD_RELEASE = {
      .ref_count = -1,
      .name = (char*)"Release"
   };
   static GDBusMethodInfo METHOD_ACTIVATE = {
      .ref_count = -1,
      .name = (char*)"Activate"
   };
   static GDBusArgInfo ARG_DEVICE[] = {
      { .ref_count = -1, .name = (char*)"device",        .signature = (char*)"o" },
   };
   static GDBusArgInfo* ARGS_DEVICE[] = {
      ARG_DEVICE + 0,
      nullptr
   };
   static GDBusMethodInfo METHOD_DEVICE_FOUND = {
      .ref_count = -1,
      .name = (char*)"DeviceFound",
      .in_args = ARGS_DEVICE
   };
   static GDBusMethodInfo METHOD_DEVICE_LOST = {
      .ref_count = -1,
      .name = (char*)"DeviceLost",
      .in_args = ARGS_DEVICE
   };
   static GDBusMethodInfo* MONITOR_METHODS[] = {
      &METHOD_RELEASE,
      &METHOD_ACTIVATE,
      &METHOD_DEVICE_LOST,
      &METHOD_DEVICE_FOUND,
      nullptr
   };

   static GDBusPropertyInfo PROPERTY_TYPE          {-1, (char*)"Type",              (char*)"s",    G_DBUS_PROPERTY_INFO_FLAGS_READABLE, nullptr};
   static GDBusPropertyInfo PROPERTY_RSSI_LOW      {-1, (char*)"RSSILowThreshold",  (char*)"n",    G_DBUS_PROPERTY_INFO_FLAGS_READABLE, nullptr};
   static GDBusPropertyInfo PROPERTY_RSSI_HIGH     {-1, (char*)"RSSIHighThreshold", (char*)"n",    G_DBUS_PROPERTY_INFO_FLAGS_READABLE, nullptr};
   static GDBusPropertyInfo PROPERTY_TIMEOUT_LOW   {-1, (char*)"RSSILowTimeout",    (char*)"q",    G_DBUS_PROPERTY_INFO_FLAGS_READABLE, nullptr};
   static GDBusPropertyInfo PROPERTY_TIMEOUT_HIGH  {-1, (char*)"RSSIHighTimeout",   (char*)"q",    G_DBUS_PROPERTY_INFO_FLAGS_READABLE, nullptr};
   static GDBusPropertyInfo PROPERTY_PERIOD        {-1, (char*)"RSSISamplingPeriod",(char*)"q",    G_DBUS_PROPERTY_INFO_FLAGS_READABLE, nullptr};
   static GDBusPropertyInfo PROPERTY_PATTERNS      {-1, (char*)"Patterns",          (char*)"yyay", G_DBUS_PROPERTY_INFO_FLAGS_READABLE, nullptr};
   
   static GDBusPropertyInfo* MONITOR_PROPERTIES[] = {
      &PROPERTY_TYPE,
      &PROPERTY_RSSI_LOW,
      &PROPERTY_RSSI_HIGH,
      &PROPERTY_TIMEOUT_LOW,
      &PROPERTY_TIMEOUT_HIGH,
      &PROPERTY_PERIOD,
      &PROPERTY_PATTERNS,
      nullptr
   };

   static const GDBusInterfaceVTable MONITOR_VTABLE = {
      .method_call = &Monitor::Call,
      .get_property = &Monitor::GetProperty,
   };

   static GDBusInterfaceInfo MONITOR_INTERFACE = {
      .ref_count = -1,
      .name = (char*)"org.bluez.AdvertisementMonitor1",
      .methods = MONITOR_METHODS,
      .properties = MONITOR_PROPERTIES
   };
   GError* err{};
   m_object_id = g_dbus_connection_register_object(
      connection,
      m_path.c_str(),
      &MONITOR_INTERFACE,
      &MONITOR_VTABLE,
      this,
      nullptr,
      &err
   );
   if (err)
   {
      m_object_id = -1;
      g_warning("Error registering monitor interface: %s", err->message);
      g_error_free(err);
   }
}


BluetoothMonitor::Monitor::~Monitor()
{
   if (m_object_id == (uint32_t)-1)
      g_dbus_connection_unregister_object(m_connection, m_object_id);
}


void BluetoothMonitor::Monitor::Call(
   GDBusConnection* connection,
   const gchar* sender,
   const gchar* path,
   const gchar* interface,
   const gchar* method,
   GVariant* parameters,
   GDBusMethodInvocation* invocation,
   gpointer user_data)
{
   auto self = (Monitor*)user_data;

   if (g_str_equal(method, "Release"))
   {
      g_info("--> %s %s Monitor::Release()", sender, path);
      // This gets called when computer suspends. We need to re-activate when
      // the computer wakes back up.
      g_dbus_method_invocation_return_value(invocation, nullptr);
   }
   else if (g_str_equal(method, "Activate"))
   {
      g_info("--> %s %s Monitor::Activate()", sender, path);
      g_dbus_method_invocation_return_value(invocation, nullptr);
   }
   else if (g_str_equal(method, "DeviceFound"))
   {
      gchar* p = nullptr;
      g_variant_get(parameters, "(o)", &p);
      std::string name = p ? p : "";
      g_free(p);
      if (!name.empty())
         self->DeviceFound(name);
      g_dbus_method_invocation_return_value(invocation, nullptr);
   }
   else if (g_str_equal(method, "DeviceLost"))
   {
      gchar* p = nullptr;
      g_variant_get(parameters, "(o)", &p);
      std::string name = p ? p : "";
      g_free(p);
      self->DeviceLost(name);
      g_dbus_method_invocation_return_value(invocation, nullptr);
   }
   else
   {
      g_info("--> %s %s Monitor::%s(%s)", sender, path, method, GVariantDump(parameters).c_str());
      g_dbus_method_invocation_return_error(invocation, g_dbus_error_quark(), G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown Method");
   }
}


GVariant* BluetoothMonitor::Monitor::GetProperty(
   GDBusConnection* connection,
   const gchar* sender,
   const gchar* object_path,
   const gchar* interface_name,
   const gchar* property_name,
   GError** error,
   gpointer user_data)
{
   auto self = (BluetoothMonitor*)user_data;
   if (sender)
      g_info("--> %s %s Monitor::GetProperty(%s)", sender, object_path, property_name);

   if (g_str_equal(property_name, "Type"))               return g_variant_new_string("or_patterns");
   if (g_str_equal(property_name, "RSSILowThreshold"))   return g_variant_new_int16(-127);
   if (g_str_equal(property_name, "RSSIHighThreshold"))  return g_variant_new_int16(-100);
   if (g_str_equal(property_name, "RSSILowTimeout"))     return g_variant_new_uint16(5);
   if (g_str_equal(property_name, "RSSIHighTimeout"))    return g_variant_new_uint16(1);
   if (g_str_equal(property_name, "RSSISamplingPeriod")) return g_variant_new_uint16(0);
   if (g_str_equal(property_name, "Patterns"))
   {
      // N.B. This is patterned after a poorly designed microsoft low power
      //      extension to the hci standard, which only matches on whole AD
      //      bytes, which limits our possibilities here. This api doesn't
      //      appear to work correctly, but it *does* enable passive adv
      //      monitoring, and even though it doesn't notify us of those
      //      advertisements, it *does* update the device RSSI values, which
      //      we can watch for instead.
      // Stuff seen at startup:
      //   16  bit uuid type   3 fdf0
      //   manufacturer type 255 00ba (starkey)
      // Stuff seen all the time:
      //   Flags        type   1 0x06 (these are flags, so we may want to watch for others values too, like 0x07)
      //   EMPTY        ?             (This is sent all the time. Not sure how to watch for it.)

      GVariantBuilder b = G_VARIANT_BUILDER_INIT(G_VARIANT_TYPE("a(yyay)"));
      auto AddFilter = [&](uint8_t ad_offset, uint8_t ad_type, const std::vector<uint8_t>& bytes)
      {
         GVariantBuilder byte_array  = G_VARIANT_BUILDER_INIT(G_VARIANT_TYPE("ay"));
         for (uint8_t byte: bytes)
            g_variant_builder_add(&byte_array, "y", byte);
         g_variant_builder_add(&b, "(yyay)", ad_offset, ad_type, &byte_array);
      };
      // Params: offset, ad_type, content_to_match
      //    See note above... I'm not sure it actually matters what we scan
      //    for, since the API is broken. My main goal is to just put the
      //    adapter into passive scanning mode so that advertisements update
      //    the device properties.
      AddFilter(0,   3, {0xf0, 0xfd}); // ASHA service id
      AddFilter(0, 255, {0xba, 0x00}); // Starkey manufacturer id
      // Scanning for flags (mostly because we just have to scan for *something*)
      //   { 0, "LE Limited Discoverable Mode"		},
	   //   { 1, "LE General Discoverable Mode"		},
	   //   { 2, "BR/EDR Not Supported"			},
	   //   { 3, "Simultaneous LE and BR/EDR (Controller)"	},
	   //   { 4, "Simultaneous LE and BR/EDR (Host)"	},
      //   We want at least bit 0 or 1 to be lit, don't care about the rest,
      //   which gives us 24 valid values.... I'm just going to scan for the
      //   flags my hearing-aids produce.
      AddFilter(0,   1, {0x06});       // LE general discoverable mode | BR/EDR not supported
      
      return g_variant_builder_end(&b);
   }

   g_warning("Requesting unimplemented AdvertisementMonitor property %s", property_name);
   if (error)
      *error = g_error_new(g_dbus_error_quark(), G_DBUS_ERROR_UNKNOWN_PROPERTY, "Property %s does not exist", property_name);
   return nullptr;
}


GVariant* BluetoothMonitor::Monitor::GetAllInterfacesAndProperties()
{
   GVariantBuilder ret = G_VARIANT_BUILDER_INIT(G_VARIANT_TYPE("a{sa{sv}}"));
   {
      GVariantBuilder props = G_VARIANT_BUILDER_INIT(G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&props, "{sv}", "Type", GetProperty(nullptr, nullptr, nullptr, nullptr, "Type", nullptr, this ));
      g_variant_builder_add(&props, "{sv}", "RSSILowThreshold", GetProperty(nullptr, nullptr, nullptr, nullptr, "RSSILowThreshold", nullptr, this ));
      g_variant_builder_add(&props, "{sv}", "RSSIHighThreshold", GetProperty(nullptr, nullptr, nullptr, nullptr, "RSSIHighThreshold", nullptr, this ));
      g_variant_builder_add(&props, "{sv}", "RSSILowTimeout", GetProperty(nullptr, nullptr, nullptr, nullptr, "RSSILowTimeout", nullptr, this ));
      g_variant_builder_add(&props, "{sv}", "RSSIHighTimeout", GetProperty(nullptr, nullptr, nullptr, nullptr, "RSSIHighTimeout", nullptr, this ));
      g_variant_builder_add(&props, "{sv}", "RSSISamplingPeriod", GetProperty(nullptr, nullptr, nullptr, nullptr, "RSSISamplingPeriod", nullptr, this ));
      g_variant_builder_add(&props, "{sv}", "Patterns", GetProperty(nullptr, nullptr, nullptr, nullptr, "Patterns", nullptr, this ));

      g_variant_builder_add(&ret, "{sa{sv}}", "org.bluez.AdvertisementMonitor1", &props);
   }

   return g_variant_builder_end(&ret);
}

GVariant* BluetoothMonitor::Monitor::GetAllInterfaces()
{
   GVariantBuilder ret = G_VARIANT_BUILDER_INIT(G_VARIANT_TYPE("as"));
   g_variant_builder_add(&ret, "s", "org.bluez.AdvertisementMonitor1");

   return g_variant_builder_end(&ret);
}


void BluetoothMonitor::Monitor::DeviceFound(const std::string& path)
{
   // Finding a device doesn't guarantee that it matches our parameters, since
   // other monitors on the system could be active, and the kernel merges all
   // of their filters together. Double-check the uuids for the asha service
   // before accepting this device.
   Properties p(BLUEZ_DEVICE, path);
   auto uuids = p.Get("UUIDs");

   if (!uuids)
   {
      // TODO: This is broken somehow? the UUIDs should always exist for
      //       bluetooth devices, especially for ones we have previously
      //       paired.
      g_warning("%s didn't have any uuids?????", path.c_str());
      return;
   }
   // g_debug("uuids: %s", GVariantDump(uuids.get()).c_str());
   
   GVariantIter* it_uuids{};
   bool has_asha = false;
   GVariant* v = g_variant_get_variant(uuids.get());
   if (v)
   {
      g_variant_get(v, "as", &it_uuids);
      if (it_uuids)
      {
         gchar* uuid;
         while (g_variant_iter_loop(it_uuids, "&s", &uuid))
         {
            if (strcasecmp(uuid, ASHA_SERVICE_UUID) == 0)
            {
               has_asha = true;
               break;
            }
         }
         g_variant_iter_free(it_uuids);
      }
      g_variant_unref(v);
   }

   if (has_asha)
   {
      auto it = m_devices.find(path);
      if (it == m_devices.end())
      {
         auto& d = m_devices[path];
         d.connected = GVariantToBool(p.Get("Connected"));
         d.paired = GVariantToBool(p.Get("Paired"));
         d.props = std::move(p);

         std::string device_path = path;
         g_info("Monitoring %s", path.c_str());
         d.props.Subscribe([this, device_path](const std::string& key, const std::shared_ptr<_GVariant>& value) {
            PropertyUpdated(device_path, key, value);
         });
      }
   }

}


void BluetoothMonitor::Monitor::DeviceLost(const std::string& path)
{
   if (m_devices.erase(path))
      g_info("No longer monitoring %s", path.c_str());
}


void BluetoothMonitor::Monitor::PropertyUpdated(const std::string& path, const std::string& key, const std::shared_ptr<_GVariant>& value)
{
   auto it = m_devices.find(path);
   if (it != m_devices.end())
   {
      // g_info("Updated %s %s: %s", path.c_str(), key.c_str(), GVariantDump(value.get()).c_str());
      if (key == "Connected")
         it->second.connected = GVariantToBool(value);
      else if (key == "Paired")
         it->second.paired = GVariantToBool(value);
      else if (key == "RSSI")
      {
         if (m_rssi_logging)
            g_info("Updated %s RSSI: %s", path.c_str(), GVariantDump(value.get()).c_str());
         if (!it->second.connected)
         {
            int min_rssi = it->second.paired ? m_rssi_paired : m_rssi_unpaired;
            if (min_rssi != 0)
            {
               int rssi = GVariantToInt16(value);
               if (rssi != 0 && min_rssi < rssi)
                  ConnectToDevice(path, it->second.paired);
            }
         }
      }
   }
}


void BluetoothMonitor::Monitor::ConnectToDevice(const std::string& path, bool already_paired)
{
   g_info("%s %s", already_paired ? "Connecting" : "Pairing", path.c_str());
   GError* err = nullptr;
   std::shared_ptr<_GDBusProxy> device(g_dbus_proxy_new_for_bus_sync(
      G_BUS_TYPE_SYSTEM,
      G_DBUS_PROXY_FLAGS_NONE,
      nullptr,
      "org.bluez",
      path.c_str(),
      BLUEZ_DEVICE,
      nullptr,
      &err
   ), g_object_unref);

   if (err)
   {
      g_error("Error getting dbus %s proxy: %s", BLUEZ_DEVICE, err->message);
      g_error_free(err);
      device.reset();
      return;
   }

   GError* e = nullptr;
   GVariant* result = g_dbus_proxy_call_sync(device.get(),
      already_paired ? "Connect" : "Pair",
      nullptr,
      G_DBUS_CALL_FLAGS_NONE,
      -1,
      nullptr,
      &e
   );
   if (e)
   {
      // TODO: knowing the severity of the error here depends on context
      g_warning("Error trying to %s to %s: %s", already_paired ? "connect" : "pair", path.c_str(), e->message);
      g_error_free(e);
   }
   if (result)
      g_variant_unref(result);
}


void BluetoothMonitor::Monitor::SetRssiPaired(int16_t rssi)
{
   m_rssi_paired = rssi;
}


void BluetoothMonitor::Monitor::SetRssiUnpaired(int16_t rssi)
{
   m_rssi_unpaired = rssi;
}


BluetoothMonitor::BluetoothMonitor()
{
   // This is complicated. We need to create a dbus object at BASE_MONITOR_PATH
   // that will respond to org.freedesktop.DBus.ObjectManager calls. We then
   // register that object with bluez, who will use it to introspect all of our
   // monitor objects. We can notify bluez that objects have been added or
   // removed via the InterfacesAdded and InterfacesRemoved signals of our
   // new object. This is not to be confused with the root ObjectManager
   // interface maintained by bluez.
   m_path = std::make_shared<Path>(BASE_MONITOR_PATH);

   // TODO: Add a separate one for each pattern? (and make pattern a parameter)
   auto monitor = std::make_shared<Monitor>(m_path->Connection(), BASE_MONITOR_PATH + std::string("/monitor0"));
   m_path->AddMonitor(monitor);
}


BluetoothMonitor::~BluetoothMonitor()
{
   m_path->RemoveMonitor(BASE_MONITOR_PATH + std::string("/monitor0"));
   m_path.reset();
}


void BluetoothMonitor::SetRssiPaired(int16_t rssi)
{
   try
   {
      Config::SetConfigItem("rssi_paired", rssi);
      m_path->SetRssiPaired(rssi);
   }
   catch (const std::runtime_error& e)
   {
      g_info("Unable to set rssi_paired to %hd: %s", rssi, e.what());
   }
}


void BluetoothMonitor::SetRssiUnpaired(int16_t rssi)
{
   try
   {
      Config::SetConfigItem("rssi_unpaired", rssi);
      m_path->SetRssiUnpaired(rssi);
   }
   catch (const std::runtime_error& e)
   {
      g_info("Unable to set rssi_unpaired to %hd: %s", rssi, e.what());
   }
}


void BluetoothMonitor::EnableRssiLogging(bool e)
{
   if (m_path)
      m_path->EnableRssiLogging(e);
}