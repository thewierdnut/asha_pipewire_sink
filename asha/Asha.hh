#pragma once

#include "Side.hh"
#include <cstdint>
#include <map>
#include <string>
#include <vector>


namespace asha
{

// proprietary volume control?
//    service 7d74f4bd-c74a-4431-862c-cce884371592
//    characteristic 4603580D-3C15-4FEC-93BE-B86B243ADA64
//       Write value from 0x00 to 0xff

// "0000180d-0000-1000-8000-00805f9b34fb" heart rate?



class Asha
{
public:
   struct Device
   {
      uint64_t id;
      std::string name;
   };
   enum PlaybackType { UNKNOWN = 0, RINGTONE = 1, PHONECALL = 2, MEDIA = 3 };

   Asha() {}

   std::vector<Device> Devices() const;
   bool Ready() const;
   void SelectDevice(uint64_t id);
   const Device& SelectedDevice() const;
   void Start();
   void Stop();
   bool SendAudio(uint8_t* left, uint8_t* right, size_t size);
   void SetVolume(int8_t v);

   void EnumerateDevices();

   // void Process(int timeout_ms);

private:
   struct DeviceInfo
   {
      std::string name;
      std::string alias;

      // These devices must all have the same properties.hi_sync_id
      std::vector<std::shared_ptr<Side>> devices;
   };
   std::map<uint64_t, DeviceInfo> m_supported_devices;
   DeviceInfo* m_current_device = nullptr;
   uint64_t m_selected_id = 0;

   uint8_t m_audio_seq = 0;
};

}