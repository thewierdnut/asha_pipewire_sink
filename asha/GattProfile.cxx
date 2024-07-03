#include "GattProfile.hh"

#include "GVariantDump.hh"
#include "ObjectManager.hh"

#include <cassert>
#include <gio/gio.h>

#include <sstream>


using namespace asha;

namespace
{
   constexpr char ASHA_SERVICE_UUID[] = "0000fdf0-0000-1000-8000-00805f9b34fb";
   constexpr char APPLICATION_PATH[] = "/org/bluez/asha";
   constexpr char GATT_PROFILE_PATH[] = "/org/bluez/asha/profile";
}

GattProfile::GattProfile()
{
   // TODO: iterate through the adapters, and register for each one.
   // TODO: need objectmanager interface.
   GError* err = nullptr;
   m_gatt.reset(g_dbus_proxy_new_for_bus_sync(
      G_BUS_TYPE_SYSTEM,
      G_DBUS_PROXY_FLAGS_NONE,
      nullptr,
      "org.bluez",
      "/org/bluez/hci0",
      "org.bluez.GattManager1",
      nullptr,
      &err
   ), g_object_unref);

   if (err)
   {
      g_error("Error getting org.bluez ObjectManager interface: %s", err->message);
      g_error_free(err);
      return;
   }

   m_om.reset(new ObjectManager(g_dbus_proxy_get_connection(m_gatt.get()), APPLICATION_PATH));

   CreateProfile();
}


GattProfile::~GattProfile()
{
   DestroyProfile();
}


std::vector<std::string> GattProfile::GetPropertyList() const
{
   return {"UUIDs"};
}


GVariant* GattProfile::GetProperty(const std::string& name) const
{
   if (name == "UUIDs")
   {
      GVariantBuilder b = G_VARIANT_BUILDER_INIT(G_VARIANT_TYPE("as"));
      g_variant_builder_add(&b, "s", ASHA_SERVICE_UUID);
      return g_variant_builder_end(&b);
   }
   else
      return nullptr;
}


void GattProfile::CreateProfile()
{
   // register an object implementing the org.bluez.GattProfile1 interface.
   static GDBusMethodInfo METHOD_GET_MANAGED_OBJECTS = {
      .ref_count = -1,
      .name = (char*)"Release",
   };

   static GDBusMethodInfo* INTERFACE_METHODS[] = {
      &METHOD_GET_MANAGED_OBJECTS,
      nullptr
   };

   static GDBusPropertyInfo PROPERTIES[] = {
      {-1, (char*)"UUIDs", (char*)"as", G_DBUS_PROPERTY_INFO_FLAGS_READABLE, nullptr },
   };
   static GDBusPropertyInfo* INTERFACE_PROPERTIES[] = {
      PROPERTIES,
      nullptr
   };

   static GDBusInterfaceInfo OBJECT_MANAGER_INTERFACE = {
      .ref_count = -1,
      .name = (char*)"org.bluez.GattProfile1",
      .methods = INTERFACE_METHODS,
      .properties = INTERFACE_PROPERTIES,
   };

   static const GDBusInterfaceVTable OBJECT_MANAGER_VTABLE = {
      .method_call = &GattProfile::Call,
      .get_property = &GattProfile::GetDbusProperty,
   };

   GError* err = nullptr;
   m_object_id = g_dbus_connection_register_object(
      g_dbus_proxy_get_connection(m_gatt.get()),
      GATT_PROFILE_PATH,
      &OBJECT_MANAGER_INTERFACE,
      &OBJECT_MANAGER_VTABLE,
      this,
      nullptr,
      &err
   );
   if (err)
   {
      g_error("Error registering org.bluez.GattProfile1 interface: %s", err->message);
      g_error_free(err);
      m_object_id = -1;
   }
   m_om->AddInterface(GATT_PROFILE_PATH, "org.bluez.GattProfile1", *this);
   RegisterApplication();
}


