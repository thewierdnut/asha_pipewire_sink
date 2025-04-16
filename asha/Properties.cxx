#include "Properties.hh"

#include <gio/gio.h>

using namespace asha;

namespace
{
   constexpr char PROPERTY_INTERFACE[] = "org.freedesktop.DBus.Properties";
}


Properties& Properties::operator=(Properties&& p)
{
   m_interface = std::move(p.m_interface);
   m_path = std::move(p.m_path);
   m_prop = std::move(p.m_prop);
   return *this;
}


Properties& Properties::operator=(const Properties& p)
{
   m_interface = p.m_interface;
   m_path = p.m_path;
   return *this;
}


Properties::~Properties()
{
   // Stub, to make sure destructor compiles here.
}


void Properties::EnsureConnected()
{
   if (m_prop) return;
   // g_debug("Creating proxy for %s for path %s", PROPERTY_INTERFACE, m_path.c_str());
   GError* err = nullptr;
   m_prop.reset(g_dbus_proxy_new_for_bus_sync(
      G_BUS_TYPE_SYSTEM,
      G_DBUS_PROXY_FLAGS_NONE,
      nullptr,
      "org.bluez",
      m_path.c_str(),
      PROPERTY_INTERFACE,
      nullptr,
      &err
   ), g_object_unref);

   if (err)
   {
      g_error("Error getting dbus %s proxy: %s", PROPERTY_INTERFACE, err->message);
      g_error_free(err);
      m_prop.reset();
      return;
   }
}


std::shared_ptr<GVariant> Properties::Get(const std::string& s)
{
   EnsureConnected();

   GVariant* args = g_variant_new("(ss)", m_interface.c_str(), s.c_str());

   GError* e = nullptr;
   GVariant* result = g_dbus_proxy_call_sync(m_prop.get(),
      "Get",
      args,
      G_DBUS_CALL_FLAGS_NONE,
      -1,
      nullptr,
      &e
   );
   if (e)
   {
      // TODO: knowing the severity of the error here depends on context
      g_warning("Error calling retrieving property %s: %s", s.c_str(), e->message);
      g_error_free(e);
   }
   if (result)
   {
      std::shared_ptr<GVariant> p(result, g_variant_unref);

      if (!g_variant_check_format_string(result, "(v)", false))
      {
         g_warning("Incorrect type signature when reading property %s: %s", m_path.c_str(), g_variant_get_type_string(result));
         return {};
      }

      return std::shared_ptr<GVariant>(g_variant_get_child_value(result, 0), g_variant_unref);
   }
   return {};
}


void Properties::Subscribe(UpdatedCallback cb)
{
   EnsureConnected();
   m_cb = cb;
   // Lambda doesn't work with G_CALLBACK
   struct Call {
      static void Back(GDBusProxy* p, gchar* sender, gchar* signal, GVariant* parameters, gpointer* user_data)
      {
         auto* self = (Properties*)user_data;

         if (g_str_equal(signal, "PropertiesChanged"))
         {
            // iface, changed_properties, invalidated_properties
            GVariantIter* it_changed_properties{};
            GVariantIter* it_invalidated_properties{};

            gchar* iface_name = nullptr;
            g_variant_get(parameters, "(sa{sv}as)", &iface_name, &it_changed_properties, &it_invalidated_properties);
            bool correct_interface = false;
            if (iface_name)
            {
               correct_interface = iface_name == self->m_interface;
               g_free(iface_name);
            }
            
            gchar* key{};
            GVariant* value{};
            if (it_changed_properties)
            {
               if (correct_interface)
               {
                  while (g_variant_iter_loop(it_changed_properties, "{&sv}", &key, &value))
                     self->m_cb(key ? key : "", std::shared_ptr<GVariant>(g_variant_ref(value), g_variant_unref));
               }
               g_variant_iter_free(it_changed_properties);
            }
            if (it_invalidated_properties)
            {
               if (correct_interface)
               {
                  while (g_variant_iter_loop(it_invalidated_properties, "&s", &key))
                     self->m_cb(key ? key : "", nullptr);
               }
               g_variant_iter_free(it_invalidated_properties);
            }
         }
      }
   };
   // Despite the documentation, this doesn't seem to ever fire. g-signal works though.
   // g_signal_connect(iface.get(),
   //    "g-properties-changed",
   //    G_CALLBACK(&Callback::PropertiesChanged),
   //    this
   // );

   g_signal_connect(m_prop.get(),
      "g-signal",
      G_CALLBACK(&Call::Back),
      this
   );
}

