#pragma once

#include <memory>
#include <vector>

struct _GDBusConnection;
struct _GDBusMethodInvocation;
struct _GDBusProxy;
struct _GError;
struct _GVariant;


namespace asha
{

class ObjectManager;

// Register as having support for the asha profile, so that bluez will
// automatically reconnect when it sees the asha uuid.
class GattProfile final
{
public:
   GattProfile();
   ~GattProfile();

   std::vector<std::string> GetPropertyList() const;
   _GVariant* GetProperty(const std::string& name) const;

protected:
   void CreateProfile();
   void RegisterApplication();
   void UnregisterApplication();
   void DestroyProfile();

   static void Call(
      struct _GDBusConnection*,
      const char* sender,
      const char* path,
      const char* interface,
      const char* method,
      struct _GVariant* parameters,
      struct _GDBusMethodInvocation* invocation,
      void* user_data);
   static _GVariant* GetDbusProperty(
      struct _GDBusConnection* connection,
      const char* sender,
      const char* object_path,
      const char* interface_name,
      const char* property_name,
      struct _GError** error,
      void* user_data);

private:
   std::shared_ptr<_GDBusProxy> m_gatt;
   std::shared_ptr<ObjectManager> m_om;
   uint32_t m_object_id = -1;
};


}