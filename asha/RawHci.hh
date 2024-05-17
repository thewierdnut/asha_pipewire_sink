#pragma once

#include <string>
#include <vector>

// We need access to some hci commands that the kernel doesn't provide through
// the normal socket interface. This uses a raw socket to send those commands.
class RawHci final
{
public:
   RawHci(const std::string& mac) noexcept;
   ~RawHci() noexcept;

   bool SetPhy2M() noexcept;
   bool SetDataLen(uint16_t size, uint16_t txtime);
   bool SetConnectionParamters(uint16_t min_interval, uint16_t max_interval, uint16_t latency, uint16_t timeout, uint16_t min_ce=0, uint16_t max_ce=0);

protected:
   template<typename T, typename ResponseT>
   bool SendCommand(uint8_t ogf, uint16_t ocf, const T& data, ResponseT* response, uint8_t meta_sub_event = 0) noexcept;

private:
   static constexpr uint16_t INVALID_ID = -1;

   uint16_t m_connection_id = INVALID_ID;
   uint16_t m_device_id = INVALID_ID;

   int m_sock = -1;
};