#pragma once

#include <cstdint>
#include "Properties.hh"
#include <memory>

struct _GDBusProxy;
struct _GDBusConnection;
struct _GError;
struct _GVariant;
struct _GDBusMethodInvocation;

namespace asha
{

// Class that monitors for bluetooth advertisement protocols.
// TODO: The kernel implementation appears to be broken. With btmon, I can see
//       bluez registering for an advertisement monitor with handle 0x0001, and
//       then advertisement responses coming back with handle 0, which bluez
//       discards.
// TODO: I think bluez utilizes a microsoft extension for this feature, but it
//       will also use these callbacks if you turn on the add device dialog, so
//       there is obviously some sort of software fallback.
// Alternatives:
//    Randomly attempt to connect to devices.
//    Turn on scanning, monitor the rssi ourselves.
//       My hearing aids report an rssi of -80 most of the time, unless
//       I literally hang them from the antenna.
//    Turn on scanning, rely on AdvertisementMonitor, which kinda-sorta-works
//    when you do that.
//    Use adapter filter on mac addresses or services relevant to us.
//       How does it filter on services if it doesn't do device discovery? it
//       has to rely on service announcements, right?
//       Doing StartDiscovery is not passive. It will contact devices to
//       resolve mac addresses. This might be a good thing if hearing devices
//       use random mac addresses (mine don't)
class BluetoothMonitor final
{
public:
   BluetoothMonitor();
   ~BluetoothMonitor();

   void SetRssiPaired(int16_t rssi);
   void SetRssiUnpaired(int16_t rssi);
   void EnableRssiLogging(bool e);

protected:
   void RegisterBasePath();

   static void DBusCall(
      struct _GDBusConnection*,
      const char* sender,
      const char* path,
      const char* interface,
      const char* method,
      struct _GVariant* parameters,
      struct _GDBusMethodInvocation* invocation,
      void* user_data);
   static _GVariant* GetProperty(
      struct _GDBusConnection* connection,
      const char* sender,
      const char* object_path,
      const char* interface_name,
      const char* property_name,
      struct _GError** error,
      void* user_data);

private:
   class Path;
   class Monitor;

   std::shared_ptr<Path> m_path;

   uint32_t m_object_id = -1;
};

}
