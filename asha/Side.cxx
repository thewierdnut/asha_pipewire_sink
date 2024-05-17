#include "Side.hh"
#include "RawHci.hh"

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


std::shared_ptr<Side> Side::CreateIfValid(const Bluetooth::Device& device)
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
   // Volume is optional, but the others are required.
   if (side->m_char.properties && side->m_char.audio_control &&
       side->m_char.status && side->m_char.le_psm_out)
   {
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

   if (connect(m_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0)
   {
      int err = errno;
      g_error("Failed to connect l2cap channel: %s", strerror(err));
      close(m_sock);
      m_sock = -1;
      return false;
   }

   EnableStatusNotifications();

   // Issue a connection update?
   //       This is supposed to be an optional step in the bluetooth
   //       spec, and the linux kernel will only respond to connection
   //       update request issued by the peripheral, but won't
   //       generate its own updates. Unfortunately, Step 4 in the
   //       audio startup sequence says "Both the central and the
   //       peripheral host wait for the update complete event."
   //       I think that the hearing aids are waiting on the
   //       connection parameters before accepting audio.
   // Sets the data length here?
   //          Set data length via hcitool?
   //          hcitool cmd 0x08 0x0022 0x10 0x00 0xa7 0x00 0x90 0x42
   //                      LE  SETDLEN CONNHANDL TXOCTETS  TXTIME
   // Sets the the PHY to LE 2M here? This is recommended, but not required.
   // Android won't attempt to set 24Khz G.722 without it.
   //          $ hcitool con
   //          Connections:
   //              < LE 9C:9C:1D:98:BE:82 handle 16 state 1 lm CENTRAL AUTH ENCRYPT
   //                                            |
   //                                           \|/
   //          $ sudo hcitool cmd 0x08 0x0032 0x10 0x00 0x00 0x02 0x02 0x00 0x00
   //                             LE   SETPHY CONNHANDL ALL    TX   RX CODEDOPTS
   //        If hcitool can do this even though the linux kernel can't, is
   //        there a way to do this with raw stanzas somehow? Probably need
   //        root access.

   // Issue a connection update. The existing kernel code expects the
   // peripheral to issue this, but the spec allows either side to set it, and
   // the asha standard requires the central to set it. This code here will
   // only work if you have the requisite bluetooth patches merged into your
   // kernel. THIS WILL NOT WORK WITHOUT THESE PATCHES.
   //  https://lore.kernel.org/linux-bluetooth/20170413122203.4247-1-eu@felipetonello.com/
   /*
   static constexpr int BT_LE_CONN_PARAM = 30;
   struct bt_le_conn_param {
      uint16_t min_interval;
      uint16_t max_interval;
      uint16_t latency;
      uint16_t supervision_timeout;
   } params;
   socklen_t param_len = sizeof(params);
   if (0 == getsockopt(m_sock, SOL_BLUETOOTH, BT_LE_CONN_PARAM, &params, &param_len))
   {
      g_info("Retrieved connection parameters: \n"
             "  min:     %hu\n"
             "  max:     %hu\n"
             "  latency: %hu\n"
             "  timeout: %hu",
         params.min_interval,
         params.max_interval,
         params.latency,
         params.supervision_timeout
      );
      params.min_interval = 16;  // Set the interval to 16 * 1.25 = 20ms
      params.max_interval = 16;
      params.latency = 10; // event count
      params.supervision_timeout = 100; // 1 second
      if (0 == setsockopt(m_sock, SOL_BLUETOOTH, BT_LE_CONN_PARAM, &params, param_len))
         g_info("Sent connection paramter update");
      else
         g_info("Failed to send connection parameter update");
   }
   else
   {
      int e = errno;
      g_warning("Failed to retrieve connection parameters: %s", strerror(e));
   }
   // */

   // This code requires CAP_NET_RAW, but it allows us to set the necessary
   // connection parameters with an unpatched kernel. 
   RawHci raw(m_mac);
   
   // I've never gotten it to work without sending this
   if (raw.SendConnectionUpdate(16, 16, 10, 100)) // 20ms interval, 10 event buffer
      g_info("Set connection parameters to 20ms");
   else
      g_warning("Failed to set the connection parameters");
   
   // These are nice to have, but it can work without them.
   // 161 data bytes, plus 6 byte header. Android sends 17040 microseconds as a
   // default, but at 16000 hz and two sides, we shouldn't spend more than 10ms
   // per pdu (each pdu is 320 mono samples)
   if (raw.SendDataLen(167, 9000))
      g_info("Set data length to 167 bytes");
   else
      g_info("Failed to set data length to 167 bytes");
   if (raw.SendPhy2M())
      g_info("Set 2M PHY mode");
   else
      g_info("Unable to set 2M PHY mode");

   return true;
}

void Side::SetVolume(int volume)
{
   m_volume = volume;

   // This characteristic is optional
   if (m_char.volume)
   {
      m_char.volume.Command({(uint8_t)m_volume});
   }
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
   static constexpr uint8_t G722_16KHZ = 1;
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

bool Side::WriteAudioFrame(uint8_t* data, size_t size, uint8_t seq)
{
   // Write 20ms of data. Should be exactly 160 bytes in size.

   assert(size == 160);
   // assert(m_sock != -1);
   if (m_sock != -1 && m_ready_to_receive_audio)
   {
      if (size > 160)
         size = 160;
      struct {
         //uint16_t sdu_length;
         uint8_t seq;
         uint8_t data[160];
      } packet;
      // packet.sdu_length = htobs(size + 1);
      packet.seq = seq;
      memcpy(packet.data, data, size);
      if (send(m_sock, &packet, size + 1, MSG_DONTWAIT) == size + 1)
         return true;
      else if (errno == EAGAIN)
      {
         g_warning("Dropping frame for %s", Description().c_str());
      }
      else
      {
         g_warning("Disconnected by %s", Description().c_str());
         close(m_sock);
         m_sock = -1;
      }
   }
   return false;
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