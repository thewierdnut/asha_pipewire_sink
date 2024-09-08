#include "Side.hh"

#include "Config.hh"
#include "GVariantDump.hh"
#include "HexDump.hh"
#include "RawHci.hh"

#include <cassert>
#include <cstring>
#include <sstream>

#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>
#include <glib-2.0/glib.h>
#include <gio/gio.h>
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
   // Don't return the side unless the correct asha characteristics exist.
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
      side->m_mac = device.mac;
      side->m_name = device.name;
      side->m_alias = device.alias;
      side->m_properties = device.properties;
      std::weak_ptr<Side> ws(side);
      side->m_properties.Subscribe([ws](const std::string& key, const std::shared_ptr<GVariant>& value) {
         auto side = ws.lock();
         if (side)
         {
            if (value)
               g_info("%s %s: %s", side->Description().c_str(), key.c_str(), GVariantDump(value.get()).c_str());
            else
               g_info("%s invalidated %s", side->Description().c_str(), key.c_str());
         }
      });

      side->m_interval = Config::Interval();
      side->m_timeout = Config::Timeout();
      side->m_celen = Config::Celength();
      side->m_volume = Config::Volume();

      side->ReadProperties();

      return side;
   }
   else
   {
      return nullptr;
   }
}


Side::~Side()
{
   if (m_sock_cancellable)
      g_cancellable_cancel(m_sock_cancellable.get());
   if (m_connect_failed_timeout != -1)
      g_source_remove(m_connect_failed_timeout);
}


std::string Side::Description() const
{
   if (m_asha_props.capabilities & CAPABILITY_BINAURAL)
   {
      if (m_asha_props.capabilities & CAPABILITY_RIGHT_SIDE)
         return m_name + " (Right)";
      else
         return m_name + " (Left)";
   }
   else
      return m_name;
}

void Side::ReadPSM()
{
   auto wp = weak_from_this();
   m_char.le_psm_out.Read([wp](const std::vector<uint8_t>& data) {
      g_debug("Read PSM Callback");

      if (data.size() == sizeof(uint16_t))
      {
         auto self = wp.lock();
         if (self)
         {
            self->m_psm_id = data[0] | (data[1] << 8);
            self->Connect();
         }
         else
            g_warning("Unable to lock side weak pointer in PSM callback");
      }
      else
      {
         g_warning("Unexpected psm data size: %zu", data.size());
      }
   });
}

void Side::ReadProperties()
{
   // Query the device properties.
   std::weak_ptr<Side> wp = shared_from_this();
   m_char.properties.Read([wp](const std::vector<uint8_t>& data) {
      if (data.size() == sizeof(m_asha_props))
      {
         g_debug("Properties read callback");
         auto self = wp.lock();
         if (self)
         {
            memcpy(&self->m_asha_props, data.data(), sizeof(m_asha_props));
            self->m_asha_props_valid = true;
            self->ReadPSM();
            // Since we now call ReadPSM in serial rather than in parallel,
            // this if statement will never be true. I'm not sure I want to do
            // it this way though.
            // if (self->m_connection_ready && self->m_OnConnectionReady)
            // {
            //    self->SetState(STOPPED);
            //    self->m_OnConnectionReady();
            // }
         }
         // TODO: once we have the properties, we can theoretically set
         //       a spa_latency_build() POD to indicate what latency the
         //       hearing devices expect.
      }
   });
}


void Side::SubscribeExtra(/* callbacks? */)
{
   if (m_char.ha_status)
      m_char.ha_status.Notify([this](const std::vector<uint8_t> &bytes) { OnHAPropChanged(bytes); });

   if (m_char.battery_100)
      m_char.battery_100.Notify([this](const std::vector<uint8_t> &bytes) { if(!bytes.empty()) OnBattery(bytes[0]); });
   else if (m_char.battery_10)
      m_char.battery_100.Notify([this](const std::vector<uint8_t> &bytes) { if(!bytes.empty()) OnBattery(bytes[0] * 10); });

   if (m_char.external_volume)
      m_char.external_volume.Notify([this](const std::vector<uint8_t> &bytes) { if (!bytes.empty()) OnExternalVolume(bytes[0]); } );
}



