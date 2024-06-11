#include "Side.hh"

#include <cassert>
#include <cstring>

#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>
#include <glib-2.0/glib.h>
#include <sys/socket.h>
#include <unistd.h>


using namespace asha;

namespace
{

constexpr char ASHA_READ_ONLY_PROPERTIES[] = "6333651e-c481-4a3e-9169-7c902aad37bb";
constexpr char ASHA_AUDIO_CONTROL_POINT[]  = "f0d4de7e-4a88-476c-9d9f-1937b0996cc0";
constexpr char ASHA_AUDIO_STATUS[]         = "38663f1a-e711-4cac-b641-326b56404837";
constexpr char ASHA_VOLUME[]               = "00e4ca9e-ab14-41e4-8823-f9e70c7e91df";
constexpr char ASHA_LE_PSM_OUT[]           = "2d410339-82b6-42aa-b34e-e2e01df8cc1a";

namespace Control
{
   constexpr uint8_t START = 1;  // followed by codec, audiotype, volume, otherstate
   constexpr uint8_t STOP = 2;   // no other args
   constexpr uint8_t STATUS = 3; // Connection status, or parameter update with interval
}

namespace Update
{
   constexpr uint8_t OTHER_DISCONNECTED = 0;
   constexpr uint8_t OTHER_CONNECTED = 1;
   constexpr uint8_t PARAMETERS_UPDATED = 2;
}

}


std::shared_ptr<Side> Side::CreateIfValid(const Bluetooth::BluezDevice& device)
{
   // Don't return the side unless the correct asha charachteristics exist.
   std::shared_ptr<Side> side(new Side);
   for(auto& c: device.characteristics)
   {
      if      (c.UUID() == ASHA_READ_ONLY_PROPERTIES) side->m_char.properties = c;
      else if (c.UUID() == ASHA_AUDIO_CONTROL_POINT)  side->m_char.audio_control = c;
      else if (c.UUID() == ASHA_AUDIO_STATUS)         side->m_char.status = c;
      else if (c.UUID() == ASHA_VOLUME)               side->m_char.volume = c;
      else if (c.UUID() == ASHA_LE_PSM_OUT)           side->m_char.le_psm_out = c;
   }
   
   if (side->m_char.properties && side->m_char.audio_control &&
       side->m_char.status && side->m_char.volume && side->m_char.le_psm_out)
   {
      //side->m_timer.reset(g_timer_new(), g_timer_destroy);
      side->m_mac = device.mac;
      side->m_name = device.name;
      side->m_alias = device.alias;
      return side;
   }
   else
   {
      return nullptr;
   }
}


Side::~Side()
{

}


std::string Side::Description() const
{
   if (m_properties.capabilities & CAPABILITY_BINAURAL)
   {
      if (m_properties.capabilities & CAPABILITY_RIGHT_SIDE)
         return m_name + " (Right)";
      else
         return m_name + " (Left)";
   }
   else
      return m_name;
}

Side::Status Side::ReadStatus()
{
   auto result = m_char.status.Read();
   return (Status)(result.empty() ? Status::CALL_FAILED : result[0]);
}

bool Side::ReadProperties()
{
   // Query the device properties.
   auto result = m_char.properties.Read();
   if (result.size() < sizeof(m_properties))
      return false;

   memcpy(&m_properties, result.data(), sizeof(m_properties));
   return true;
}

bool Side::Disconnect()
{
   if (m_sock != -1)
   {
      close(m_sock);
      m_sock = -1;
      return true;
   }
   return false;
}


bool Side::Connect()
{
   // Retrieve the PSM to connect to.
   auto result = m_char.le_psm_out.Read();
   if (result.size() < 2) return false;

   // Should be two bytes.
   if (result.empty())
      return false;
   m_psm_id = result[0];
   if (result.size() > 1)
      m_psm_id |= result[1] << 8;
   
   bool ret = Reconnect();
   EnableStatusNotifications();
   return ret;
}

bool Side::Reconnect()
{
   assert(m_sock == -1);
   m_sock = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
   struct sockaddr_l2 addr{};
   addr.l2_family = AF_BLUETOOTH;
   addr.l2_bdaddr_type = BDADDR_LE_PUBLIC;
   if (0 != bind(m_sock, (struct sockaddr*)&addr, sizeof(addr)))
   {
      int e = errno;
      g_error("Failed to bind l2cap socket: %s", strerror(e));
      close(m_sock);
      m_sock = -1;
      return false;
   }
   
   addr.l2_psm = htobs(m_psm_id);
   sscanf(m_mac.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
      &addr.l2_bdaddr.b[5],&addr.l2_bdaddr.b[4],&addr.l2_bdaddr.b[3],
      &addr.l2_bdaddr.b[2],&addr.l2_bdaddr.b[1],&addr.l2_bdaddr.b[0]
   );
   // str2ba(mac.c_str(), &addr.l2_bdaddr);

   // SOL_L2CAP options don't handle the new CoC mode. Use SOL_BLUETOOTH instead
   uint8_t mode = BT_MODE_LE_FLOWCTL;
   if (0 != setsockopt(m_sock, SOL_BLUETOOTH, BT_MODE, &mode, sizeof(mode)))
   {
      int e = errno;
      g_error("Unable to set CoC flow control mode: %s", strerror(e));

      if (e == ENOPROTOOPT)
      {
         g_error("Please make sure that the bluetooth kernel module is being loaded with enable_ecred=1.");
      }
      close(m_sock);
      m_sock = -1;
      return false;
   }


   int err = 0;
   bool success = false;
   for (size_t i = 0; i < 10; ++i)
   {
      if (connect(m_sock, (struct sockaddr*)&addr, sizeof(addr)) == 0)
      {
         success = true;
         break;
      }
      
      err = errno;
      if (err != EBUSY && err != EAGAIN || err != EWOULDBLOCK)
         break;

      g_usleep(1000);
   }

   if (!success)
   {
      g_warning("Failed to connect l2cap channel: %s", strerror(err));
      close(m_sock);
      m_sock = -1;
      return false;
   }

   return true;
}


