#pragma once

#include "Bluetooth.hh"
#include "Characteristic.hh"
#include <string>
#include <vector>

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

   static std::shared_ptr<Side> CreateIfValid(const Bluetooth::Device& device);

   ~Side();

   std::string Description() const;
   Status ReadStatus();
   bool ReadProperties();
   const Properties& GetProperties() const { return m_properties; }
   bool Right() const { return m_properties.capabilities & 0x01; }
   bool Left() const { return !Right(); }

   bool Disconnect();
   bool Connect();
   void SetVolume(int volume);
   bool EnableStatusNotifications();
   bool DisableStatusNotifications();
   bool Start(bool otherstate);
   bool Stop();
   bool WriteAudioFrame(uint8_t* data, size_t size, uint8_t seq);
   bool UpdateOtherConnected(bool connected);
   bool UpdateConnectionParameters(uint8_t interval);

   const std::string& Name() const { return m_name; }
   const std::string& Alias() const { return m_alias; }

private:
   Side() {}

   struct
   {
      Characteristic properties;
      Characteristic audio_control;
      Characteristic status;
      Characteristic volume;
      Characteristic le_psm_out;
   } m_char;

   std::string m_name;
   std::string m_alias;
   std::string m_mac;
   Properties m_properties{};

   uint16_t m_psm_id = 0;
   int8_t m_volume = -20;
   int m_sock = -1;

   bool m_status_notify_enabled = false;
};

}