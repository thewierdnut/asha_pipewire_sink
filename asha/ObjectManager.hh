#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <map>

#include <gio/gio.h>

namespace asha
{

// Wrap a dbus object manager interface, used to introspect the available
// interfaces for an object.
// TODO: I'm not sure I'm ok with this design, as it registers an object with
//       only one interface. I should probably create an abstraction that
//       allows us to combine multiple interfaces into a single object, but
//       I would have to come up with a way to do that that is simpler to
//       use than the native gdbus interface.
class ObjectManager final
{
public:
   ObjectManager(GDBusConnection* connection, const std::string& base_path);
   ~ObjectManager();

   template <typename I>
   void AddInterface(const std::string& path, const std::string& iface_name, const I& iface)
   {
      m_objects[path].emplace_back(IfaceInfo{
         .path = path,
         .name = iface_name,
         .GetPropertyList = std::bind(&I::GetPropertyList, &iface),
         .GetProperty = std::bind(&I::GetProperty, &iface, std::placeholders::_1),
      });
      AddInterface(path, iface_name);
   }

   void RemoveInterface(const std::string& path, const std::string& iface_name);

protected:
   void AddInterface(const std::string& path, const std::string& iface_name);

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
   GDBusConnection* m_connection = nullptr;
   uint32_t m_object_id = -1;
   const std::string m_base_path;

   struct IfaceInfo
   {
      std::string path;
      std::string name;
      std::function<std::vector<std::string>()> GetPropertyList;
      std::function<GVariant*(const std::string&)> GetProperty;
   };

   std::map<const std::string, std::vector<IfaceInfo>> m_objects;
};


}