#include "ObjectManager.hh"
#include "GVariantDump.hh"
#include <cassert>



using namespace asha;

ObjectManager::ObjectManager(GDBusConnection* connection, const std::string& base_path):
   m_connection(connection),
   m_base_path(base_path)
{
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
      .method_call = &ObjectManager::Call,
   };
   GError* err = nullptr;
   m_object_id = g_dbus_connection_register_object(
      m_connection,
      m_base_path.c_str(),
      &OBJECT_MANAGER_INTERFACE,
      &OBJECT_MANAGER_VTABLE,
      this,
      nullptr,
      &err
   );
   if (err)
   {
      m_object_id = -1;
      g_error("Error registering ObjectManager interface: %s", err->message);
      g_error_free(err);
   }
}


ObjectManager::~ObjectManager()
{
   if (m_connection && m_object_id == (uint32_t)-1)
   {
      g_dbus_connection_unregister_object(m_connection, m_object_id);
   }
}


void ObjectManager::AddInterface(const std::string& path, const std::string& iface_name)
{
   auto it = m_objects.find(path);
   if (it == m_objects.end()) return;
   auto it2 = it->second.begin();
   for(;it2 != it->second.end() && it2->name != iface_name; ++it2);
   if (it2 == it->second.end()) return;

   GVariantBuilder ob = G_VARIANT_BUILDER_INIT(G_VARIANT_TYPE("a{sa{sv}}"));
   GVariantBuilder ib = G_VARIANT_BUILDER_INIT(G_VARIANT_TYPE("a{sv}"));
   for (auto& propertyname: it2->GetPropertyList())
      g_variant_builder_add(&ib, "{sv}", propertyname.c_str(), it2->GetProperty(propertyname));
   g_variant_builder_add(&ob, "{sa{sv}}", it2->name.c_str(), &ib);
   
   GVariant* args = g_variant_new("(oa{sa{sv}})", it->first.c_str(), &ob);

   g_info("<-- %s ObjectManager::InterfacesAdded(%s)", m_base_path.c_str(), GVariantDump(args).c_str());

   GError* err{};
   g_dbus_connection_emit_signal(
      m_connection,
      nullptr,
      it->first.c_str(),
      "org.freedesktop.DBus.ObjectManager",
      "InterfacesAdded",
      args, // (oa{sa{sv}})
      &err
   );
   if (err)
   {
      g_warning("Error emitting InterfacesAdded for the new interface: %s", err->message);
      g_error_free(err);
   }
}


void ObjectManager::RemoveInterface(const std::string& path, const std::string& iface_name)
{
   for (auto it = m_objects.begin(); it != m_objects.end(); ++it)
   {
      for (auto it2 = it->second.begin(); it2 != it->second.end(); ++it2)
      {
         if (it2->path == path && it2->name == iface_name)
         {
            g_info("<-- %s ObjectManager::InterfacesRemoved(%s %s)", m_base_path.c_str(), it->first.c_str(), iface_name.c_str());
            GVariantBuilder b = G_VARIANT_BUILDER_INIT(G_VARIANT_TYPE("(oas)"));
            g_variant_builder_add(&b, "o", it->first.c_str());
            GVariantBuilder ab = G_VARIANT_BUILDER_INIT(G_VARIANT_TYPE("as"));
            for (auto& property_name: it2->GetPropertyList())
               g_variant_builder_add(&ab, "s", property_name.c_str());
            g_variant_builder_add(&b, "as", &ab);

            GError* err{};
            g_dbus_connection_emit_signal(
               m_connection,
               nullptr,
               it->first.c_str(),
               "org.freedesktop.DBus.ObjectManager",
               "InterfacesRemoved",
               g_variant_builder_end(&b),
               &err
            );
            if (err)
            {
               g_warning("Error emitting InterfacesRemoved for deleted interface: %s", err->message);
               g_error_free(err);
            }

            it->second.erase(it2);
            if (it->second.empty())
               m_objects.erase(it);
            
            break;
         }
      }
   }
}


void ObjectManager::Call(
   GDBusConnection*,
   const gchar* sender,
   const gchar* path,
   const gchar* interface,
   const gchar* method,
   GVariant* parameters,
   GDBusMethodInvocation* invocation,
   gpointer user_data)
{
   auto self = (ObjectManager*)user_data;
   assert(path == self->m_base_path);
   g_info("--> %s ObjectManager::%s", path, method);

   if (g_str_equal(method, "GetManagedObjects"))
   {
      GVariantBuilder b = G_VARIANT_BUILDER_INIT(G_VARIANT_TYPE("a{oa{sa{sv}}}"));
      for (auto& object: self->m_objects)
      {
         GVariantBuilder ob = G_VARIANT_BUILDER_INIT(G_VARIANT_TYPE("a{sa{sv}}"));
         for (auto& iface: object.second)
         {
            GVariantBuilder ib = G_VARIANT_BUILDER_INIT(G_VARIANT_TYPE("a{sv}"));
            for (auto& propertyname: iface.GetPropertyList())
               g_variant_builder_add(&ib, "{sv}", propertyname.c_str(), iface.GetProperty(propertyname));
            g_variant_builder_add(&ob, "{sa{sv}}", iface.name.c_str(), &ib);
         }
         g_variant_builder_add(&b, "{oa{sa{sv}}}", object.first.c_str(), &ob);
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