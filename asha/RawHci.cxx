#include "RawHci.hh"

#include <iomanip>
#include <iostream>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <poll.h>

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


RawHci::RawHci(const std::string& mac) noexcept
{
   // TODO: Is there a better way to do this? Ideally we would map the
   //       connection fd to the bluetooth connection id, but that info
   //       is stored in an unaccessible kernel structure somewhere. For
   //       now, just assume there is only one connection per device, and
   //       use the device mac address to find it. Presumably the connection
   //       we want is just the newest one.

   // MAC address of device we are searching for.
   bdaddr_t mac_addr{};
   if (6 != sscanf(mac.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                     &mac_addr.b[5],&mac_addr.b[4],&mac_addr.b[3],
                     &mac_addr.b[2],&mac_addr.b[1],&mac_addr.b[0]
                  ))
      return;
   
   // Use raw bluetooth socket ioctls to get the info needed.
   // First, get a list of devices.
   int sock = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
   if (sock < 0) return;

   char buffer[sizeof(hci_dev_list_req) + HCI_MAX_DEV * sizeof(hci_dev_req)] = {};
   auto dev_list = (hci_dev_list_req*)buffer;
   dev_list->dev_num = HCI_MAX_DEV;

   if (ioctl(sock, HCIGETDEVLIST, buffer) < 0)
   {
      close(sock);
      return;
   }

   for (size_t i = 0; i < dev_list->dev_num; ++i)
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
               if (memcmp(&conn_list->conn_info[j].bdaddr, &mac_addr, 6) == 0)
               {
                  // This is the device we are looking for
                  m_device_id = conn_list->dev_id;
                  m_connection_id = conn_list->conn_info[j].handle;

                  // Keep searching, in case we find a higher id one (which is
                  // presumably newer).
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
}

RawHci::~RawHci() noexcept
{
   if (m_sock >= 0)
      close(m_sock);
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
   if (m_connection_id != INVALID_ID && (m_sock >= 0))
   {
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

      std::cout << "request: ";
      DumpHex((uint8_t*)&msg, sizeof(msg));
      std::cout << '\n';
      while (send(m_sock, &msg, sizeof(msg), 0) < 0)
      {
         if (errno != EAGAIN && errno != EINTR)
         {
            int e = errno;
            std::cout << "Unable to send command: " << strerror(e) << '\n';
            return false;
         }
      }

      // TODO: Wait for and check response? I don't actually want to absorb a
      //       response that bluez wants, but hopefully a filter will handle
      //       that.

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
                     if (cs->opcode == msg.opcode)
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
                     if (cc->opcode == msg.opcode)
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
                              if (response->handle == m_connection_id)
                                 return true;
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
                              if (response->handle == m_connection_id)
                                 return true;
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
      }

      return true;
   }
   return false;
}