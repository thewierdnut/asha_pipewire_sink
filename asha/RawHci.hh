#pragma once

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// We need access to some hci commands that the kernel doesn't provide through
// the normal socket interface. This uses a raw socket to send those commands.
class RawHci final
{
public:
   RawHci() noexcept {}
   RawHci(const std::string& mac, int sock = -1) noexcept;
   ~RawHci() noexcept;

   const struct hci_conn_info& ConnectionInfo() const { return m_connection_info; }
   uint16_t ConnectionHandle() const { return m_connection_id; }
   uint16_t DeviceId() const { return m_device_id; }

   bool ReadLinkQuality(uint8_t* quality);
   bool ReadRssi(int8_t* rssi);
   struct SystemConfig {
      // refer to src/adapter.c in bluez, the load_*_defaults functions
      uint16_t min_conn_interval;      // value 17
      uint16_t max_conn_interval;      // value 18

      std::map<uint16_t, std::vector<uint8_t>> raw;
   };
   bool ReadSysConfig(SystemConfig& config);
   bool ReadLEFeatures(std::vector<uint8_t>& features);

   // Set Phy2M. Requires CAP_NET_RAW access.
   bool SendPhy(bool phy1m, bool phy2m) noexcept;
   // Set DataLen. Requires CAP_NET_RAW access.
   bool SendDataLen(uint16_t size, uint16_t txtime);
   // Set Connection interval. Requires CAP_NET_RAW access.
   bool SendConnectionUpdate(uint16_t min_interval, uint16_t max_interval, uint16_t latency, uint16_t timeout, uint16_t min_ce=0, uint16_t max_ce=0);

   static bool HandleFromSocket(int sock, uint16_t* handle);
   

protected:
   template<typename T, typename ResponseT>
   bool SendCommand(uint8_t ogf, uint16_t ocf, const T& data, ResponseT* response, uint8_t meta_sub_event = 0) noexcept;

   template<typename RequestT, typename ResponseT>
   bool SendAndWaitForResponse(const RequestT& request, ResponseT* response, uint8_t meta_sub_event = 0) noexcept;

private:
   static constexpr uint16_t INVALID_ID = -1;

   uint16_t m_connection_id = INVALID_ID;
   uint16_t m_device_id = INVALID_ID;

   struct hci_conn_info m_connection_info{};



   int m_sock = -1;
};