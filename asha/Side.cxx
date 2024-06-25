#include "Side.hh"

#include "HexDump.hh"
#include "RawHci.hh"

#include <cassert>
#include <cstring>
#include <sstream>

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

constexpr char HA_STATUS[]                 = "38278651-76d7-4dee-83d8-894f3fa6bb99";
constexpr char EXTERNAL_VOLUME[]           = "f3f594f9-e210-48f3-85e2-4b0cf235a9d3";
constexpr char BATTERY_10[]                = "24e1dff3-ae90-41bf-bfbd-2cf8df42bf87";
constexpr char BATTERY_100[]               = "60fb6208-9b02-468e-aba8-b702dd6f543a";

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
      else if (c.UUID() == HA_STATUS)                 side->m_char.ha_status = c;
      else if (c.UUID() == EXTERNAL_VOLUME)           side->m_char.external_volume = c;
      else if (c.UUID() == BATTERY_10)                side->m_char.battery_10 = c;
      else if (c.UUID() == BATTERY_100)               side->m_char.battery_100 = c;
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


void Side::SubscribeExtra(/* callbacks? */)
{
   if (m_char.ha_status)
      m_char.ha_status.Notify([this](const std::vector<uint8_t> &bytes) { OnHAStatus(bytes); });

   if (m_char.battery_100)
      m_char.battery_100.Notify([this](const std::vector<uint8_t> &bytes) { if(!bytes.empty()) OnBattery(bytes[0]); });
   else if (m_char.battery_10)
      m_char.battery_100.Notify([this](const std::vector<uint8_t> &bytes) { if(!bytes.empty()) OnBattery(bytes[0] * 10); });

   if (m_char.external_volume)
      m_char.external_volume.Notify([this](const std::vector<uint8_t> &bytes) { if (!bytes.empty()) OnExternalVolume(bytes[0]); } );
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

   RawHci hci(m_mac, m_sock);
   // This requires CAP_NET_RAW
   if (hci.SendConnectionUpdate(16, 16, 10, 100, 12, 12))
   {
      // This succeeded. Notify the device.
      UpdateConnectionParameters(16);
   }
   else
   {
      // This failed (probably don't have requisite permissions). Extract the
      // configured value using an unpriviledge request, and notify of that instead.
      RawHci::SystemConfig config;
      hci.ReadSysConfig(config);
      if (config.max_conn_interval == config.min_conn_interval && config.max_conn_interval <= 16)
         UpdateConnectionParameters(config.min_conn_interval);
      else
      {
         // This configuration isn't going to work.
         g_warning("The currently configured connection paramters will not work. "
                   "Please set these values in /etc/bluetooth/main.conf, and restart the bluetooth service.\n"
                   "  [LE]\n"
                   "  MinConnectionInterval=16\n"
                   "  MaxConnectionInterval=16\n"
                   "  ConnectionLatency=10\n"
                   "  ConnectionSupervisionTimeout=100");
      }
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
   const char* side = Left() ? "left " : "right";
   g_info("%s Sending ACP start other %s", side, otherstate ? "connected" : "not connected");

   static constexpr uint8_t G722_16KHZ = 1;
   m_ready_to_receive_audio = false;
   m_next_status_fn = [this](Status s) {
      m_ready_to_receive_audio = s == STATUS_OK;
   };
   return m_char.audio_control.Write({Control::START, G722_16KHZ, 0, (uint8_t)m_volume, (uint8_t)otherstate});
}

bool Side::Stop()
{
   const char* side = Left() ? "left " : "right";
   g_info("%s Sending ACP stop", side);

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
         g_info("Dropping frame for %s", Description().c_str());
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

void Side::ReadFromAudioSocket()
{
   // ASHA standard says nothing about receiving traffic here, but android
   // source says we get some statistics here.
   // I haven't personally observed any traffic here.
   if (m_sock != -1)
   {
      uint8_t buffer[512];
      ssize_t count = recv(m_sock, buffer, sizeof(buffer), MSG_DONTWAIT);
      if (count > 0)
      {
         const char* side = Left() ? "left " : "right";
         for (int i = 0; i + 4 <= count; i += 4)
         {
            uint16_t events = *(uint16_t*)(buffer + i);
            uint16_t frames = *(uint16_t*)(buffer + i + 2);
            g_info("%s read %hu events %hu frames", side, events, frames);
         }
      }
   }
}

bool Side::UpdateOtherConnected(bool connected)
{
   const char* side = Left() ? "left " : "right";
   g_info("%s Sending ACP status other %s", side, connected ? "connected" : "not connected");
   return m_char.audio_control.Command({Control::STATUS, (connected ? Update::OTHER_CONNECTED : Update::OTHER_DISCONNECTED)});
}

bool Side::UpdateConnectionParameters(uint8_t interval)
{
   const char* side = Left() ? "left " : "right";
   g_info("%s Sending ACP status parameters updated %hhu", side, interval);
   return m_char.audio_control.Command({Control::STATUS, Update::PARAMETERS_UPDATED, interval});
}

void Side::OnStatusNotify(const std::vector<uint8_t>& data)
{
   const char* side = Left() ? "left " : "right";
   if (!data.empty())
      g_info("%s AshaStatus: %hhu", side, data.front());
   if (m_next_status_fn && !data.empty())
   {
      m_next_status_fn((Status)data.front());
      m_next_status_fn = std::function<void(Status)>();
   }
}

void Side::OnHAStatus(const std::vector<uint8_t>& data)
{
   // 38278651-76d7-4dee-83d8-894f3fa6bb99 notifications
   // I've partially figured out what some of this stuff means.
   const char* side = Left() ? "left " : "right";
   if (data.size() > 2)
   {
      // Header is two bytes, but I'm not sure how its broken up.
      // Reading this as big-endian for convenience.
      uint16_t header = data[0] << 8 | data[1];
      switch(header)
      {
      case 0x0014: // Volume changed
         if (data.size() == 5)
         {
            g_info("%s OnHaStatus(Muted: %hhu, Volume: %hhu, ??: %02hhx)", side, data[2], data[3], data[4]);
            return;
         }
         break;
      case 0x0194: // Not sure... it turns on and off with muting and streaming
         if (data.size() == 3)
         {
            g_info("%s OnHAStatus(0194: %02hhx)", side, data[2]);
            return;
         }
         break;
      case 0x0034: // Stream state
         if (data.size() == 3)
         {
            // Both hearing aids:
            //    Sending ACP start goes to 3 at the same time we get asha status ok, then to 1 250ms later
            //    Sending ACP stop goes to 2
            // Only right HA turned on:
            //    ACP start goes to 3 at the same time as asha status ok, then *no audio*.
            //    no audio for several seconds
            //    turn on left HA *but don't connect left to bluetooth*, goes to 1, audio starts
            //
            switch(data[2])
            {
            case 1:
               g_info("%s OnHAStatus(Stream status 0034: (1) streaming)", side);
               break;
            case 2:
               g_info("%s OnHAStatus(Stream status 0034: (2) stopped)", side);
               break;
            case 3:
               g_info("%s OnHAStatus(Stream status 0034: (3) syncing)", side);
               break;
            default:
               g_info("%s OnHAStatus(Stream status 0034: %02hhx)", side, data[2]);
               break;
            }
            return;
         }
         break;
      case 0x0024:
         if (data.size() == 3)
         {
            g_info("%s OnHAStatus(Profile Index: %hhu)", side, data[2]);
            return;
         }
         break;
      case 0x0054:
         // I have never seen this value change. I don't know what it is.
         // always 00 54 69 ff 00 80 00
         break;
      }
   }

   std::stringstream ss;
   HexDump(ss, data.data(), data.size());
   g_info("%s OnHaStatus(%s)", side, ss.str().c_str());
}


void Side::OnBattery(uint8_t percent)
{
   g_info("%s Battery %hhu%%", Left() ? "Left" : "Right", percent);
}


void Side::OnExternalVolume(uint8_t value)
{
   g_info("%s External Volume %hhu", Left() ? "Left" : "Right", value);
}