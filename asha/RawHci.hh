#pragma once

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>

#include <cstdint>
#include <string>
#include <vector>

// We need access to some hci commands that the kernel doesn't provide through
// the normal socket interface. This uses a raw socket to send those commands.
class RawHci final
{
public:
   RawHci(const std::string& mac, int sock = -1) noexcept;
   ~RawHci() noexcept;

   const struct hci_conn_info& ConnectionInfo() const { return m_connection_info; }
   uint16_t ConnectionHandle() const { return m_connection_id; }
   uint16_t DeviceId() const { return m_device_id; }

   //uint64_t ReadSupportedFeatures();
   bool ReadExtendedFeatures(uint8_t page, uint64_t* features, bool* more);
   bool ReadLinkQuality(uint8_t* quality);
   bool ReadRssi(int8_t* rssi);

   // Set Phy2M. Requires CAP_NET_RAW access.
   bool SendPhy2M() noexcept;
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