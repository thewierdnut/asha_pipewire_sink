#include "RawHci.hh"

#include <iomanip>
#include <iostream>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/l2cap.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <poll.h>

// What other sources of information can we access?
// Non-root access:
//    management commands from https://elixir.bootlin.com/linux/v6.1/source/net/bluetooth/mgmt.c#L185
//       These might be bad to use, as they are not exposed via constants and
//       may change in the future. btmgmr uses magic values to access them.
//       MGMT_OP_READ_INDEX_LIST,            // Read a list of devices
//       MGMT_OP_READ_INFO,                  // Gets a mgmt_rp_read_info struct, which includes current and supported settings. This can tell us LE is supported.
//       MGMT_OP_READ_UNCONF_INDEX_LIST,     // Gets a list of controllers that are unconfigured.
//       MGMT_OP_READ_CONFIG_INFO,           // Returns mgmt_rp_read_config_info struct. Nothing useful for us.
//       MGMT_OP_READ_EXT_INDEX_LIST,        // Returns a slightly more detailed list of devices.
//       MGMT_OP_READ_EXT_INFO,              // Returns a mgmt_rp_read_ext_info struct
//       MGMT_OP_READ_CONTROLLER_CAP,        // Reads a set of controller capabilities
//       MGMT_OP_READ_EXP_FEATURES_INFO,     // Read a set of list of 16 bit uuid features.
//       MGMT_OP_READ_DEF_SYSTEM_CONFIG,     // Returns the system defaults (mostly the values in /etc/bluetooth/main.conf)
//       MGMT_OP_READ_DEF_RUNTIME_CONFIG,    // seems to just return an empty response.

namespace
{
   void DumpHex(uint8_t* p, size_t len)
   {
      auto oldflags = std::cout.flags();
      for (ssize_t i = 0; i < len; ++i)
      {
         if (i != 0) std::cout << ' ';
         std::cout << std::setfill('0') << std::setw(2) << std::hex << (uint32_t)p[i];
      }
      std::cout.flags(oldflags);
   }
}


RawHci::RawHci(const std::string& mac, int connection_sock) noexcept
{
   uint16_t handle;
   if (!HandleFromSocket(connection_sock, &handle))
      return;

   // MAC address of device we are searching for.
   bdaddr_t mac_addr{};
   if (6 != sscanf(mac.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                     &mac_addr.b[5],&mac_addr.b[4],&mac_addr.b[3],
                     &mac_addr.b[2],&mac_addr.b[1],&mac_addr.b[0]
                  ))
      return;
   
   // Use raw bluetooth socket ioctls to get the info needed.
   int sock = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
   if (sock < 0) return;

   // Get a list of devices.
   char buffer[sizeof(hci_dev_list_req) + HCI_MAX_DEV * sizeof(hci_dev_req)] = {};
   auto dev_list = (hci_dev_list_req*)buffer;
   dev_list->dev_num = HCI_MAX_DEV;

   if (ioctl(sock, HCIGETDEVLIST, buffer) < 0)
   {
      close(sock);
      return;
   }

   for (size_t i = 0; i < dev_list->dev_num && m_device_id == INVALID_ID; ++i)
   {
      if (hci_test_bit(HCI_UP, &(dev_list->dev_req[i].dev_opt)))
      {
         // Next, get a list of connections for each device.
         static constexpr size_t MAX_CONN = 10;
         char buffer2[sizeof(hci_conn_list_req) + MAX_CONN * sizeof(hci_conn_info)] = {};
         auto conn_list = (hci_conn_list_req*)buffer2;
         conn_list->dev_id = dev_list->dev_req[i].dev_id;
         conn_list->conn_num = MAX_CONN;

         if (ioctl(sock, HCIGETCONNLIST, buffer2) == 0)
         {
            for (size_t j = 0; j < conn_list->conn_num; ++j)
            {
               // Only check outgoing LE connections.
               if (!conn_list->conn_info[j].out)
                  continue;
               // Commenting this out. Source code says 0x03, but gdb says 0x80.
               // if (conn_list->conn_info[j].type != 0x03) // LE_LINK
               //    continue;
               if (conn_list->conn_info[j].handle == handle &&
                   memcmp(&conn_list->conn_info[j].bdaddr, &mac_addr, 6) == 0)
               {
                  // This is the device we are looking for
                  m_device_id = conn_list->dev_id;
                  m_connection_id = conn_list->conn_info[j].handle;
                  m_connection_info = conn_list->conn_info[j];

                  break;
               }
            }
         }
      }
   }

   struct sockaddr_hci addr{};
   addr.hci_family = AF_BLUETOOTH;
   addr.hci_dev = m_device_id;

   if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0)
   {
      close(sock);
      return;
   }

   struct hci_filter filter{};
   hci_filter_set_ptype(HCI_EVENT_PKT, &filter);
   hci_filter_set_event(EVT_CMD_STATUS, &filter);
   hci_filter_set_event(EVT_CMD_COMPLETE, &filter);
   hci_filter_set_event(EVT_LE_META_EVENT, &filter);
   if (setsockopt(sock, SOL_HCI, HCI_FILTER, &filter, sizeof(filter)) < 0)
   {
      // This probably won't break our query if the filter is broken.
   }
   
   m_sock = sock;

   // TODO: is there any useful information we can get out of HCIINQUIRY ioctl?
}

