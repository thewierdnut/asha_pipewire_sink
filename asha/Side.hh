#pragma once

#include "AudioPacket.hh"
#include "Bluetooth.hh"
#include "Characteristic.hh"
#include <string>
#include <vector>

struct _GTimer;

namespace asha
{
constexpr uint8_t CAPABILITY_RIGHT_SIDE = 0x01;
constexpr uint8_t CAPABILITY_BINAURAL = 0x02;
constexpr uint8_t CAPABILITY_CSIS = 0x04;
constexpr uint8_t FEATURE_STREAMING = 0x01;
constexpr uint16_t CODEC_G722_16KHZ = 0x02;
// Hidden/unsupported in android. Not supported here either
// constexpr uint16_t CODEC_G722_24KHZ = 0x04;

class Side final
{
public:
   enum Status {STATUS_OK = 0, UNKNOWN_COMMAND = -1, ILLEGAL_PARAMTER = -2, CALL_FAILED = -128};
   
   
   
   struct Properties
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
   enum PlaybackType { UNKNOWN = 0, RINGTONE = 1, PHONECALL = 2, MEDIA = 3 };
   enum WriteStatus { WRITE_OK, DISCONNECTED, BUFFER_FULL, NOT_READY, TRUNCATED, OVERSIZED };

   static std::shared_ptr<Side> CreateIfValid(const Bluetooth::BluezDevice& device);

   ~Side();

   std::string Description() const;
   std::string Mac() const { return m_mac; }
   Status ReadStatus();
   bool ReadProperties();
   const Properties& GetProperties() const { return m_properties; }
   bool Right() const { return m_properties.capabilities & 0x01; }
   bool Left() const { return !Right(); }
   void SubscribeExtra(/* callbacks? */);

   bool Disconnect();
   bool Connect();
   bool Reconnect();
   void SetStreamVolume(int8_t volume);
   void SetDeviceVolume(int8_t volume);
   // Start playback. The callback is called once the device is ready to receive.
   bool Start(bool otherstate);
   bool Stop();
   WriteStatus WriteAudioFrame(const AudioPacket& packet);
   void ReadFromAudioSocket();
   bool UpdateOtherConnected(bool connected);
   bool UpdateConnectionParameters(uint8_t interval);

   const std::string& Name() const { return m_name; }
   const std::string& Alias() const { return m_alias; }
   bool Ready() const { return m_ready_to_receive_audio; }

   int Sock() const { return m_sock; }

private:
   Side() {}
   bool EnableStatusNotifications();
   bool DisableStatusNotifications();

   void OnStatusNotify(const std::vector<uint8_t>& data);
   void OnHAStatus(const std::vector<uint8_t>& data);
   void OnBattery(uint8_t percent);
   void OnExternalVolume(uint8_t value);

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

   std::string m_name;
   std::string m_alias;
   std::string m_mac;
   Properties m_properties{};

   uint16_t m_psm_id = 0;
   int8_t m_volume = 0;
   int m_sock = -1;

   bool m_status_notify_enabled = false;
   bool m_ready_to_receive_audio = false;

   std::function<void(Status)> m_next_status_fn;

   // TODO: remove this debugging code
   // std::shared_ptr<struct _GTimer> m_timer;
   // size_t m_packet_count = 0;
};

}