bool Side::Disconnect()
{
   if (m_sock)
   {
      g_socket_close(m_sock.get(), nullptr);
      m_sock.reset();
      return true;
   }
   return false;
}


bool Side::Connect()
{
   assert(m_psm_id != 0);
   
   bool ret = Reconnect();
   EnableStatusNotifications();
   return ret;
}

bool Side::Reconnect()
{
   g_debug("Creating Connection");

   assert(!m_sock);
   int sock = socket(AF_BLUETOOTH, SOCK_SEQPACKET|SOCK_NONBLOCK, BTPROTO_L2CAP);
   struct sockaddr_l2 addr{};
   addr.l2_family = AF_BLUETOOTH;
   addr.l2_bdaddr_type = BDADDR_LE_PUBLIC;
   if (0 != bind(sock, (struct sockaddr*)&addr, sizeof(addr)))
   {
      int e = errno;
      g_error("Failed to bind l2cap socket: %s", strerror(e));
      close(sock);
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
   if (0 != setsockopt(sock, SOL_BLUETOOTH, BT_MODE, &mode, sizeof(mode)))
   {
      int e = errno;
      g_error("Unable to set CoC flow control mode: %s", strerror(e));

      if (e == ENOPROTOOPT)
      {
         g_error("Please make sure that the bluetooth kernel module is being loaded with enable_ecred=1.");
      }
      close(sock);
      return false;
   }

   // TODO: Use setsockopt(sock, SOL_L2CAP, L2CAP_OPTIONS, ...) to set imtu/omtu
   //       Or use setsockopt(sock, SOL_BLUETOOTH, BT_SNDMTU/BT_RCVMTU) ? BT_SNDMTU always returns EPERM?
   //       and see if it overrides the crap 23 mtu that some devices use by
   //       default.

   // Switching from a native socket to a GSocket here, so that we can handle
   // async operations from the main loop.
   m_sock.reset(g_socket_new_from_fd(sock, nullptr), g_object_unref);
   g_socket_set_blocking(m_sock.get(), false);
   std::shared_ptr<GSocketAddress> gaddr(g_socket_address_new_from_native(&addr, sizeof(addr)), g_object_unref);

   // If we get deleted while this operation is pending, we will want to cancel
   // it.
   m_sock_cancellable.reset(g_cancellable_new(), g_object_unref);
   GError* err = nullptr;
   if (g_socket_connect(m_sock.get(), gaddr.get(), m_sock_cancellable.get(), &err))
   {
      // We shouldn't get here? I expect err to be set to G_IO_ERROR_PENDING.
      // If we do get here, it means we performed a synchronous connect.
      assert(!err);
      if (err)
      {
         g_warning("Got an error from g_socket_connect: %s", err->message);
         g_object_unref(err);
      }
      ConnectSucceeded();
      return true;
   }
   else if (!err)
   {
      // Not sure this is valid
      g_error("No error, and no socket?");
   }

   if (err->code != G_IO_ERROR_PENDING)
   {
      // Something bad happened.
      g_warning("Unable to to connect to %s: %s", Description().c_str(), err->message);
      g_object_unref(err);
      return false;
   }
   g_error_free(err);

   // Since this is a non-blocking socket, we don't know yet whether the
   // connection will succeed or not. Instead, we need to place an event that
   // gets called when the socket becomes writable, or we need to time out.
   m_sock_source.reset(g_socket_create_source(m_sock.get(), G_IO_OUT, m_sock_cancellable.get()), g_source_unref);
   struct CallbackContext
   {
      std::weak_ptr<Side> side;
   };
   auto* context = new CallbackContext{weak_from_this()};
   GSocketSourceFunc src_callback = [](GSocket*, GIOCondition condition, gpointer data) {
      g_debug("Connection Callback");
      // Asynchronously called when the socket is connected, or gets an error.
      auto* cc = (CallbackContext*)data;

      auto self = cc->side.lock();
      if (self && !g_cancellable_is_cancelled(self->m_sock_cancellable.get()))
      {
         GError* err = nullptr;
         if (g_socket_check_connect_result(self->m_sock.get(), &err))
            self->ConnectSucceeded();
         else
         {
            if (err->code != G_IO_ERROR_PENDING)
               self->ConnectFailed(err);
            g_error_free(err);
         }
      }
      return (gboolean)G_SOURCE_REMOVE;
   };

   g_source_set_callback(m_sock_source.get(),
      G_SOURCE_FUNC(src_callback),
      context,
      [](gpointer data) { // Destructor for userdata.
         delete (CallbackContext*)data;
      }
   );

   g_source_attach(m_sock_source.get(), nullptr);

   return true;
}

void Side::ConnectSucceeded()
{
   g_debug("Connection Succeeded");
   // TODO: make these async
   RawHci hci(m_mac, g_socket_get_fd(m_sock.get()));
   if (Config::Phy1m() || Config::Phy2m())
   {
      // This requires CAP_NET_RAW
      if (!hci.SendPhy(Config::Phy1m(), Config::Phy2m()))
         g_warning("Unable to negotiate the requested PHY without CAP_NET_RAW");
   }

   // This requires CAP_NET_RAW
   if (!hci.SendConnectionUpdate(m_interval, m_interval, m_latency, m_timeout, m_celen, m_celen))
   {
      // This failed (probably don't have requisite permissions). Extract the
      // configured value using an unpriviledge request, and notify of that instead.
      RawHci::SystemConfig config;
      hci.ReadSysConfig(config);
      if (config.max_conn_interval == config.min_conn_interval && config.max_conn_interval <= 16)
         m_interval = config.min_conn_interval;
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
   UpdateConnectionParameters(m_interval);
   ConnectionReady();
}


void Side::ConnectionReady()
{
   g_debug("Connection Ready");

   // Call the user callback if we have one.
   m_connection_ready = true;
   if (m_asha_props_valid && m_OnConnectionReady)
   {
      SetState(STOPPED);
      m_OnConnectionReady();
   }
}


void Side::SetOnConnectionReady(std::function<void()> ready)
{
   g_debug("Connection Ready Callback Set");
   // If the connection is already ready, then call the callback immediately.
   m_OnConnectionReady = ready;
   if (m_connection_ready && m_asha_props_valid)
   {
      SetState(STOPPED);
      m_OnConnectionReady();
   }
}


void Side::ConnectFailed(const GError* err)
{
   g_warning("Connection to %s failed. Retrying in 1 second: %s", Description().c_str(), err->message);

   m_connect_failed_timeout = g_timeout_add(1000, [](gpointer data) -> gboolean {
      auto* self = (Side*)data;
      self->Connect();
      self->m_connect_failed_timeout = -1;
      return G_SOURCE_REMOVE;
   }, this);
}


void Side::SetStreamVolume(int8_t volume)
{
   m_volume = volume;

   m_char.volume.Command({(uint8_t)m_volume});
}


void Side::SetExternalVolume(uint8_t volume)
{
   if (m_char.external_volume)
      m_char.external_volume.Command({volume});
}


bool Side::EnableStatusNotifications()
{
   // Turn on status notifications.
   m_char.status.Notify([this](const std::vector<uint8_t>& data) { OnStatusNotify(data); });
   return true;
}

bool Side::DisableStatusNotifications()
{
   m_char.status.StopNotify();

   return true;
}

bool Side::Start(bool otherstate, std::function<void(bool)> OnDone)
{
   assert(m_state == STOPPED);
   const char* side = Left() ? "left " : "right";
   g_info("%s Sending ACP start other %s", side, otherstate ? "connected" : "not connected");

   static constexpr uint8_t G722_16KHZ = 1;
   m_ready_to_receive_audio = false;
   auto wt = weak_from_this();
   m_next_status_fn = [wt, OnDone](Status s) {
      auto t = wt.lock();
      if (t)
      {
         t->m_ready_to_receive_audio = s == STATUS_OK;
         if (s == STATUS_OK)
            t->SetState(READY);
         OnDone(t->m_ready_to_receive_audio);
      }
   };
   m_char.audio_control.Write({Control::START, G722_16KHZ, 0, (uint8_t)m_volume, (uint8_t)otherstate}, [](bool){});
   return true;
}

bool Side::Stop(std::function<void(bool)> OnDone)
{
   assert(m_state == READY);
   const char* side = Left() ? "left " : "right";
   g_info("%s Sending ACP stop", side);

   m_ready_to_receive_audio = false;
   auto wt = weak_from_this();
   SetState(WAITING_FOR_STOP);
   m_char.audio_control.Write({Control::STOP}, [OnDone, wt](bool status) {
      auto t = wt.lock();
      if (t)
         t->SetState(STOPPED);
      OnDone(status);
   });
   return true;
}

Side::WriteStatus Side::WriteAudioFrame(const AudioPacket& packet)
{
   // Write 20ms of data. Should be exactly 160 bytes in size.
   static_assert(sizeof(packet) == 161, "We can only send 161 byte audio packets");
   WriteStatus ret = NOT_READY;
   if (m_sock && m_ready_to_receive_audio)
   {
      int bytes_sent = g_socket_send(m_sock.get(), (const char*)&packet, sizeof(packet), m_sock_cancellable.get(), nullptr);
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
         g_socket_close(m_sock.get(), nullptr);
         m_sock.reset();
         m_ready_to_receive_audio = false;
         ret = DISCONNECTED;
      }
   }
   return ret;
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


int Side::Sock() const
{
   return m_sock ? g_socket_get_fd(m_sock.get()) : -1;
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

void Side::OnHAPropChanged(const std::vector<uint8_t>& data)
{
   // 38278651-76d7-4dee-83d8-894f3fa6bb99 notifications
   // I've partially figured out what some of this stuff means.
   const char* side = Left() ? "left " : "right";
   if (data.size() > 2)
   {
      // Property is two bytes, but I'm not sure how its broken up.
      // Reading this as big-endian for convenience.
      uint16_t property = data[0] << 8 | data[1];
      switch(property)
      {
      case 0x0014: // Volume changed
         if (data.size() == 5)
         {
            g_info("%s OnHAPropChanged(Muted: %hhu, Volume: %hhu, ??: %02hhx)", side, data[2], data[3], data[4]);
            return;
         }
         break;
      case 0x0194: // Not sure... it turns on and off with muting and streaming
         if (data.size() == 3)
         {
            g_info("%s OnHAPropChanged(0194: %02hhx)", side, data[2]);
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
               g_info("%s OnHAPropChanged(Stream status 0034: (1) streaming)", side);
               break;
            case 2:
               g_info("%s OnHAPropChanged(Stream status 0034: (2) stopped)", side);
               break;
            case 3:
               g_info("%s OnHAPropChanged(Stream status 0034: (3) syncing)", side);
               break;
            default:
               g_info("%s OnHAPropChanged(Stream status 0034: %02hhx)", side, data[2]);
               break;
            }
            return;
         }
         break;
      case 0x0024:
         if (data.size() == 3)
         {
            g_info("%s OnHAPropChanged(Profile Index: %hhu)", side, data[2]);
            return;
         }
         break;
      case 0x0054:
         // I have never seen this value change. I don't know what it is.
         // always 00 54 69 ff 00 80 00
         break;
      }
      std::stringstream ss;
      HexDump(ss, data.data() + 2, data.size() - 2);
      g_info("%s OnHAPropChanged(%04x, %s)", side, property, ss.str().c_str());
   }
   else
   {
      std::stringstream ss;
      HexDump(ss, data.data(), data.size());
      g_info("%s OnHAPropChanged(%s)", side, ss.str().c_str());
   }

   
}


void Side::OnBattery(uint8_t percent)
{
   g_info("%s Battery %hhu%%", Left() ? "Left" : "Right", percent);
}


void Side::OnExternalVolume(uint8_t value)
{
   g_info("%s External Volume %hhu", Left() ? "Left" : "Right", value);
}