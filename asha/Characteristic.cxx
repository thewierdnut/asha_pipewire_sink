#include "Characteristic.hh"
#include "GVariantDump.hh"

#include <iostream>
#include <memory>
#include <stdexcept>

#include <gio/gio.h>

using namespace asha;

namespace
{
   constexpr char READ_VALUE[] = "ReadValue";
   constexpr char WRITE_VALUE[] = "WriteValue";
   constexpr char START_NOTIFY[] = "StartNotify";
   constexpr char STOP_NOTIFY[] = "StopNotify";
}


Characteristic::Characteristic(const std::string& uuid, const std::string& path):
   m_uuid(uuid),
   m_path(path)
{

}

Characteristic::~Characteristic()
{
   StopNotify();
}

Characteristic& Characteristic::operator=(const Characteristic& o)
{
   StopNotify();
   m_char.reset();
   m_uuid = o.m_uuid;
   m_path = o.m_path;
   return *this;
}


std::vector<uint8_t> Characteristic::Read()
{
   // Args needs to be a tuple containing dict options. (dbus dicts are arrays
   // of key/value pairs).
   GVariantBuilder b = G_VARIANT_BUILDER_INIT(G_VARIANT_TYPE("a{sv}"));
   g_variant_builder_add(&b, "{sv}", "offset", g_variant_new_uint16(0));
   
   GVariantBuilder ab = G_VARIANT_BUILDER_INIT(G_VARIANT_TYPE("(a{sv})"));
   g_variant_builder_add_value(&ab, g_variant_builder_end(&b));

   std::shared_ptr<GVariant> args(g_variant_builder_end(&ab), g_variant_unref);
   
   // If you want to know why this is here, search google for "glib floating
   // reference", and be prepared to get very, very angry.
   g_variant_ref_sink(args.get());

   GError* e = nullptr;
   auto result = Call(READ_VALUE, args);

   if (!result)
      return {};

   if (!g_variant_check_format_string(result.get(), "(ay)", false))
   {
      g_warning("Incorrect type signature when reading %s: %s", m_path.c_str(), g_variant_get_type_string(result.get()));
      return {};
   }

   gsize length = 0;
   std::shared_ptr<GVariant> ay(g_variant_get_child_value(result.get(), 0), g_variant_unref);
   guint8* data = (guint8*)g_variant_get_fixed_array(ay.get(), &length, sizeof(guint8));


   std::vector<uint8_t> ret(data, data + length);

   ay.reset();
   result.reset();
   args.reset();


   return ret;
}

bool Characteristic::Write(const std::vector<uint8_t>& bytes)
{
   // Args is a tuple containing a byte aray and the dict options.
   GVariantBuilder b = G_VARIANT_BUILDER_INIT(G_VARIANT_TYPE("a{sv}"));
   g_variant_builder_add(&b, "{sv}", "offset", g_variant_new_uint16(0));
   g_variant_builder_add(&b, "{sv}", "type", g_variant_new_string("request"));
   
   GVariantBuilder ab = G_VARIANT_BUILDER_INIT(G_VARIANT_TYPE("(aya{sv})"));
   g_variant_builder_add_value(&ab, g_variant_new_fixed_array(G_VARIANT_TYPE("y"), bytes.data(), bytes.size(), sizeof(gint8)));
   g_variant_builder_add_value(&ab, g_variant_builder_end(&b));

   std::shared_ptr<GVariant> args(g_variant_builder_end(&ab), g_variant_unref);
   
   // If you want to know why this is here, search google for "glib floating
   // reference", and be prepared to get very, very angry.
   g_variant_ref_sink(args.get());

   GError* e = nullptr;
   auto result = Call(WRITE_VALUE, args);

   if (!result)
      return false;

   if (!g_variant_check_format_string(result.get(), "()", false))
   {
      g_warning("Incorrect type signature when reading %s: %s", m_path.c_str(), g_variant_get_type_string(result.get()));
      return false;
   }
   return true;
}