RawHci::~RawHci() noexcept
{
   if (m_sock >= 0)
      close(m_sock);
}


bool RawHci::ReadExtendedFeatures(uint8_t page, uint64_t* features, bool* more)
{
   if (!features) return false;
   struct {
      uint8_t status;
      uint8_t page;
      uint8_t max_page;
      uint32_t features;
   } __attribute__((packed)) response{};

   if (SendCommand(OGF_INFO_PARAM, OCF_READ_LOCAL_EXT_FEATURES, page, &response) && response.status == 0)
   {
      if (more)
         *more = page < response.max_page;
      *features = response.features;
      return true;
   }
   return false;
}


bool RawHci::ReadLinkQuality(uint8_t* quality)
{
   if (!quality) return false;
   struct {
      uint8_t  type;
      uint16_t opcode;
      uint8_t  len;
      uint16_t connection_id;
   } __attribute__((packed)) msg{
      HCI_COMMAND_PKT,
      cmd_opcode_pack(OGF_STATUS_PARAM, OCF_READ_LINK_QUALITY),
      sizeof(uint16_t),
      m_connection_id
   };
   read_link_quality_rp response{};
   if (SendAndWaitForResponse(msg, &response) && response.status == 0)
   {
      *quality = response.link_quality;
      return true;
   }
   return false;
}


bool RawHci::ReadRssi(int8_t* rssi)
{
   if (!rssi) return false;
   struct {
      uint8_t  type;
      uint16_t opcode;
      uint8_t  len;
      uint16_t connection_id;
   } __attribute__((packed)) msg{
      HCI_COMMAND_PKT,
      cmd_opcode_pack(OGF_STATUS_PARAM, OCF_READ_RSSI),
      sizeof(uint16_t),
      m_connection_id
   };
   read_rssi_rp response{};
   if (SendAndWaitForResponse(msg, &response) && response.status == 0)
   {
      *rssi = response.rssi;
      return true;
   }
   return false;
}


