#include "Characteristic.hh"
#include "GVariantDump.hh"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <sstream>

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
   m_cancellable.reset(g_cancellable_new(), g_object_unref);
}


Characteristic::~Characteristic()
{
   if (m_cancellable)
      g_cancellable_cancel(m_cancellable.get());
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


void Characteristic::Read(std::function<void(const std::vector<uint8_t>&)> cb)
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

   Call(READ_VALUE, args, [=](const std::shared_ptr<GVariant>& result){
      if (result)
      {
         if (!g_variant_check_format_string(result.get(), "(ay)", false))
         {
            g_warning("Incorrect type signature when reading characteristic: %s", g_variant_get_type_string(result.get()));
         }
         else if (cb)
         {
            gsize length = 0;
            std::shared_ptr<GVariant> ay(g_variant_get_child_value(result.get(), 0), g_variant_unref);
            guint8* data = (guint8*)g_variant_get_fixed_array(ay.get(), &length, sizeof(guint8));
            std::vector<uint8_t> ret(data, data + length);
            cb(ret);
         }
      }
   });
}

void Characteristic::Write(const std::vector<uint8_t>& bytes, std::function<void(bool)> cb)
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

   Call(WRITE_VALUE, args, [=](const std::shared_ptr<GVariant>& result){
      if (result)
      {
         if (!g_variant_check_format_string(result.get(), "()", false))
            g_warning("Incorrect type signature when writing characteristic: %s", g_variant_get_type_string(result.get()));
         if (cb)
            cb(true);
      }
      else if (cb)
         cb(false);
   });
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

   Call(WRITE_VALUE, args);

   return true;
}

void Characteristic::Notify(std::function<void(const std::vector<uint8_t>&)> fn)
{
   StopNotify();

   // lambda doesn't work with typecasts required for slots.
   typedef void(*NotifyCallback)(GDBusProxy* self, GVariant* changed_properties, char** invalidated_properties, gpointer user_data);
   static NotifyCallback cb = (NotifyCallback)[](GDBusProxy* self, GVariant* changed_properties, char** invalidated_properties, gpointer user_data)
   {
      std::stringstream ss;
      ss << changed_properties;
      auto* characteristic = (Characteristic*)user_data;
      // g_info("Property %s notified: %s", characteristic->m_uuid.c_str(), ss.str().c_str());
      
      if (!g_variant_check_format_string(changed_properties, "a{sv}", false))
      {
         g_warning("Incorrect type signature when changed property %s: %s", characteristic->m_path.c_str(), g_variant_get_type_string(changed_properties));
         return;
      }

      GVariant* value = g_variant_lookup_value(changed_properties, "Value", G_VARIANT_TYPE_BYTESTRING);
      if (!value)
      {
         // I don't think this is an error, but it isn't what we are
         // watching for.
         return;
      }
      std::shared_ptr<GVariant> pvalue(value, g_variant_unref);

      if (!g_variant_check_format_string(value, "ay", false))
      {
         g_warning("Changed Value is not a byte array for %s: %s", characteristic->m_path.c_str(), g_variant_get_type_string(value));
         return;
      }

      gsize length = 0;
      const guint8* data = (const guint8*)g_variant_get_fixed_array(value, &length, sizeof(guint8));

      std::vector<uint8_t> ret(data, data + length);
      characteristic->m_notify_callback(std::vector<uint8_t>(data, data + length));
   };
   
   
   Call(START_NOTIFY, nullptr, [this, fn](const std::shared_ptr<GVariant>& result){
      if (result)
      {
         if (!g_variant_check_format_string(result.get(), "()", false))
         {
            g_warning("Incorrect return type signature when subscribing to %s notifications: %s", m_uuid.c_str(), g_variant_get_type_string(result.get()));
            return;
         }
         m_notify_callback = fn;
         m_notify_handler_id = g_signal_connect(m_char.get(),
            "g-properties-changed",
            G_CALLBACK(cb),
            this
         );

      }
   });
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


void Characteristic::Call(const char* fname, const std::shared_ptr<GVariant>& args, std::function<void(const std::shared_ptr<GVariant>&)> cb) noexcept
{
   CreateProxyIfNotAlreadyCreated();

   if (m_char)
   {
      struct CallbackContext
      {
         std::string fname;
         std::string uuid;
         std::function<void(const std::shared_ptr<GVariant>&)> cb;
      };

      g_dbus_proxy_call(m_char.get(), fname, args.get(), G_DBUS_CALL_FLAGS_NONE, 5000,
         m_cancellable.get(),
         [](GObject* proxy, GAsyncResult* res, gpointer data) {
            CallbackContext* cc = (CallbackContext*)data;
            std::unique_ptr<CallbackContext> ccdeleter(cc);
            GError* e = nullptr;
            GVariant* result = g_dbus_proxy_call_finish((GDBusProxy*)proxy, res, &e);
            if (e)
            {
               // TODO: knowing the severity of the error here depends on context
               g_warning("Error calling %s(%s): %s", cc->fname.c_str(), cc->uuid.c_str(), e->message);
               g_error_free(e);
               if (cc->cb)
                  cc->cb(nullptr);
            }
            if (result)
            {
               std::shared_ptr<GVariant> p(result, g_variant_unref);
               if (cc->cb)
                  cc->cb(p);
            }
         },
         new CallbackContext{fname, m_uuid, cb}
      );
   }
}
