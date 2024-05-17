#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct _GDBusProxy;
struct _GVariant;

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
   std::vector<uint8_t> Read();
   // Write to the given Gatt characteristic.
   bool Write(const std::vector<uint8_t>& bytes);
   // Command the given Gatt characteristic.
   bool Command(const std::vector<uint8_t>& bytes);
   // When the given Gatt characteristic is notified, call the given function.
   bool Notify(std::function<void()> fn);
   void StopNotify();

   operator bool() const { return !m_uuid.empty(); }

protected:
   void CreateProxyIfNotAlreadyCreated() noexcept;

   std::shared_ptr<_GVariant> Call(const char* fname, const std::shared_ptr<_GVariant>& args = nullptr) noexcept;

private:
   std::shared_ptr<_GDBusProxy> m_char;
   
   std::string m_uuid;
   std::string m_path;

   unsigned long m_notify_handler_id = -1;
   std::function<void()> m_notify_callback;
};

}