bool Characteristic::Command(const std::vector<uint8_t>& bytes)
{
   // Args is a tuple containing a byte aray and the dict options.
   GVariantBuilder b = G_VARIANT_BUILDER_INIT(G_VARIANT_TYPE("a{sv}"));
   g_variant_builder_add(&b, "{sv}", "offset", g_variant_new_uint16(0));
   g_variant_builder_add(&b, "{sv}", "type", g_variant_new_string("command"));
   
   GVariantBuilder ab = G_VARIANT_BUILDER_INIT(G_VARIANT_TYPE("(aya{sv})"));
   g_variant_builder_add_value(&ab, g_variant_new_fixed_array(G_VARIANT_TYPE("y"), bytes.data(), bytes.size(), sizeof(gint8)));
   g_variant_builder_add_value(&ab, g_variant_builder_end(&b));

   std::shared_ptr<GVariant> args(g_variant_builder_end(&ab), g_variant_unref);

   // If you want to know why this is here, search google for "glib floating
   // reference", and be prepared to get very, very angry.
   g_variant_ref_sink(args.get());

   GError* e = nullptr;
   auto result = Call(WRITE_VALUE, args);

   if (!result)
      return false;

   if (!g_variant_check_format_string(result.get(), "()", false))
   {
      g_warning("Incorrect type signature when reading %s: %s", m_path.c_str(), g_variant_get_type_string(result.get()));
      return false;
   }
   return true;
}

bool Characteristic::Notify(std::function<void()> fn)
{
   // No args for the dbus call.

   GError* e = nullptr;
   auto result = Call(START_NOTIFY);

   if (!result)
      return false;

   if (!g_variant_check_format_string(result.get(), "()", false))
   {
      g_warning("Incorrect return type signature for %s: %s", m_path.c_str(), g_variant_get_type_string(result.get()));
      return false;
   }

   // lambda doesn't work here for some reason.
   struct Call
   {
      static void Back(GDBusProxy* self, GVariant* changed_properties, char** invalidated_properties, gpointer user_data)
      {
         std::cout << "Characteristic g-properties-changed: " << changed_properties << '\n';
         ((Characteristic*)user_data)->m_notify_callback();
      }
   };

   m_notify_callback = fn;
   m_notify_handler_id = g_signal_connect(m_char.get(),
      "g-properties-changed",
      G_CALLBACK(&Call::Back),
      this
   );
   return true;
}


void Characteristic::StopNotify()
{
   // Unregister for any notifications.
   if (m_char && m_notify_handler_id != -1)
   {
      Call(STOP_NOTIFY);
      g_signal_handler_disconnect(m_char.get(), m_notify_handler_id);
   }
}


void Characteristic::CreateProxyIfNotAlreadyCreated() noexcept
{
   // What a great function name!
   if (m_char) return;

   GError* err = nullptr;
   m_char.reset(g_dbus_proxy_new_for_bus_sync(
      G_BUS_TYPE_SYSTEM,
      G_DBUS_PROXY_FLAGS_NONE,
      nullptr,
      "org.bluez",
      m_path.c_str(),
      CHARACTERISTIC_INTERFACE,
      nullptr,
      &err
   ), g_object_unref);

   if (err)
   {
      g_error("Error getting dbus %s proxy: %s", CHARACTERISTIC_INTERFACE, err->message);
      g_error_free(err);
      m_char.reset();
      return;
   }
}


std::shared_ptr<_GVariant> Characteristic::Call(const char* fname, const std::shared_ptr<_GVariant>& args) noexcept
{
   CreateProxyIfNotAlreadyCreated();

   if (m_char)
   {
      GError* e = nullptr;
      // Cannot directly capture result into a shared_ptr because the shared_ptr
      // will happily delete a null pointer, which g_variant_unref does not like.
      GVariant* result = g_dbus_proxy_call_sync(m_char.get(),
         fname,
         args.get(),
         G_DBUS_CALL_FLAGS_NONE,
         -1,
         nullptr,
         &e
      );
      if (e)
      {
         g_error("Error calling %s: %s", fname, e->message);
         g_error_free(e);
      }
      if (result)
      {
         // Convert from floating reference to a full reference.
         // g_variant_ref_sink(result);
         return std::shared_ptr<GVariant>(result, g_variant_unref);
      }
      else
      {
         g_warning("Null result when calling %s", fname);
      }
   }

   return nullptr;
}
