#include <cassert>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <unordered_map>
#include <vector>

// No ntoh64 on my box :(
template <typename T>
inline T NetSwap(T v)
{
   if (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
   {
      switch (sizeof(v))
      {
      case 2: return __bswap_16(v);
      case 4: return __bswap_32(v);
      case 8: return __bswap_64(v);
      }
   }
   return v;
}

template <typename T>
inline T BtSwap(T v)
{
   if (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
   {
      switch (sizeof(v))
      {
      case 1: return v;
      case 2: return __bswap_16(v);
      case 4: return __bswap_32(v);
      case 8: return __bswap_64(v);
      }
   }
   return v;
}

template <typename T>
inline std::string Hex(const T& v)
{
   std::stringstream ss;
   ss << std::hex << std::setfill('0') << "0x" << std::setw(sizeof(T)*2) << (uint64_t)v;
   return ss.str();
}
template <>
inline std::string Hex(const std::vector<uint8_t>& bytes)
{
   std::stringstream ss;
   ss << std::hex << std::setfill('0');
   bool first = true;
   for (uint8_t b: bytes)
   {
      if (!first)
         ss << ' ';
      first = false;
      ss << std::setw(2) << (uint64_t)b;
   }
    
   return ss.str();
}

std::string ToString(const std::vector<uint8_t>& bytes)
{
   bool printable = true;
   size_t null_count = 0;
   for (uint8_t b: bytes)
   {
      if (b == 0)
         ++null_count;
      // else if (0 < b && b < 7)
      // {
      //    printable = false;
      //    break;
      // }
      // else if (13 < b && b < 20)
      // {
      //    printable = false;
      //    break;
      // }
      // else if (126 < b)
      // {
      //    printable = false;
      //    break;
      // }
      else if (b < ' ' || b > '~')
      {
         printable = false;
         break;
      }
   }
   if (null_count > 1 || null_count == 1 && bytes.back() != 0)
      printable = false;

   std::string ret;
   if (printable)
   {
      if (bytes.size() > 40)
         ret = std::string(bytes.begin(), bytes.begin() + 40).c_str() + std::string("...");
      else
         ret = std::string(bytes.begin(), bytes.begin() + bytes.size()).c_str();
   }
   else
   {
      if (bytes.size() > 20)
         ret = Hex(std::vector<uint8_t>(bytes.begin(), bytes.begin() + 20)) + " plus " + std::to_string(bytes.size() - 20) + " more";
      else
         ret = Hex(bytes);
   }
   return ret;
}


// Monitor snoop opcodes.
static constexpr uint16_t NEW_INDEX = 0;
static constexpr uint16_t DEL_INDEX = 1;
static constexpr uint16_t COMMAND_PKT = 2;
static constexpr uint16_t EVENT_PKT = 3;
static constexpr uint16_t ACL_TX_PKT = 4;
static constexpr uint16_t ACL_RX_PKT = 5;
static constexpr uint16_t SCO_TX_PKT = 6;
static constexpr uint16_t SCO_RX_PKT = 7;
static constexpr uint16_t OPEN_INDEX = 8;
static constexpr uint16_t CLOSE_INDEX = 9;
static constexpr uint16_t INDEX_INFO = 10;
static constexpr uint16_t VENDOR_DIAG = 11;
static constexpr uint16_t SYSTEM_NOTE = 12;
static constexpr uint16_t USER_LOGGING = 13;
static constexpr uint16_t CTRL_OPEN = 14;
static constexpr uint16_t CTRL_CLOSE = 15;
static constexpr uint16_t CTRL_COMMAND = 16;
static constexpr uint16_t CTRL_EVENT = 17;
static constexpr uint16_t ISO_TX_PKT = 18;
static constexpr uint16_t ISO_RX_PKT = 19;

// LE Supported Features (The ones we care about anyways)
static constexpr uint64_t FEATURE_DLE   = 0x0020;
static constexpr uint64_t FEATURE_2MPHY = 0x0100;

// Command Opcodes
#define OPCODE(ocf, ogf) ((ocf) << 10 | ogf)
static constexpr uint16_t LE_CREATE_CONNECTION = OPCODE(0x08, 0x00D);
static constexpr uint16_t LE_EXTENDED_CREATE_CONNECTION = OPCODE(0x08, 0x043);

// Special UUIDs
static const std::string GATT_SERVICES         = "00002800-0000-1000-8000-00805f9b34fb";
static const std::string GATT_SECONDARY        = "00002801-0000-1000-8000-00805f9b34fb";
static const std::string GATT_INCLUDE          = "00002802-0000-1000-8000-00805f9b34fb";
static const std::string GATT_CHARACTERISTICS  = "00002803-0000-1000-8000-00805f9b34fb";

static const std::string GATT_CHAR_DESCRIPTION = "00002901-0000-1000-8000-00805f9b34fb";
static const std::string GATT_CCC              = "00002902-0000-1000-8000-00805f9b34fb";

const std::unordered_map<std::string, std::string> KNOWN_UUIDS = {
   { GATT_SERVICES, "Services"},
   { GATT_SECONDARY, "Secondary"},
   { GATT_INCLUDE, "Include"},
   { GATT_CHARACTERISTICS, "Characteristics"},
   { GATT_CHAR_DESCRIPTION, "Description"},
   { GATT_CCC, "CCC"},

   { "0000fdf0-0000-1000-8000-00805f9b34fb", "ASHA"},
   { "6333651e-c481-4a3e-9169-7c902aad37bb", "ReadOnlyProperties"},
   { "f0d4de7e-4a88-476c-9d9f-1937b0996cc0", "AudioControlPoint"},
   { "38663f1a-e711-4cac-b641-326b56404837", "AudioStatus"},
   { "00e4ca9e-ab14-41e4-8823-f9e70c7e91df", "Volume"},
   { "2d410339-82b6-42aa-b34e-e2e01df8cc1a", "LE_PSM_OUT"},
};

// Wrapper around a buffer that can read and convert bytes and make sure we
// don't overflow.
class BtBufferStream
{
public:
   BtBufferStream(const char* context, const uint8_t* p, size_t l): m_context{context}, m_p{p}, m_l{l} {}

   const uint8_t* Data() const { return m_p; }
   size_t Size() const { return m_l; }

   uint8_t  U8()  { return *Bytes(1); }
   uint16_t U16() { return BtSwap(*(uint16_t*)(Bytes(2))); }
   uint32_t U32() { return BtSwap(*(uint32_t*)(Bytes(4))); }
   uint64_t U64() { return BtSwap(*(uint64_t*)(Bytes(8))); }

   void Skip(size_t count) { Bytes(count); }

   BtBufferStream Sub(const char* context) { return Sub(context, m_l); }
   BtBufferStream Sub(const char* context, size_t count) { return BtBufferStream(context, Bytes(context, count), count); }

   std::string UUID()
   {
      if (m_l == 2) return UUID16();
      if (m_l == 16) return UUID128();
      throw std::logic_error("Not enough context to determine the uuid size");
   }

   std::string UUID16()
   {
      uint16_t uuid = U16();
      std::stringstream ss;
      ss << std::hex << std::setfill('0');
      ss << "0000"
         << std::setw(4) << uuid
         << "-0000-1000-8000-00805f9b34fb";
      return ss.str();
   }

   std::string UUID128()
   {
      const uint8_t* data = Bytes(16);
      std::stringstream ss;
      ss << std::hex << std::setfill('0');
      for (int i = 15; i >= 0; --i)
      {
         ss << std::setw(2) << (unsigned)data[i];
         if (i == 12 || i == 10 || i == 8 || i == 6)
            ss << '-';
      }
      return ss.str();
   }

   std::string Mac()
   {
      const uint8_t* data = Bytes(6);
      std::stringstream ss;
      ss << std::hex << std::setfill('0');
      // The mac is in there backwards.
      ss << std::setw(2) << (unsigned)data[5] << ':' << std::setw(2) << (unsigned)data[4] << ':' << std::setw(2) << (unsigned)data[3] << ':'
         << std::setw(2) << (unsigned)data[2] << ':' << std::setw(2) << (unsigned)data[1] << ':' << std::setw(2) << (unsigned)data[0];
      return ss.str();
   }

   void Error(const char* e)
   {
      throw std::runtime_error(std::string(m_context) + " " + e);
   }

protected:
   const uint8_t* Bytes(size_t count) { return Bytes(m_context, count); }
   const uint8_t* Bytes(const char* context, size_t count)
   {
      if (count > m_l)
         throw std::runtime_error(std::string(context) + " truncated");
      const uint8_t* ret = m_p;
      m_p += count;
      m_l -= count;
      return ret;
   }

private:
   const char* m_context = nullptr;
   const uint8_t* m_p = nullptr;
   size_t m_l = 0;
};


// Can load a btsnoop file, and return packets one at a time.
class BtSnoopFile final
{
public:
   BtSnoopFile(std::istream& in): m_in{in}
   {
      Reset();
   }

   void Reset()
   {
      // Read file header.
      m_in.seekg(0);
      char magic[8] = {};
      m_in.read(magic, 8);
      uint32_t version;
      if (memcmp(magic, "btsnoop\0", 8) != 0 ||
          !ReadInt(&version) || version != 1 ||
          !ReadInt(&m_type) || !(m_type == MONITOR_FILE || m_type == HCI_FILE))
         throw std::runtime_error("Not a bluetooth snoop file");
   }

   struct Packet
   {
      uint32_t length;
      uint16_t idx;
      uint16_t opcode;
      uint64_t stamp;
      uint8_t data[1024];
      bool eof;

      operator bool() const { return !eof; }
   };

   const Packet& Next()
   {
      uint32_t original_length;
      uint32_t drops;
      uint32_t flags;
      if (ReadInt(&original_length) &&
          ReadInt(&m_packet.length) &&
          original_length == m_packet.length &&
          ReadInt(&flags) &&
          ReadInt(&drops) &&
          ReadInt(&m_packet.stamp) &&
          m_packet.length <= sizeof(m_packet.data) &&
          m_in.read((char*)m_packet.data, m_packet.length))
      {
         // TODO: How to read an HCI file?
         if (m_type == MONITOR_FILE)
         {
            m_packet.opcode = flags & 0xffff;
            m_packet.idx = flags >> 16;
         }
         else
         {
            m_packet.idx = 0;
            switch (flags & 0x3)
            {
            case 0: m_packet.opcode = ACL_TX_PKT; break;
            case 1: m_packet.opcode = ACL_RX_PKT; break;
            case 2: m_packet.opcode = COMMAND_PKT; break;
            case 3: m_packet.opcode = EVENT_PKT; break;
            default: m_packet.opcode = 0xffff;
            }
         }
         m_packet.eof = false;
         return m_packet;
      }
      m_packet.length = 0;
      m_packet.eof = true;
      return m_packet;
   }

   static constexpr uint32_t MONITOR_FILE = 2001;
   static constexpr uint32_t HCI_FILE = 1001;
   uint32_t Type() const { return m_type; }

private:

   template<typename T>
   bool ReadInt(T* v)
   {
      if (m_in.read((char*)v, sizeof(*v)))
      {
         *v = NetSwap(*v);
         return true;
      }
      return false;
   }


   std::istream& m_in;
   Packet m_packet{};
   uint32_t m_type = 0;
};

class BtParser final
{
public:

   void Parse(uint16_t opcode, BtBufferStream& b)
   {
      // TODO: Opcode meaning changes depending on the file type.
      switch(opcode)
      {
      case COMMAND_PKT: CommandPkt(b.Sub("HCI Command")); break;
      case EVENT_PKT:   EventPkt(b.Sub("HCI Event")); break;
      case ACL_TX_PKT:  AclPkt(false, b.Sub("ACL TX Packet")); break;
      case ACL_RX_PKT:  AclPkt(true, b.Sub("ACL RX Packet")); break;
      case NEW_INDEX:   /* TODO: read adapter mac here, to get cache path in /var/lib/bluetooth */ break;
      case SYSTEM_NOTE:
         if (NoteCallback)
            NoteCallback(std::string(b.Data(), b.Data() + b.Size()));
         break;
      }
   }

   std::function<void(const std::string& s)> NoteCallback;
   std::function<void(uint16_t connection_handle, uint8_t status, const std::string& mac, uint16_t interval, uint16_t latency, uint16_t timeout)> ConnectionCallback;
   std::function<void(uint16_t connection_handle, uint16_t tx_dlen, uint16_t tx_time, uint16_t rx_dlen, uint16_t rx_time)> DleChange;
   std::function<void(uint16_t connection_handle, uint64_t features)> RemoteFeatures;
   std::function<void(uint16_t connection_handle, uint16_t handle, uint16_t end_handle, const std::string& uuid)> Service;
   std::function<void(uint16_t connection_handle, uint16_t handle, uint16_t value, uint8_t props, const std::string& uuid)> Characteristic;
   std::function<void(uint16_t connection_handle, uint16_t char_handle, uint16_t desc_handle, const std::string& uuid)> Descriptor;
   std::function<void(uint16_t connection_handle, uint16_t handle, const std::vector<uint8_t>& bytes)> Write;
   std::function<void(uint16_t connection_handle, uint16_t handle, const std::vector<uint8_t>& bytes)> Read;
   std::function<void(uint16_t connection_handle, uint16_t handle, uint8_t code)> FailedWrite;
   std::function<void(uint16_t connection_handle, uint16_t handle, uint8_t code)> FailedRead;




   struct GattCharacteristic
   {
      uint16_t handle;
      uint16_t value;
      uint16_t ccc;
      uint16_t description;
      uint8_t properties;
      std::string uuid;
   };
   struct GattService
   {
      uint16_t handle;
      uint16_t end_handle;

      std::string uuid;
   };
   struct HandleInfo
   {
      enum {INVALID, SERVICE, SERVICE_CHANGED, CHAR, CHAR_CCC, CHAR_DESCRIPTION, CHAR_VALUE, CHAR_DESCRIPTOR} type = INVALID;
      uint16_t handle = 0;
      GattService* service = nullptr;
      GattCharacteristic* characteristic = nullptr;
   };

   HandleInfo FindHandle(uint16_t connection, uint16_t handle)
   {
      auto& conn = m_connections[connection];

      GattService* pservice = nullptr;

      // First, check if it is a service. If it isn't, which service does it
      // belong to?
      for (auto& s: conn.services)
      {
         if (s.first == handle) return HandleInfo{HandleInfo::SERVICE, handle, &s.second};
         if (s.second.end_handle == handle) return HandleInfo{HandleInfo::SERVICE_CHANGED, handle, &s.second};

         if (s.second.handle < handle && handle < s.second.end_handle)
         {
            pservice = &s.second;
            break;
         }
      }

      // Then check the characteristics.
      for (auto& c: conn.characteristic)
      {
         if (c.first == handle) return HandleInfo{HandleInfo::CHAR, handle, pservice, &c.second};
         if (c.second.ccc == handle) return HandleInfo{HandleInfo::CHAR_CCC, handle, pservice, &c.second};
         if (c.second.description == handle) return HandleInfo{HandleInfo::CHAR_DESCRIPTION, handle, pservice, &c.second};
         if (c.second.value == handle) return HandleInfo{HandleInfo::CHAR_VALUE, handle, pservice, &c.second};
         // TODO: handle other descriptor types.
      }
      return HandleInfo{};
   }

   std::string HandleDescription(uint16_t connection, uint16_t handle)
   {
      return HandleDescription(FindHandle(connection, handle));
   }
   std::string UuidStr(const std::string& uuid)
   {
      auto it = KNOWN_UUIDS.find(uuid);
      return it == KNOWN_UUIDS.end() ? uuid : it->second;
   }
   std::string HandleDescription(const HandleInfo& info)
   {
      switch (info.type)
      {
      case HandleInfo::SERVICE:           return UuidStr(info.service->uuid);
      case HandleInfo::SERVICE_CHANGED:   return UuidStr(info.service->uuid) + " onchange";
      case HandleInfo::CHAR:              return UuidStr(info.characteristic->uuid);
      case HandleInfo::CHAR_CCC:          return UuidStr(info.characteristic->uuid) + " ccc";
      case HandleInfo::CHAR_DESCRIPTION:  return UuidStr(info.characteristic->uuid) + " description";
      case HandleInfo::CHAR_VALUE:        return UuidStr(info.characteristic->uuid) + " value";
      case HandleInfo::CHAR_DESCRIPTOR:   return UuidStr(info.characteristic->uuid) + " descriptor";
      default: return "unknown";
      }
   }

protected:
   void CommandPkt(BtBufferStream pkt)
   {
      uint16_t opcode = pkt.U16();
      switch (opcode)
      {
      case LE_CREATE_CONNECTION:
      {
         BtBufferStream b = pkt.Sub("LE Create Connection", 25);
         b.Skip(13);
         m_pending_connection = PendingConnectionInfo{};
         m_pending_connection.phy = 1;
         m_pending_connection.p1m.interval_min = b.U16();
         m_pending_connection.p1m.interval_max = b.U16();
         m_pending_connection.p1m.latency = b.U16();
         m_pending_connection.p1m.timeout = b.U16();
         m_pending_connection.p1m.celength_min = b.U16();
         m_pending_connection.p1m.celength_max = b.U16();
         break;
      }
      case LE_EXTENDED_CREATE_CONNECTION:
      {
         BtBufferStream b = pkt.Sub("LE Extended Create Connection");
         b.Skip(10);
         m_pending_connection = PendingConnectionInfo{};
         m_pending_connection.phy = b.U8();
         
         auto ReadPhyParam = [&](uint16_t& param1m, uint16_t& param2m){
            for (uint8_t i = 0; i < 8; ++i)
            {
               if (m_pending_connection.phy & (1 << i))
               {
                  uint16_t param = b.U16();
                  if (i == 0)
                     param1m = param;
                  else if (i == 1)
                     param2m = param;
                  // else if (i == 2) // coded
               }
            }
         };
         uint16_t unused;
         ReadPhyParam(unused, unused); // scan interval
         ReadPhyParam(unused, unused); // scan window
         ReadPhyParam(m_pending_connection.p1m.interval_min, m_pending_connection.p2m.interval_min);
         ReadPhyParam(m_pending_connection.p1m.interval_max, m_pending_connection.p2m.interval_max);
         ReadPhyParam(m_pending_connection.p1m.latency, m_pending_connection.p2m.latency);
         ReadPhyParam(m_pending_connection.p1m.timeout, m_pending_connection.p2m.timeout);
         ReadPhyParam(m_pending_connection.p1m.celength_min, m_pending_connection.p2m.celength_min);
         ReadPhyParam(m_pending_connection.p1m.celength_max, m_pending_connection.p2m.celength_max);

         break;
      }
      }
   }

   void LeMetaEvent(BtBufferStream pkt)
   {
      uint8_t length = pkt.U8();
      if (length > pkt.Size())
         pkt.Error("bad length");
      uint8_t subcode = pkt.U8();
      switch(subcode)
      {
      case 0x01:     // LE Connection Complete
      {
         BtBufferStream b = pkt.Sub("LE Connection Complete");
         uint8_t status = b.U8();
         uint16_t handle = b.U16();
         b.U8(); // Role
         auto& conn = m_connections[handle] = ConnectionInfo{};
         conn.handle = handle;
         conn.type = ConnectionInfo::L2CAP;
         conn.mac = b.Mac();
         conn.interval = b.U16();
         conn.latency  = b.U16();
         conn.timeout  = b.U16();
         // b.U8(); // clock accuracy
         // TODO: The pending connection object can have both phy2m and
         //       and phy1m parameters. How do we know which was selected?
         conn.celength = m_pending_connection.p1m.celength_min;
         conn.phy = m_pending_connection.phy & 2 ? ConnectionInfo::PHY2M : ConnectionInfo::PHY1M;
         if (ConnectionCallback)
            ConnectionCallback(handle, status, conn.mac, conn.interval, conn.latency, conn.timeout);
         
         break;
      }
      case 0x0a:     // LE Enhanced Connection Complete
      {
         BtBufferStream b = pkt.Sub("LE Enhanced Connection Complete");
         uint8_t status = b.U8();
         uint16_t handle = b.U16();
         auto& conn = m_connections[handle] = ConnectionInfo{};
         conn.handle = handle;
         b.Skip(2); // role/peer address type
         conn.type = ConnectionInfo::L2CAP;
         conn.mac = b.Mac();
         b.Skip(12); // local/peer resolvable macs
         conn.interval = b.U16();
         conn.latency = b.U16();
         conn.timeout = b.U16();
         b.U8(); // clock accuracy
         // TODO: The pending connection object can have both phy2m and
         //       and phy1m parameters. How do we know which was selected?
         conn.celength = m_pending_connection.p1m.celength_min;
         conn.phy = m_pending_connection.phy & 2 ? ConnectionInfo::PHY2M : ConnectionInfo::PHY1M;
         if (ConnectionCallback)
            ConnectionCallback(conn.handle, status, conn.mac, conn.interval, conn.latency, conn.timeout);
      
         break;
      }
      case 0x07:     // LE Data Length Change
      {
         BtBufferStream b = pkt.Sub("LE Data Length Change");
         uint16_t handle = b.U16();
         auto& conn = m_connections[handle];
         conn.tx_dlen = b.U16();
         conn.tx_time = b.U16();
         conn.rx_dlen = b.U16();
         conn.rx_time = b.U16();
         if (DleChange)
            DleChange(conn.handle, conn.tx_dlen, conn.tx_time, conn.rx_dlen, conn.rx_time);
         
         break;
      }
      case 0x04:     // LE Read Remote Features
      {
         BtBufferStream b = pkt.Sub("LE Data Length Change");
         uint8_t status =  b.U8();
         uint16_t handle = b.U16();
         uint64_t flags =  b.U64();
         auto& conn = m_connections[handle];
         conn.features = flags;

         if (RemoteFeatures)
            RemoteFeatures(handle, flags);
         break;
      }
      }
   }

   void EventPkt(BtBufferStream pkt)
   {
      switch (pkt.U8())
      {
      case 0x3e:  LeMetaEvent(pkt.Sub("LE Meta Event")); break;
      }
   }

   void FindInformationResponse(uint16_t connection_handle, BtBufferStream b)
   {
      uint8_t format = b.U8();
      if (format != 1 && format != 2) // 16 bit / 128 bit uuids
         throw std::runtime_error("implement additional uuid types please!"); // don't know what this is.
      size_t response_size = format == 1 ? 4 : 18;
      size_t count = b.Size() / response_size;
      
      auto& conn = m_connections[connection_handle];

      for (size_t i = 0; i < count; ++i)
      {
         BtBufferStream binfo = b.Sub("Information Data", response_size);
         uint16_t handle = binfo.U8();
         std::string uuid = format == 1 ? binfo.UUID16() : binfo.UUID128();

         // I'm a little fuzzy on how to find the correct handle here. The
         // central would request these for any gaps in the handles, and so
         // would know which characteristic or service this belongs to.
         // For now, assuming that handle is not in the characteristic list,
         // lower_bound returns the element after the one we want, so go to the
         // previous element to get the associated characteristic.
         auto it = conn.characteristic.lower_bound(handle);
         if (it != conn.characteristic.begin())
         {
            --it;

            if (uuid == GATT_CHAR_DESCRIPTION)
               it->second.description = handle;
            else if (uuid == GATT_CCC)
               it->second.ccc = handle;
            if (Descriptor)
               Descriptor(connection_handle, it->first, handle, uuid);

         }
      }
   }

   void ReadByTypeResponse(uint16_t connection_handle, bool group, BtBufferStream b)
   {
      uint8_t response_size = b.U8();
      size_t attribute_count = response_size == 0 ? 0 : b.Size() / response_size;

      auto& conn = m_connections[connection_handle];

      for (size_t i = 0; i < attribute_count; ++i)
      {
         // If response_size was invalid here, then attribute count would be zero.
         BtBufferStream rsp = b.Sub(group ? "Read By Type" : "Read By Group Type", response_size);
         uint16_t handle = rsp.U16();
         uint16_t end_handle = group ? rsp.U16() : 0;
         
         if (conn.current_uuid == GATT_SERVICES)
         {
            std::string uuid = rsp.UUID();

            auto& service = conn.services[handle];
            service.handle = handle;
            service.end_handle = end_handle;
            service.uuid = uuid;
            if (Service)
               Service(connection_handle, handle, end_handle, uuid);
         }
         else if (conn.current_uuid == GATT_CHARACTERISTICS)
         {
            uint8_t properties = rsp.U8(); // bitmask (Extended, authenticated writes, indicate, notify, write, command, read, broadcast)
            uint16_t value_handle = rsp.U16();
            std::string uuid = rsp.UUID();

            auto& characteristic = conn.characteristic[handle];
            characteristic.handle = handle;
            characteristic.value = value_handle;
            characteristic.properties = properties;
            characteristic.uuid = uuid;
            if (Characteristic)
               Characteristic(connection_handle, handle, value_handle, properties, uuid);
         }
      }
      conn.current_uuid.clear();
   }

   void AttributeError(uint16_t handle, BtBufferStream b)
   {
      auto& conn = m_connections[handle];
      uint8_t original_method = b.U8() & 0x3f;
      uint16_t handle_in_error = b.U16();
      uint8_t error_code = b.U8();
      switch (original_method)
      {
      case 0x11: // Read by group type request
         conn.current_uuid.clear(); // error means we are done reading these.
         break;
      case 0x0a: // Read
         assert(conn.pending_handle == handle_in_error); // remove later, just making sure I did this right;
         if (FailedRead)
            FailedRead(handle, conn.pending_handle, error_code);
         conn.pending_handle = 0;
         break;
      case 0x12: // Write
         assert(conn.pending_handle == handle_in_error); // remove later, just making sure I did this right;
         if (FailedWrite)
            FailedWrite(handle, conn.pending_handle, error_code);
         conn.pending_handle = 0;
         break;
      }
   }
   
   void WriteRequest(uint16_t handle, BtBufferStream b)
   {
      uint16_t write_handle = b.U16();
      m_connections[handle].pending_handle = write_handle;
      if (Write)
         Write(handle, write_handle, std::vector<uint8_t>(b.Data(), b.Data() + b.Size()));
   }

   void WriteResponse(uint16_t handle, BtBufferStream b)
   {
      // b should be empty. This is a successful write.
   }

   void ReadRequest(uint16_t handle, BtBufferStream b)
   {
      m_connections[handle].pending_handle = b.U16();
   }

   void ReadResponse(uint16_t handle, BtBufferStream b)
   {
      auto& conn = m_connections[handle];
      if (conn.pending_handle)
      {
         if (Read)
            Read(handle, conn.pending_handle, std::vector<uint8_t>(b.Data(), b.Data() + b.Size()));

         conn.pending_handle = 0;
      }
   }


   void ParseAttribute(bool rx, uint16_t handle, BtBufferStream b)
   {
      auto& conn = m_connections[handle];

      uint8_t opcode = b.U8();
      bool command = opcode & 0x40;
      uint8_t method = opcode & 0x3f;
      switch(method)
      {
      case 0x01: // Error response
         AttributeError(handle, b.Sub("Attribute Error Response", 4));
         break;
      case 0x02: // Exchange MTU Request. Fallthrough intentional
      case 0x03: // Exchange MTU Response
         (rx ? conn.peripheral_mtu : conn.central_mtu) = b.Sub("Exchange MTU").U16();
         break;
      case 0x05: // Find Information Response
         FindInformationResponse(handle, b.Sub("Find Information Response"));
         break;
      case 0x08: // Read by type request. Fallthrough intentional
      case 0x10: // Read by group type request
      {
         BtBufferStream bg = b.Sub("Read By Type Request");
         bg.Skip(4); // Starting/ending handle.
         conn.current_uuid = bg.UUID();
         break;
      }
      case 0x09: // Read By Type Response
         ReadByTypeResponse(handle, false, b.Sub("Read By Type Response"));
         break;
      case 0x11: // Read By Group Type Response
         ReadByTypeResponse(handle, true, b.Sub("Read By Group Type Response"));
         break;
      case 0x12: // Write Request
         WriteRequest(handle, b.Sub("Write Request"));
         break;
      case 0x13: // Write Response
         WriteResponse(handle, b.Sub("Write Response"));
         break;
      case 0x0a: // Read Request
         ReadRequest(handle, b.Sub("Read Request"));
         break;
      case 0x0b: // Read Response
         ReadResponse(handle, b.Sub("Read Response"));
         break;
      }
   }
   
   void AclPkt(bool rx, BtBufferStream pkt)
   {
      uint16_t header = pkt.U16();
      uint16_t handle = header & 0x0fff;
      uint16_t length = pkt.U16();
      uint16_t boundary = (header >> 12) & 0x3;
      uint16_t broadcast = (header >> 14) & 0x3;
      
      auto it = m_connections.find(handle);
      if (it == m_connections.end())
         return; // We don't have the context necessary to understand this packet.
      if (it->second.type == ConnectionInfo::L2CAP)
      {
         BtBufferStream b = pkt.Sub("L2CAP", length);
         std::vector<uint8_t> fragment_data;
         // length is already correct.
         if (it->second.fragment.empty())
         {
            if (b.Size() < 4)
               throw std::runtime_error("Truncated TX L2CAP packet header");
            // Peek at packet length in stream
            it->second.expected_fragment_size = BtSwap(*(uint16_t*)(b.Data() + 0)) + 4;
            if (it->second.expected_fragment_size > b.Size())
            {
               it->second.fragment.assign(b.Data(), b.Data() + length);
               return; // Need to wait for more data.
            }
         }
         else
         {
            it->second.fragment.insert(it->second.fragment.end(), b.Data(), b.Data() + b.Size());
            if (it->second.fragment.size() < it->second.expected_fragment_size)
               return; // Need to wait for more data.
            fragment_data.swap(it->second.fragment);
            b = BtBufferStream("L2CAP", fragment_data.data(), fragment_data.size());
            length = it->second.expected_fragment_size;
         }
         uint16_t l2cap_length = b.U16();
         uint16_t cid = b.U16();
         assert(b.Size() == l2cap_length); // In case we screwed up the reassembly logic.

         switch (cid)
         {
         case 0x0004: // Attribute Protocol
            ParseAttribute(rx, handle, b.Sub("Attribute Protocol"));
            break;
         }
      }
   }

private:
   struct ConnectionInfo
   {
      uint16_t handle = 0;
      enum {NONE, L2CAP, LE_SOCKET} type = NONE;
      std::string mac;
      enum {PHY1M, PHY2M} phy{};
      uint16_t interval = 0;
      uint16_t latency = 0;
      uint16_t timeout = 0;
      uint16_t celength = 0;
      uint16_t tx_dlen = 0;
      uint16_t tx_time = 0;
      uint16_t rx_dlen = 0;
      uint16_t rx_time = 0;
      uint64_t features = 0;
      uint16_t peripheral_mtu = 0;
      uint16_t central_mtu = 0;

      std::map<uint16_t, GattService> services;
      std::map<uint16_t, GattCharacteristic> characteristic;
      

      // These variables represent temporary state while parsing.
      std::string current_uuid;        // UUID for group read requests
      std::vector<uint8_t> fragment;   // Reassembled l2cap fragment.
      uint16_t expected_fragment_size = 0;
      uint16_t pending_handle = 0;
   };
   std::unordered_map<uint16_t, ConnectionInfo> m_connections;

   struct PendingConnectionInfo
   {
      uint8_t phy = 0;
      struct Parameters
      {
         uint16_t interval_min = 0;
         uint16_t interval_max = 0;
         uint16_t latency = 0;
         uint16_t timeout = 0;
         uint16_t celength_min = 0;
         uint16_t celength_max = 0;
      } p1m, p2m;
   };
   PendingConnectionInfo m_pending_connection;

};



int main(int argc, char** argv)
{
   std::string snoop_filename;
   if (argc > 1)
      snoop_filename = argv[1];

   std::ifstream infile;
   if (argc > 1)
      infile.open(argv[1], std::ios::binary);

   BtSnoopFile snoop(argc > 1 ? infile : std::cin);

   // Make a first pass through the file, trying to find relevant uuids.
   BtParser parser;
   parser.NoteCallback = [](const std::string& s) {
      std::cout << "System Note: " << s << '\n';
   };
   parser.ConnectionCallback = [](uint16_t connection, uint8_t status, const std::string& mac, uint16_t interval, uint16_t latency, uint16_t timeout) {
      std::cout << "New Connection: " << Hex(connection) << " " << mac << " params(" << interval << ", " << latency << ", " << timeout << ")\n";
   };
   parser.DleChange = [](uint16_t connection, uint16_t tx_dlen, uint16_t tx_time, uint16_t rx_dlen, uint16_t rx_time) {
      std::cout << "Dle Change:     " << Hex(connection) << " tx: " << tx_dlen << " " << tx_time << "μs   rx: " << rx_dlen << " " << rx_time << "μs\n";
   };
   parser.RemoteFeatures = [](uint16_t connection, uint64_t features) {
      std::cout << "Supported:      " << Hex(connection) << " DLE: " << (features & FEATURE_DLE ? "true": "false")
                                                     << " 2MPHY: " << (features & FEATURE_2MPHY ? "true": "false")
                                                     << '\n';
   };
   parser.Service = [](uint16_t connection, uint16_t handle, uint16_t end_handle, const std::string& uuid) {
      std::cout << "Service:        " << Hex(connection) << " " << Hex(handle) << " " << Hex(end_handle) << " " << uuid << '\n';
   };
   parser.Characteristic = [](uint16_t connection, uint16_t handle, uint16_t value, uint8_t props, const std::string& uuid) {
      // std::string props;
      // if (properties & 0x01) props += "Broadcast ";
      // if (properties & 0x02) props += "Read ";
      // if (properties & 0x04) props += "Command ";
      // if (properties & 0x08) props += "Write ";
      // if (properties & 0x10) props += "Notify ";
      // if (properties & 0x20) props += "Indicate ";
      // if (properties & 0x40) props += "AuthWrite ";
      // if (properties & 0x80) props += "Extended ";
      // if (!props.empty()) props.pop_back();
      std::cout << "Characteristic: " << Hex(connection) << " " << Hex(handle) << " " << Hex(value) << " " << Hex(props) << " " << uuid << '\n';
   };
   parser.Descriptor = [](uint16_t connection, uint16_t char_handle, uint16_t desc_handle, const std::string& uuid) {
      std::cout << "Descriptor:     " << Hex(connection) << " " << Hex(char_handle) << " " << Hex(desc_handle) << " " << uuid << '\n';
   };
   parser.Write = [&](uint16_t connection, uint16_t handle, const std::vector<uint8_t>& bytes) {
      std::cout << "Write:          " << Hex(connection) << " " << Hex(handle) << " " << parser.HandleDescription(connection, handle) << " " << ToString(bytes) << '\n';
   };
   parser.Read = [&](uint16_t connection, uint16_t handle, const std::vector<uint8_t>& bytes) {
      std::cout << "Read:           " << Hex(connection) << " " << Hex(handle) << " " << parser.HandleDescription(connection, handle) << " " << ToString(bytes) << '\n';
   };
   parser.FailedWrite = [&](uint16_t connection, uint16_t handle, uint8_t code) {
      std::cout << "Failed Write:   " << Hex(connection) << " " << Hex(handle) << " " << parser.HandleDescription(connection, handle) << " " << code << '\n';
   };
   parser.FailedRead = [&](uint16_t connection, uint16_t handle, uint8_t code) {
      std::cout << "Failed Read:    " << Hex(connection) << " " << Hex(handle) << " " << parser.HandleDescription(connection, handle) << " " << code << '\n';
   };

   uint64_t frame_idx = 0;
   while (true)
   {
      auto& packet = snoop.Next();
      if (!packet)
         break;
      ++frame_idx;
      BtBufferStream b("Transport", packet.data, packet.length);
      try
      {
         parser.Parse(packet.opcode, b);
      }
      catch (const std::runtime_error& e)
      {
         std::cout << "Invalid packet " << frame_idx << ": " << e.what() << '\n';
      }
   }


   return 0;
}