bool RawHci::ReadSysConfig(SystemConfig& config)
{
   // Root should not be needed for this one.
   config = SystemConfig{};

   // struct mgmt_hdr from mgmt.h
   struct mgmt_hdr {
      uint16_t opcode;
      uint16_t index;
      uint16_t len;
   } __attribute__((packed));
   struct mgmt_status
   {
      uint16_t opcode;
      uint8_t status;
   } __attribute__((packed));
   struct mgmt_hdr hdr{};
   hdr.opcode = htobs(0x004b); // MGMT_OP_READ_DEF_SYSTEM_CONFIG
   hdr.index = htobs(m_device_id == INVALID_ID ? 0 : m_device_id);
   hdr.len = htobs(0);

   // Can't use normal raw socket for this one.
   int sock = socket(PF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
   if (sock < 0)
   {
      std::cout << "Unable to create management socket\n";
      return false;
   }
   struct sockaddr_hci addr{};
   addr.hci_family = htobs(AF_BLUETOOTH);
   addr.hci_dev = htobs(HCI_DEV_NONE);
   addr.hci_channel = htobs(HCI_CHANNEL_CONTROL);
   if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0)
   {
      std::cout << "Unable to bind to control channel\n";
      close(sock);
      return false;
   }

   if (sizeof(hdr) != send(sock, &hdr, sizeof(hdr), 0))
   {
      std::cout << "Unable to write management request: " << strerror(errno) << '\n';
      close(sock);
      return false;
   }

   while(true)
   {
      uint8_t buffer[65536];
      auto bytes_read = read(sock, buffer, sizeof(buffer));
      
      if (bytes_read < 0)
      {
         std::cout << "Unable to read management response\n";
         break;
      }

      if (bytes_read < sizeof(mgmt_hdr))
      {
         std::cout << "Truncated management header in response\n";
         break;
      }
      mgmt_hdr* resp = (mgmt_hdr*)buffer;

      if (bytes_read < sizeof(mgmt_hdr) + btohs(resp->len))
      {
         std::cout << "Truncated management packet in response\n";
         break;
      }
      auto length = btohs(resp->len);
      auto event = btohs(resp->opcode);
      if (event == 0x0001) // MGMT_EV_CMD_COMPLETE
      {
         if (length < sizeof(mgmt_hdr) + sizeof(mgmt_status))
         {
            std::cout << "Truncated management complete\n";
            break;
         }
         auto status = (mgmt_status*)(resp + 1);
         if (btohs(status->opcode) == 0x004b)
         {
            length -= sizeof(mgmt_hdr) + sizeof(mgmt_status);
            uint8_t* p = buffer + sizeof(mgmt_hdr) + sizeof(mgmt_status);
            while (length > sizeof(uint16_t) + sizeof(uint8_t))
            {
               uint16_t type = btohs(*(uint16_t*)p);
               length -= sizeof(uint16_t); p += sizeof(uint16_t);
               uint8_t datalen = *p;
               length -= sizeof(uint8_t); p += sizeof(uint8_t);
               if (datalen > length)
                  break;
               config.raw[type].assign(p, p + datalen);
               switch(type)
               {
               case 0x0017: // min_conn_interval;
                  if (datalen == 2)
                     config.min_conn_interval = btohs(*(uint16_t*)p);
                  break;
               case 0x0018: // max_conn_interval;
                  if (datalen == 2)
                     config.max_conn_interval = btohs(*(uint16_t*)p);
                  break;
               }
               length -= datalen; p += datalen;
            }
            // DumpHex(buffer + sizeof(mgmt_hdr) + sizeof(mgmt_status), length - sizeof(mgmt_hdr) + sizeof(mgmt_status));
            break;
         }
      }
      else if (event == 0x0002) // MGMT_EV_CMD_STATUS
      {
         // if (length < sizeof(mgmt_hdr) + sizeof(mgmt_status))
         // {
         //    std::cout << "Truncated management status\n";
         //    break;
         // }
         // auto status = (mgmt_status*)(resp + 1);
         // // Not sure we actually care?
         // if (btohs(status->opcode) == 0x004b)
         //    std::cout << "Read status: " << (unsigned)status->status << '\n';
      }
      else
      {
         // ?
      }
   }

   close(sock);
   return true;
}



bool RawHci::SendPhy2M() noexcept
{
   struct {
      uint8_t  phy_flags = 0;
      uint8_t  phy_tx = 0x02; // Prefer to send LE 2M
      uint8_t  phy_rx = 0x02; // Prefer to receive LE 2M
      uint16_t coding = 0x0000;
   } __attribute__((packed)) msg{};
   struct {
      uint8_t status;
      uint16_t handle;
      uint8_t phy_tx;
      uint8_t phy_rx;
   } __attribute__((packed)) response{};
   bool success = SendCommand(0x08, 0x0032, msg, &response, 0x0C);
   if (success)
   {
      success = response.status == 0;
      // TODO: should we verify that the returned parameters match the ones we
      //       requested?
   }
   return success;
}


