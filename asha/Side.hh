#pragma once

#include "AudioPacket.hh"
#include "Bluetooth.hh"
#include "Characteristic.hh"
#include <string>
#include <vector>

struct _GTimer;
struct _GSocket;
struct _GSource;
struct _GCancellable;
struct _GError;

namespace asha
{
constexpr uint8_t CAPABILITY_RIGHT_SIDE = 0x01;
constexpr uint8_t CAPABILITY_BINAURAL = 0x02;
constexpr uint8_t CAPABILITY_CSIS = 0x04;
constexpr uint8_t FEATURE_STREAMING = 0x01;
constexpr uint16_t CODEC_G722_16KHZ = 0x02;
// Hidden/unsupported in android. Not supported here either
// constexpr uint16_t CODEC_G722_24KHZ = 0x04;

class Side: public std::enable_shared_from_this<Side>
{
public:
   enum Status {STATUS_OK = 0, UNKNOWN_COMMAND = -1, ILLEGAL_PARAMTER = -2, CALL_FAILED = -128};
   
   
   struct AshaProps
   {
      uint8_t version;        // must be 0x01
      uint8_t capabilities;   // Mask. 0x01 is right side, 0x02 is binaural, 0x04 is CSIS support
      uint64_t hi_sync_id;    // Same for paired devices
      uint8_t feature_map;    // 0x1 is audio streaming supported.
      uint16_t render_delay;  // ms delay before audio is rendered. For synchronization purposes.
      uint16_t reserved;
      uint16_t codecs;        // 0x2 is g.722. No others are defined.
   } __attribute__((packed));

   // Is this necessary? android always marks this as unknown.
   enum WriteStatus { WRITE_OK, DISCONNECTED, BUFFER_FULL, NOT_READY, TRUNCATED, OVERSIZED };

   static std::shared_ptr<Side> CreateIfValid(const Bluetooth::BluezDevice& device);

   virtual ~Side();

   std::string Description() const;
   std::string Mac() const { return m_mac; }

   const AshaProps& GetProperties() const { return m_asha_props; }
   bool Right() const { return m_asha_props.capabilities & 0x01; }
   bool Left() const { return !Right(); }
   void SubscribeExtra(/* callbacks? */);

   virtual void SetStreamVolume(int8_t volume);
   virtual void SetExternalVolume(uint8_t volume);
   // Start playback. The callback is called once the device is ready to receive.
   virtual bool Start(bool otherstate, std::function<void(bool)> OnDone);
   virtual bool Stop(std::function<void(bool)> OnDone);
   virtual WriteStatus WriteAudioFrame(const AudioPacket& packet);
   virtual bool UpdateOtherConnected(bool connected);
   virtual bool UpdateConnectionParameters(uint8_t interval);

   const std::string& Name() const { return m_name; }
   const std::string& Alias() const { return m_alias; }
   
   enum SideState {INIT, STOPPED, WAITING_FOR_READY, READY, WAITING_FOR_STOP};
   SideState State() const { return m_state; }

   int Sock() const;

   // Must be called before Connect()
   void SetConnectionParameters(uint16_t interval, uint16_t latency, uint16_t timeout, uint16_t celen)
   {
      m_interval = interval;
      m_latency = latency;
      m_timeout = timeout;
      m_celen = celen;
   }

   void SetOnConnectionReady(std::function<void()> ready);

protected:
   // Used by unit test mock.
   Side(const std::string& name): m_name(name), m_alias(name) {}
   void SetProps(bool left, uint64_t hisync)
   {
      m_asha_props = AshaProps{
         .version = 1,
         .capabilities = (uint8_t)(left ? 2 : 3),
         .hi_sync_id = hisync,
         .feature_map = 1,
         .render_delay = 160,
         .codecs = 2
      };
   }
   void SetState(SideState s) { m_state = s; }

   void OnStatusNotify(const std::vector<uint8_t>& data);
   void OnHAPropChanged(const std::vector<uint8_t>& data);
   void OnBattery(uint8_t percent);
   void OnExternalVolume(uint8_t value);

private:
   Side() {}
   bool EnableStatusNotifications();
   bool DisableStatusNotifications();

   void ReadPSM();
   void ReadProperties();
   bool Disconnect();
   bool Connect();
   bool Reconnect();

   void ConnectSucceeded();
   void ConnectFailed(const struct _GError* err);
   void ConnectionReady();

   struct
   {
      Characteristic properties;
      Characteristic audio_control;
      Characteristic status;
      Characteristic volume;
      Characteristic le_psm_out;

      // Not part of asha, but useful.
      Characteristic ha_status;
      Characteristic external_volume;
      Characteristic battery_10;
      Characteristic battery_100;
   } m_char;
   Properties m_properties;

   std::string m_name;
   std::string m_alias;
   std::string m_mac;
   AshaProps m_asha_props{};
   bool m_asha_props_valid = false;

   uint16_t m_psm_id = 0;
   int8_t m_volume = 0;
   // int m_sock = -1;
   std::shared_ptr<_GSocket> m_sock;
   std::shared_ptr<_GSource> m_sock_source;
   std::shared_ptr<_GCancellable> m_sock_cancellable;
   std::function<void()> m_OnConnectionReady;
   bool m_connection_ready = false;
   bool m_ready_to_receive_audio = false;
   unsigned int m_connect_failed_timeout = -1;
   SideState m_state = INIT;

   bool m_status_notify_enabled = false;
   std::function<void(Status)> m_next_status_fn;

   // Need CAP_NET_RAW to set these
   uint16_t m_interval = 16;
   uint16_t m_latency = 10;
   uint16_t m_timeout = 100;
   uint16_t m_celen = 12;
};

}