void Side::SetStreamVolume(int8_t volume)
{
   m_volume = volume;

   m_char.volume.Command({(uint8_t)m_volume});
}


void Side::SetDeviceVolume(int8_t volume)
{
   // TODO: This will probably be manufacturer specific.
   // Starkey:
   //    Volume is f3f594f9-e210-48f3-85e2-4b0cf235a9d3, range of 00 to ff
}


bool Side::EnableStatusNotifications()
{
   // Turn on status notifications.
   return m_char.status.Notify([this](const std::vector<uint8_t>& data) { OnStatusNotify(data); });
}

bool Side::DisableStatusNotifications()
{
   m_char.status.StopNotify();

   return true;
}

bool Side::Start(bool otherstate)
{
   uint32_t phys = 0;
   socklen_t size = sizeof(phys);
   
   // TODO: Bah. This still keeps posting false positives.
   // bool phy_2m_enabled = false;
   // // Give it 200 ms to finish phy negotiation.
   // for (size_t i = 0; i < 10 && !phy_2m_enabled; ++i)
   // {
   //    if (getsockopt(m_sock, SOL_BLUETOOTH, BT_PHY, &phys, &size) >= 0)
   //    {
   //       if (phys & BT_PHY_LE_2M_TX)
   //       {
   //          phy_2m_enabled = true;
   //          break;
   //       }
   //    }
   //    else
   //       break; // No point in polling. Something else is broken
   //    g_usleep(20000);
   // }
   // if (!phy_2m_enabled)
   // {
   //    g_warning("2M PHY not enabled");
   //    g_info("        Unless you enable LE_2M_TX, don't expect to be able to stream to more than one device.");
   //    g_info("        You can use `btmgmt phy` to check the supported phy's, and enable additional phy's by running a command like");
   //    g_info("           btmgmt phy BR1M1SLOT BR1M3SLOT BR1M5SLOT EDR2M1SLOT EDR2M3SLOT EDR2M5SLOT EDR3M1SLOT EDR3M3SLOT EDR3M5SLOT LE1MTX LE1MRX LE2MTX LE2MRX");
   //    g_info("        and then disconnecting and reconnecting your hearing devices.");
   //    g_info("        Note that some devices and adapters don't support 2M PHY's, despite advertising otherwise");
   // }

   static constexpr uint8_t G722_16KHZ = 1;
   m_ready_to_receive_audio = false;
   m_next_status_fn = [this](Status s) {
      m_ready_to_receive_audio = s == STATUS_OK;
   };
   return m_char.audio_control.Write({Control::START, G722_16KHZ, 0, (uint8_t)m_volume, (uint8_t)otherstate});
}

bool Side::Stop()
{
   m_ready_to_receive_audio = false;
   return m_char.audio_control.Write({Control::STOP});
}

Side::WriteStatus Side::WriteAudioFrame(const AudioPacket& packet)
{
   // Write 20ms of data. Should be exactly 160 bytes in size.
   static_assert(sizeof(packet) == 161, "We can only send 161 byte audio packets");
   // assert(m_sock != -1);
   WriteStatus ret = NOT_READY;
   if (m_sock != -1 && m_ready_to_receive_audio)
   {
      // ++m_packet_count;
      // double elapsed = g_timer_elapsed(m_timer.get(), nullptr);
      // if (elapsed > 10)
      // {
      //    // Each packet should be 320 samples.
      //    double rate = 320 * m_packet_count / elapsed;
      //    g_info("Receiving data at %u hz", (unsigned)rate);
      //    m_packet_count = 0;
      //    g_timer_start(m_timer.get());
      // }

      int bytes_sent = send(m_sock, &packet, sizeof(packet), MSG_DONTWAIT);
      int err = errno;
      if (bytes_sent == sizeof(packet))
         ret = WRITE_OK;
      else if (bytes_sent > (int)sizeof(packet))
      {
         g_warning("Ok, this has to be a kernel bug. We tried to send %zu bytes, but really sent %d", sizeof(packet), bytes_sent);
         ret = OVERSIZED;
      }
      else if (bytes_sent >= 0)
      {
         g_warning("Only sent %d out of %zu bytes", bytes_sent, sizeof(packet));
         ret = TRUNCATED;
      }
      else if (err == EAGAIN || err == EWOULDBLOCK)
      {
         g_warning("Dropping frame for %s", Description().c_str());
         ret = BUFFER_FULL;
      }
      else
      {
         g_warning("Disconnected from %s: (%s)", Description().c_str(), strerror(err));
         close(m_sock);
         m_sock = -1;
         m_ready_to_receive_audio = false;
         ret = DISCONNECTED;
      }
   }
   return ret;
}

bool Side::UpdateOtherConnected(bool connected)
{
   return m_char.audio_control.Command({Control::STATUS, (connected ? Update::OTHER_CONNECTED : Update::OTHER_DISCONNECTED)});
}

bool Side::UpdateConnectionParameters(uint8_t interval)
{
   return m_char.audio_control.Command({Control::STATUS, Update::PARAMETERS_UPDATED, interval});
}

void Side::OnStatusNotify(const std::vector<uint8_t>& data)
{
   if (m_next_status_fn && !data.empty())
   {
      m_next_status_fn((Status)data.front());
      m_next_status_fn = std::function<void(Status)>();
   }
}