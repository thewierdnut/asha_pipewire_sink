#pragma once

#include <string>
#include <memory>
#include <map>
#include <sstream>
#include <functional>

struct _GDBusProxy;
struct _GVariant;

namespace asha
{

// Model dbus properties object.
class Properties final
{
public:
   Properties() {}
   Properties(const std::string& path): m_path(path) {}
   Properties(const Properties& p): m_path(p.m_path) {}
   Properties(Properties&& p) { *this = std::move(p); }
   Properties& operator=(Properties&& p) { m_path = std::move(p.m_path); m_prop = std::move(p.m_prop); return *this; }
   Properties& operator=(const Properties& p) { m_path = p.m_path; return *this; }
   ~Properties();

   std::shared_ptr<_GVariant> Get(const std::string& s);
   
   typedef std::function<void(const std::string&, const std::shared_ptr<_GVariant>&)> UpdatedCallback;
   void Subscribe(UpdatedCallback cb);

protected:
   void EnsureConnected();

private:
   std::string m_path;
   std::shared_ptr<_GDBusProxy> m_prop;

   UpdatedCallback m_cb;
};


}