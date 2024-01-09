#pragma once

#include <memory>
#include <string>
#include <vector>

// Manages and streams data to asha devices
class Asha
{
public:
   struct AshaDevice
   {
      uint64_t id;
      std::string name;
   };
   enum PlaybackType { UNKNOWN = 0, RINGTONE = 1, PHONECALL = 2, MEDIA = 3 };

   Asha();

   std::vector<AshaDevice> Devices() const;
   bool Ready() const;
   void SelectDevice(uint64_t id);
   const AshaDevice& SelectedDevice() const;
   void Start();
   void Stop();
   bool SendAudio(uint8_t* left, uint8_t* right, size_t size);
   void SetVolume(int8_t v);

   void Process(int timeout_ms);

private:
   struct AshaImpl;
   std::shared_ptr<AshaImpl> m;
};