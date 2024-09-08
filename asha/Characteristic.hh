#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct _GDBusProxy;
struct _GVariant;
struct _GCancellable;

namespace asha
{

static constexpr char CHARACTERISTIC_INTERFACE[] = "org.bluez.GattCharacteristic1";


// Abstraction of bluez interface using dbus
class Characteristic final
{
public:
   Characteristic() {}
   Characteristic(const std::string& uuid, const std::string& path);
   ~Characteristic();

   Characteristic& operator=(const Characteristic& o);

   const std::string& UUID() const { return m_uuid; }

   // Read the given Gatt characteristic.
   void Read(std::function<void(const std::vector<uint8_t>&)> cb);
   // Write to the given Gatt characteristic.
   void Write(const std::vector<uint8_t>& bytes, std::function<void(bool)> cb);
   // Command the given Gatt characteristic.
   bool Command(const std::vector<uint8_t>& bytes);
   // When the given Gatt characteristic is notified, call the given function.
   void Notify(std::function<void(const std::vector<uint8_t>&)> fn);
   void StopNotify();

   operator bool() const { return !m_uuid.empty(); }

   

protected:
   void CreateProxyIfNotAlreadyCreated() noexcept;

   void Call(
      const char* fname,
      const std::shared_ptr<_GVariant>& args = nullptr,
      std::function<void(const std::shared_ptr<_GVariant>&)> cb = std::function<void(const std::shared_ptr<_GVariant>&)>{}
   ) noexcept;

private:
   std::shared_ptr<_GDBusProxy> m_char;
   
   std::string m_uuid;
   std::string m_path;

   unsigned long m_notify_handler_id = -1;
   std::function<void(const std::vector<uint8_t>&)> m_notify_callback;
   std::shared_ptr<_GCancellable> m_cancellable;

};

}