bool RawHci::SendDataLen(uint16_t size, uint16_t txtime)
{
   struct {
      uint16_t size;
      uint16_t time;
   } __attribute__((packed)) msg{
      size, txtime
   };
   struct {
      uint8_t status;
      uint16_t handle;
   } __attribute__((packed)) response{};
   bool success = SendCommand(0x08, 0x0022, msg, &response);
   if (success)
   {
      success = response.status == 0;
      // TODO: should we verify that the returned parameters match the ones we
      //       requested?
   }
   return success;
}

bool RawHci::SendConnectionUpdate(uint16_t min_interval, uint16_t max_interval, uint16_t latency, uint16_t timeout, uint16_t min_ce, uint16_t max_ce)
{
   struct {
      uint16_t min_interval;
      uint16_t max_interval;
      uint16_t latency;
      uint16_t timeout;
      uint16_t min_ce;
      uint16_t max_ce;
   } __attribute__((packed)) msg{
      min_interval, max_interval,
      latency, timeout,
      min_ce, max_ce
   };
   struct {
      uint8_t status;
      uint16_t handle;  // TODO: Validate that this matches m_connection_id?
      uint16_t interval;
      uint16_t latency;
      uint16_t timeout;
   } __attribute((packed)) response{};
   bool success = SendCommand(0x08, 0x0013, msg, &response, 0x03);
   if (success)
   {
      success = response.status == 0;
      // TODO: should we verify that the returned parameters match the ones we
      //       requested?
   }
   return success;
}


template<typename T, typename ResponseT>
bool RawHci::SendCommand(uint8_t ogf, uint16_t ocf, const T& data, ResponseT* response, uint8_t meta_sub_event) noexcept
{
   // Reading through the linux source code in source/net/bluetooth/hci_sock.c
   // it looks like it will let you create a raw socket without CAP_NET_RAW,
   // but it will return EPERM if you try to send an unapproved OGF
   if (m_connection_id == INVALID_ID || (m_sock < 0))
      return false;
   struct {
      uint8_t  type;
      uint16_t opcode;
      uint8_t  len;
      uint16_t connection_id;
      T data;
   } __attribute__((packed)) msg{
      HCI_COMMAND_PKT,
      cmd_opcode_pack(ogf, ocf),
      sizeof(uint16_t) + sizeof(T),
      m_connection_id,
      data
   };

   return SendAndWaitForResponse(msg, response, meta_sub_event);
}