void GattProfile::RegisterApplication()
{
   GVariantBuilder b = G_VARIANT_BUILDER_INIT(G_VARIANT_TYPE("(oa{sv})"));
   g_variant_builder_add_value(&b, g_variant_new_object_path(APPLICATION_PATH));
   g_variant_builder_add(&b, "a{sv}", nullptr); // I don't think any arguments are defined here.
   
   // Calling this asynchronously, in case bluez wants to examine the interface
   // (on this thread) before it returns.
   g_info("<-- gatt_manager.RegisterApplication(%s)", APPLICATION_PATH);
   g_dbus_proxy_call(m_gatt.get(),
      "RegisterApplication",
      g_variant_builder_end(&b),
      G_DBUS_CALL_FLAGS_NONE,
      -1,
      nullptr,
      [](GObject* proxy, GAsyncResult* async, gpointer user_data) {
         auto self = (GattProfile*)user_data;
         GError* e = nullptr;
         GVariant* result = g_dbus_proxy_call_finish((GDBusProxy*)proxy, async, &e);

         if (e)
         {
            g_warning("Error calling RegisterApplication: %s", e->message);
            g_error_free(e);
         }

         if (result)
         {
            std::shared_ptr<GVariant> presult(result, g_variant_unref);
            if (!g_variant_check_format_string(result, "()", false))
            {
               std::stringstream ss;
               GVariantDump(result, ss);
               g_warning("Incorrect response from RegisterApplication with type %s: %s", g_variant_get_type_string(result), ss.str().c_str());
            }
            else
               g_info("--> Finished registering GattManager with bluez");
         }
         else
         {
            g_warning("Null result when calling RegisterApplication");
         }
      },
      this
   );
}


void GattProfile::UnregisterApplication()
{
   GVariantBuilder b = G_VARIANT_BUILDER_INIT(G_VARIANT_TYPE("(o)"));
   g_variant_builder_add_value(&b, g_variant_new_object_path(APPLICATION_PATH));
   
   // Calling this asynchronously, in case bluez wants to examine the interface
   // (on this thread) before it returns.
   g_info("<-- gatt_manager.UnregisterApplication(%s)", APPLICATION_PATH);
   g_dbus_proxy_call(m_gatt.get(),
      "UnregisterApplication",
      g_variant_builder_end(&b),
      G_DBUS_CALL_FLAGS_NONE,
      -1,
      nullptr,
      [](GObject* proxy, GAsyncResult* async, gpointer user_data) {
         auto self = (GattProfile*)user_data;
         GError* e = nullptr;
         GVariant* result = g_dbus_proxy_call_finish((GDBusProxy*)proxy, async, &e);

         if (e)
         {
            g_warning("Error calling UnregisterApplication: %s", e->message);
            g_error_free(e);
         }

         if (result)
         {
            std::shared_ptr<GVariant> presult(result, g_variant_unref);
            if (!g_variant_check_format_string(result, "()", false))
            {
               std::stringstream ss;
               GVariantDump(result, ss);
               g_warning("Incorrect response from UnregisterApplication with type %s: %s", g_variant_get_type_string(result), ss.str().c_str());
            }
            else
               g_info("--> Finished Unregistering GattManager with bluez");
         }
         else
         {
            g_warning("Null result when calling UnregisterApplication");
         }
      },
      this
   );
}


void GattProfile::DestroyProfile()
{
   if (m_gatt && m_object_id != (uint32_t)-1)
   {
      UnregisterApplication();
      m_om->RemoveInterface(GATT_PROFILE_PATH, "org.bluez.GattProfile1");
      m_om.reset();
      g_dbus_connection_unregister_object(g_dbus_proxy_get_connection(m_gatt.get()), m_object_id);
   }
}


void GattProfile::Call(
   GDBusConnection*,
   const gchar* sender,
   const gchar* path,
   const gchar* interface,
   const gchar* method,
   GVariant* parameters,
   GDBusMethodInvocation* invocation,
   gpointer user_data)
{
   auto self = (GattProfile*)user_data;
   assert(g_str_equal(path, GATT_PROFILE_PATH));
   g_info("--> GattProfile::%s", method);

   if (g_str_equal(method, "Release"))
   {
      // TODO: This will probably happen after suspending... we need to
      //       re-register this profile on resuming.
      
      g_dbus_method_invocation_return_value(invocation, nullptr);
   }
   else
   {
      g_dbus_method_invocation_return_error(invocation, g_dbus_error_quark(), G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown Method");
   }
}


GVariant* GattProfile::GetDbusProperty(
   GDBusConnection* connection,
   const gchar* sender,
   const gchar* object_path,
   const gchar* interface_name,
   const gchar* property_name,
   GError** error,
   gpointer user_data)
{
   auto self = (GattProfile*)user_data;
   if (sender && object_path && property_name)
      g_info("--> %s %s GattProfile::GetProperty(%s)", sender, object_path, property_name);

   GVariant* ret = self->GetProperty(property_name);
   
   if (!ret)
   {
      g_warning("Requested unimplemented GattProfile property %s", property_name);
      if (error)
         *error = g_error_new(g_dbus_error_quark(), G_DBUS_ERROR_UNKNOWN_PROPERTY, "Property %s does not exist", property_name);
   }
   return ret;
}