template<typename RequestT, typename ResponseT>
bool RawHci::SendAndWaitForResponse(const RequestT& request, ResponseT* response, uint8_t meta_sub_event) noexcept
{
   if (m_connection_id == INVALID_ID || (m_sock < 0))
      return false;

   std::cout << "request: ";
   DumpHex((uint8_t*)&request, sizeof(request));
   std::cout << '\n';
   while (send(m_sock, &request, sizeof(request), 0) < 0)
   {
      if (errno != EAGAIN && errno != EINTR)
      {
         int e = errno;
         std::cout << "Unable to send command: " << strerror(e) << '\n';
         return false;
      }
   }
   for (int i = 0; i < 5; ++i)
   {
      struct pollfd events{};
      events.fd = m_sock;
      events.events = POLLIN;
      if (0 == poll(&events, 1, 2000))
      {
         // timeout
         std::cout << "Timed out waiting for response\n";
         return false;
      }
      uint8_t buffer[HCI_MAX_EVENT_SIZE] = {};
      ssize_t len = read(m_sock, buffer, sizeof(buffer));
      if (len < 0)
      {
         int e = errno;
         std::cout << "Failed to read command response: " << strerror(e) << '\n';
         return false;
      }
      else if (len > 1 + sizeof(hci_event_hdr))
      {
         std::cout << "response: ";
         DumpHex(buffer, len);
         std::cout << '\n';
         uint8_t* p = buffer;
         p += 1;
         len -= 1;
         
         auto* hdr = (hci_event_hdr*)p;
         p += sizeof(hci_event_hdr);
         len -= sizeof(hci_event_hdr);
         
         if (len >= hdr->plen)
         {
            // We are filtering the response types, so these are the only ones
            // we should see.
            switch (hdr->evt)
            {
            case EVT_CMD_STATUS:
               if (len >= sizeof(evt_cmd_status))
               {
                  auto* cs = (evt_cmd_status*)p;
                  if (cs->opcode == request.opcode)
                  {
                     if (cs->status == 0)
                     {
                        // Pending command... wait longer.
                     }
                     else
                     {
                        // Error. We won't get any further response. List of codes is here:
                        // https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/architecture,-mixing,-and-conventions/controller-error-codes.html
                        std::cout << "   Error response: " << cs->status << '\n';
                        return false;
                     }
                  }
               }
               break;
            case EVT_CMD_COMPLETE:
               if (len >= sizeof(evt_cmd_complete))
               {
                  auto* cc = (evt_cmd_complete*)p;
                  p += sizeof(evt_cmd_complete);
                  len -= sizeof(evt_cmd_complete);
                  if (cc->opcode == request.opcode)
                  {
                     // Remaining bytes depend on the command sent.
                     if (response)
                     {
                        if (len >= sizeof(ResponseT))
                        {
                           memcpy(response, p, sizeof(ResponseT));
                           // TODO: Not every command complete event has a
                           //       connection handle, but every response we
                           //       expect does. This code will break if we
                           //       try to use a command that doesn't have
                           //       this.
                           // if (response->handle == m_connection_id)
                           //    return true;
                           // else this is somebody else's response. ignore it.
                        }
                        else
                        {
                           // Not sure whether to count this as a success or
                           // failure. We got a response, but we don't know
                           // to read it.
                           return false;
                        }
                     }
                     else
                        return true;
                  }
                  else
                  {
                     std::cout << "Whoops, we just consumed somebody else's command response.\n";
                  }
               }
               else
               {
                  std::cout << "Just read a truncated command complete. I'm confused.\n";
               }
               break;
            case EVT_LE_META_EVENT:
               if (len >= sizeof(evt_le_meta_event))
               {
                  auto* me = (evt_le_meta_event*)p;
                  p += sizeof(evt_le_meta_event);
                  len -= sizeof(evt_le_meta_event);

                  if (meta_sub_event && me->subevent == meta_sub_event)
                  {
                     // Remaining bytes depend on the subevent code.
                     if (response)
                     {
                        if (len >= sizeof(ResponseT))
                        {
                           memcpy(response, p, sizeof(ResponseT));
                           // TODO: Not every meta event has a connection
                           //       handle, but every response we expect
                           //       does. This code will break if we try to
                           //       use a command that doesn't have this.
                           // if (response->handle == m_connection_id)
                           //    return true;
                           // else This is somebody else's response. Ignore it.
                        }
                        else
                        {
                           // Not sure whether to count this as a success or
                           // failure. We got a response, but we don't know
                           // to read it.
                           return false;
                        }
                     }
                     else
                        return true;
                  }
                  else
                  {
                     std::cout << "Whoops, we just consumed somebody else's meta update.\n";
                  }
               }
               else
               {
                  std::cout << "Just read a truncated meta event. I'm confused.\n";
               }
               break;
            }
         }
      }

      return true;
   }
   return false;
}


bool RawHci::HandleFromSocket(int sock, uint16_t* handle)
{
   struct l2cap_conninfo ci{};
   socklen_t size = sizeof(ci);
   int err = getsockopt(sock, SOL_L2CAP, L2CAP_CONNINFO, &ci, &size);
   if (err < 0)
      return false;
   *handle = ci.hci_handle;
   return true;
}
