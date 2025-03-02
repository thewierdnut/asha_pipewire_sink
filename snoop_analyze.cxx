#include <cassert>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <vector>


#include <dirent.h>
#include <sys/stat.h>

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
   ss << std::hex << std::setfill('0') << std::setw(sizeof(T)*2) << (uint64_t)v;
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

struct StreamCids
{
   // TODO: A lot of the logic that uses this struct relies on the capture
   //       being from the perspective of the central.
   uint16_t rx = 0;
   uint16_t tx = 0;

   bool operator<(const StreamCids& o) const
   {
      return (rx == o.rx ? tx < o.tx : rx < o.rx);
   }
   bool operator==(const StreamCids& o) const
   {
      return rx == o.rx && tx == o.tx;
   }
};


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

static const std::string DEVICE_NAME           = "00002a00-0000-1000-8000-00805f9b34fb";

static const std::string ASHA_READ_ONLY_PROPERTIES = "6333651e-c481-4a3e-9169-7c902aad37bb";
static const std::string ASHA_AUDIO_CONTROL_POINT  = "f0d4de7e-4a88-476c-9d9f-1937b0996cc0";
static const std::string ASHA_AUDIO_STATUS         = "38663f1a-e711-4cac-b641-326b56404837";
static const std::string ASHA_VOLUME               = "00e4ca9e-ab14-41e4-8823-f9e70c7e91df";
static const std::string ASHA_LE_PSM_OUT           = "2d410339-82b6-42aa-b34e-e2e01df8cc1a";

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

struct GattCharacteristic
{
   uint16_t handle;
   uint16_t value;
   uint16_t ccc;
   uint16_t description;
   uint8_t properties;
   std::string uuid;
   bool guess = false;
};
struct GattService
{
   uint16_t handle;
   uint16_t end_handle;

   std::string uuid;
};

struct L2CapCreditConnection
{
   bool outgoing = false;
   StreamCids cids{};
   uint16_t psm = 0;
   uint16_t mtu = 0; // Maximum pre-fragmented packet size
   uint16_t mps = 0; // Maximum fragment size
   int32_t tx_credits = 0;
};


class BtDatabase final
{
public:
   BtDatabase()
   {
      // First, try to load the bluez database. This requires root access though.
      LoadPath("/var/lib/bluetooth");
      // Next, try to load anything we have stored locally, so that we can
      // cache any mac addesses we find in snoop files.
      std::string home = getenv("HOME");
      LoadPath(home + "/.local/share/snoop_analyze");
   }
   ~BtDatabase()
   {
      // Dump the database to our local cache.
      std::string home = getenv("HOME");
      mkdir((home + "/.local/share/snoop_analyze").c_str(), 0770);
      mkdir((home + "/.local/share/snoop_analyze/cache").c_str(), 0770);
      for (auto& kv: m_info)
      {
         if (kv.first.empty() || kv.first == s_default_mac)
            continue; // Don't dump empty or defaulted macs to the database.

         std::ofstream out(home + "/.local/share/snoop_analyze/cache/" + kv.first);

         // Index everything first so that we can output the interleaved data
         // with the handles in order.
         std::vector<std::pair<const GattService*, const GattCharacteristic*>> idx;
         idx.reserve(65536);
         for (auto& s: kv.second.services)
         {
            if (idx.size() <= s.handle)
               idx.resize(s.handle + 1);
            idx[s.handle].first = &s;
         }
         for (auto& c: kv.second.characteristics)
         {
            if (idx.size() <= c.handle)
               idx.resize(c.handle + 1);
            idx[c.handle].second = &c;
         }
         out << "[Attributes]\n";
         for (size_t i = 0; i < idx.size(); ++i)
         {
            if (idx[i].first)
               out << Hex((uint16_t)i) << "=2800:" << Hex(idx[i].first->end_handle) << ":" << idx[i].first->uuid << '\n';
            else if (idx[i].second)
            {
               auto& c = *idx[i].second;
               out << Hex((uint16_t)i) << "=2803:" << Hex(c.value) << ":" << Hex(c.properties) << ":" << c.uuid << '\n';
               if (c.ccc)
                  out << Hex(c.ccc) << "=" << GATT_CCC << '\n';
               if (c.description)
                  out << Hex(c.description) << "=" << GATT_CHAR_DESCRIPTION << '\n';
            }
         }
      }
   }

   static void SetDefaultMac(const std::string& mac)
   {
      s_default_mac.clear();
      for (auto c: mac)
         s_default_mac.push_back(std::tolower(c));
   }

   const std::vector<GattService>& Services(const std::string& mac) const
   {
      static std::vector<GattService> empty;
      auto it = m_info.find(mac.empty() ? s_default_mac : mac);
      return it == m_info.end() ? empty : it->second.services;
   }

   const std::vector<GattCharacteristic>& Characteristics(const std::string& mac) const
   {
      static std::vector<GattCharacteristic> empty;
      auto it = m_info.find(mac.empty() ? s_default_mac : mac);
      return it == m_info.end() ? empty : it->second.characteristics;
   }

   void CacheService(const std::string& mac, const GattService& service)
   {
      auto& info = m_info[mac.empty() ? s_default_mac : mac];
      for (auto& old_service: info.services)
      {
         if (old_service.uuid == service.uuid)
         {
            old_service = service;
            return;
         }
      }
      info.services.push_back(service);
   }

   void CacheCharacteristic(const std::string& mac, const GattCharacteristic& characteristic)
   {
      auto& info = m_info[mac.empty() ? s_default_mac : mac];
      for (auto& old_char: info.characteristics)
      {
         if (old_char.uuid == characteristic.uuid)
         {
            old_char = characteristic;
            return;
         }
      }
      info.characteristics.push_back(characteristic);
   }

private:
   void LoadPath(const std::string& path)
   {
      // Recursively search all directories for the "cache" directory, then
      // parse all mac address-named files in it.
      bool cache = path.size() > 5 && path.substr(path.size() - 5) == "cache";
      std::shared_ptr<DIR> d(opendir(path.c_str()), closedir);
      if (d)
      {
         struct dirent* de{};
         while ((de = readdir(d.get())))
         {
            if (de->d_name[0] == '.')
               continue;
            else if (de->d_type == DT_DIR)
               LoadPath(path + "/" + de->d_name);
            else if (cache && de->d_type == DT_REG)
            {
               // Is this filename a mac address?
               bool ismac = de->d_name[17] == 0; // Has to be 17 chars long.
               for (size_t i = 0; i < 17 && ismac; ++i)
               {
                  if (i % 3 == 2)
                     ismac = de->d_name[i] == ':';
                  else
                     ismac = isxdigit(de->d_name[i]);
               }
               if (ismac)
                  LoadDatabase(path + "/" + de->d_name, de->d_name);
            }
         }
      }
   }
   void LoadDatabase(const std::string& path, const std::string& mac)
   {
      std::vector<GattService> services;
      std::vector<GattCharacteristic> characteristics;

      // These are in glib's keyfile format. Kind of like ini files.
      auto split = [](const std::string& s, char c) {
         std::vector<std::string> ret;
         size_t pos = 0;
         size_t char_pos = s.find(c);
         while (char_pos != std::string::npos)
         {
            ret.push_back(s.substr(pos, char_pos - pos));
            pos = char_pos + 1;
            char_pos = s.find(c, pos);
         }
         ret.push_back(s.substr(pos));
         return ret;
      };
      std::ifstream in(path);
      std::string line;
      std::string section;
      while (std::getline(in, line))
      {
         // Strip trailing whitespace.
         while (!line.empty() && std::isspace(line.back()))
            line.pop_back();
         // Comment character only allowed on the front.
         if (line.empty() || line.front() == '#')
            continue;

         if (line.front() == '[' && line.back() == ']')
            section = line.substr(1, line.size() - 2);
         else if (section == "Attributes")
         {
            size_t eqpos = line.find('=');
            if (eqpos == std::string::npos)
               continue; // Broken File?
            std::string key = line.substr(0, eqpos);
            auto values = split(line.substr(eqpos+1), ':');

            // service:        2800:end:uuid
            // secondary svc:  2801:end:uuid
            // include:        2802:start:end:uuid
            // characteristic: 2803:value handle:properties:uuid
            // descriptor:     ext_props:uuid_str
            // descriptor:     uuid_str
            uint16_t handle = std::stoul(key, nullptr, 16);
            if (values.size() == 1)
            {
               // Descriptor for the most recently read characteristic.
               assert(!characteristics.empty());
               if (characteristics.empty())
                  continue;
               if (values.front() == GATT_CCC)
                  characteristics.back().ccc = handle;
               else if (values.front() == GATT_CHAR_DESCRIPTION)
                  characteristics.back().description = handle;
            }
            else if (values.front() == "2800" || values.front() == "2801")
            {
               assert(values.size() == 3);
               if (values.size() != 3)
                  continue;
               uint16_t end_handle = std::stoul(values[1], nullptr, 16);
               services.push_back(GattService{handle, end_handle, values[2]});
            }
            else if (values.front() == "2802")
               throw std::runtime_error("Please implement includes");
            else if (values.front() == "2803")
            {
               assert(values.size() == 4);
               if (values.size() != 4)
                  continue;
               uint16_t value_handle = std::stoul(values[1], nullptr, 16);
               uint8_t properties = std::stoul(values[2], nullptr, 16);
               characteristics.push_back(GattCharacteristic{handle, value_handle, 0, 0, properties, values[3]});
            }
            // else we don't care? or don't support?

         }
         // else We don't care about other sections.
      }

      std::string mac_lower;
      for (char c: mac) mac_lower.push_back(std::tolower(c));
      auto& info = m_info[mac_lower];
      info.services = std::move(services);
      info.characteristics = std::move(characteristics);
   }

private:
   struct CacheInfo
   {
      std::vector<GattService> services;
      std::vector<GattCharacteristic> characteristics;
   };
   std::map<std::string, CacheInfo> m_info;

   static std::string s_default_mac;
};

std::string BtDatabase::s_default_mac;

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
   std::function<void(uint16_t connection_handle, uint16_t handle, const std::vector<uint8_t>& bytes)> Notify;
   std::function<void(uint16_t connection_handle, uint16_t handle, uint8_t code)> FailedWrite;
   std::function<void(uint16_t connection_handle, uint16_t handle, uint8_t code)> FailedRead;
   std::function<void(uint16_t connection_handle, uint16_t status, const L2CapCreditConnection& info)> NewCreditConnection;
   std::function<void(uint16_t connection_handle, bool rx, const L2CapCreditConnection& info, const uint8_t* data, size_t len, size_t fragment_count)> Data;
   std::function<void(uint16_t connection_handle, uint8_t status, const std::string& mac, uint8_t reason)> Disconnect;


   struct HandleInfo
   {
      enum {INVALID, SERVICE, CHAR, CHAR_CCC, CHAR_DESCRIPTION, CHAR_VALUE, CHAR_DESCRIPTOR} type = INVALID;
      uint16_t handle = 0;
      GattService* service = nullptr;
      GattCharacteristic* characteristic = nullptr;
   };

   HandleInfo FindHandle(uint16_t connection, uint16_t handle)
   {
      auto& conn = m_connections[connection];

      if (conn.services.empty())
      {
         for (auto& s: m_db.Services(""))
            conn.services[s.handle] = s;
      }
      if (conn.characteristic.empty())
      {
         for (auto& c: m_db.Characteristics(""))
            conn.characteristic[c.handle] = c;
      }

      GattService* pservice = nullptr;

      // First, check if it is a service. If it isn't, which service does it
      // belong to?
      for (auto& s: conn.services)
      {
         if (s.first == handle) return HandleInfo{HandleInfo::SERVICE, handle, &s.second};

         if (s.second.handle < handle && handle <= s.second.end_handle)
         {
            pservice = &s.second;
            break;
         }
      }

      // Then check the characteristics.
      for (auto& c: conn.characteristic)
      {
         if (pservice && (pservice->handle > c.first || pservice->end_handle < c.first))
            continue;
         if (c.second.value == handle) return HandleInfo{HandleInfo::CHAR_VALUE, handle, pservice, &c.second};
         if (c.second.ccc == handle) return HandleInfo{HandleInfo::CHAR_CCC, handle, pservice, &c.second};
         if (c.second.description == handle) return HandleInfo{HandleInfo::CHAR_DESCRIPTION, handle, pservice, &c.second};
         if (c.first == handle) return HandleInfo{HandleInfo::CHAR, handle, pservice, &c.second};
         // TODO: handle other descriptor types.
      }



      // TODO: I'm not sure how, but bluez seems to infer the ccc
      //       descriptor handles for the service changed characteristic
      for (auto& c: conn.characteristic)
      {
         if (pservice && (pservice->handle > c.first || pservice->end_handle < c.first))
            continue;
         if ((c.second.value + 1 == handle) && c.second.uuid == "00002a05-0000-1000-8000-00805f9b34fb")
            return HandleInfo{HandleInfo::CHAR_CCC, handle, pservice, &c.second};
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
      case HandleInfo::CHAR:              return UuidStr(info.characteristic->uuid);
      case HandleInfo::CHAR_CCC:          return UuidStr(info.characteristic->uuid) + " ccc";
      case HandleInfo::CHAR_DESCRIPTION:  return UuidStr(info.characteristic->uuid) + " description";
      case HandleInfo::CHAR_VALUE:        return UuidStr(info.characteristic->uuid) + " value";
      case HandleInfo::CHAR_DESCRIPTOR:   return UuidStr(info.characteristic->uuid) + " descriptor";
      default: return "unknown";
      }
   }

   void AddCharacteristicGuess(uint16_t connection, uint16_t value_handle, const std::string& uuid)
   {
      // We don't know the characteristic handle so just use the value_handle
      // instead.
      auto& characteristic = m_connections[connection].characteristic[value_handle];
      characteristic.value = value_handle;
      characteristic.uuid = uuid;
      characteristic.guess = true;
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

   void DisconnectEvent(BtBufferStream pkt)
   {
      uint8_t length = pkt.U8();
      if (length > pkt.Size())
         pkt.Error("bad length");
      if (length != 4)
         pkt.Error("Unexpected length");
      uint8_t status = pkt.U8();
      uint16_t handle = pkt.U16();
      uint8_t reason = pkt.U8();
      auto& info = m_connections[handle];

      if (Disconnect)
         Disconnect(handle, status, info.mac, reason);

      m_connections.erase(handle);
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
         b.U8(); // peer address type
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
         
         // Load any cached services and characteristics.
         for (auto& s: m_db.Services(conn.mac))
            conn.services[s.handle] = s;
         for (auto& c: m_db.Characteristics(conn.mac))
            conn.characteristic[c.handle] = c;

         break;
      }
      case 0x0a:     // LE Enhanced Connection Complete
      {
         BtBufferStream b = pkt.Sub("LE Enhanced Connection Complete");
         uint8_t status = b.U8();
         uint16_t handle = b.U16();
         auto& conn = m_connections[handle] = ConnectionInfo{};
         conn.handle = handle;
         // TODO: if the address type is random, how do we recognize it if they
         //       disconnect and reconnect? Should we at least print a warning
         //       here?
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

         // Load any cached services and characteristics.
         for (auto& s: m_db.Services(conn.mac))
            conn.services[s.handle] = s;
         for (auto& c: m_db.Characteristics(conn.mac))
            conn.characteristic[c.handle] = c;
      
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
      case 0x0c:     // PHY update complete
      {
         BtBufferStream b = pkt.Sub("PHY update complete");
         uint8_t status = b.U8();
         uint16_t handle = b.U16();
         uint8_t tx = b.U8();
         uint8_t rx = b.U8();
         auto& conn = m_connections[handle];
         if (tx == 1)
            conn.phy = ConnectionInfo::PHY1M;
         else if (tx == 2)
            conn.phy = ConnectionInfo::PHY2M;
         else
            conn.phy = ConnectionInfo::PHY_UNKNOWN;
         break;
      }
      }
   }

   void EventPkt(BtBufferStream pkt)
   {
      switch (pkt.U8())
      {
      case 0x05:  DisconnectEvent(pkt.Sub("Disconnect Complete")); break;
      case 0x3e:  LeMetaEvent(pkt.Sub("LE Meta Event")); break;
      }
   }

   void FindInformationResponse(bool rx, uint16_t connection_handle, BtBufferStream b)
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
         uint16_t handle = binfo.U16();
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
            m_db.CacheCharacteristic(conn.mac, it->second);
            if (Descriptor)
               Descriptor(connection_handle, it->first, handle, uuid);

         }
      }
   }

   void ReadByTypeResponse(bool rx, uint16_t connection_handle, bool group, BtBufferStream b)
   {
      uint8_t response_size = b.U8();
      size_t attribute_count = response_size == 0 ? 0 : b.Size() / response_size;

      auto& conn = m_connections[connection_handle];
      auto& current_uuid = conn.pending[!rx].current_uuid;
      for (size_t i = 0; i < attribute_count; ++i)
      {
         // If response_size was invalid here, then attribute count would be zero.
         BtBufferStream rsp = b.Sub(group ? "Read By Type" : "Read By Group Type", response_size);
         uint16_t handle = rsp.U16();
         uint16_t end_handle = group ? rsp.U16() : 0;
         if (current_uuid == GATT_SERVICES)
         {
            std::string uuid = rsp.UUID();

            auto& service = conn.services[handle];
            service.handle = handle;
            service.end_handle = end_handle;
            service.uuid = uuid;
            m_db.CacheService(conn.mac, service);
            if (Service)
               Service(connection_handle, handle, end_handle, uuid);
            
         }
         else if (current_uuid == GATT_CHARACTERISTICS)
         {
            uint8_t properties = rsp.U8(); // bitmask (Extended, authenticated writes, indicate, notify, write, command, read, broadcast)
            uint16_t value_handle = rsp.U16();
            std::string uuid = rsp.UUID();

            auto& characteristic = conn.characteristic[handle];
            characteristic.handle = handle;
            characteristic.value = value_handle;
            characteristic.properties = properties;
            characteristic.uuid = uuid;
            m_db.CacheCharacteristic(conn.mac, characteristic);
            if (Characteristic)
               Characteristic(connection_handle, handle, value_handle, properties, uuid);
         }
      }
      current_uuid.clear();
   }

   void AttributeError(bool rx, uint16_t handle, BtBufferStream b)
   {
      auto& conn = m_connections[handle];
      uint8_t original_method = b.U8() & 0x3f;
      uint16_t handle_in_error = b.U16();
      uint8_t error_code = b.U8();
      switch (original_method)
      {
      case 0x11: // Read by group type request
         conn.pending[!rx].current_uuid.clear(); // error means we are done reading these.
         break;
      case 0x0a: // Read
         if (FailedRead)
            FailedRead(handle, conn.pending[!rx].handle, error_code);
         conn.pending[!rx].handle = 0;
         break;
      case 0x12: // Write
         assert(conn.pending[!rx].handle == handle_in_error); // remove later, just making sure I did this right;
         if (FailedWrite)
            FailedWrite(handle, conn.pending[!rx].handle, error_code);
         conn.pending[!rx].handle = 0;
         break;
      }
   }
   
   void WriteRequest(bool rx, uint16_t handle, BtBufferStream b)
   {
      uint16_t write_handle = b.U16();
      m_connections[handle].pending[rx].handle = write_handle;
      if (Write)
         Write(handle, write_handle, std::vector<uint8_t>(b.Data(), b.Data() + b.Size()));
   }

   void WriteResponse(bool rx, uint16_t handle, BtBufferStream b)
   {
      // b should be empty. This is a successful write.
   }

   void ReadRequest(bool rx, uint16_t handle, BtBufferStream b)
   {
      m_connections[handle].pending[rx].handle = b.U16();
   }

   void ReadResponse(bool rx, uint16_t handle, BtBufferStream b)
   {
      auto& conn = m_connections[handle];
      auto& pending_handle = conn.pending[!rx].handle;
      if (pending_handle)
      {
         if (Read)
            Read(handle, pending_handle, std::vector<uint8_t>(b.Data(), b.Data() + b.Size()));

         pending_handle = 0;
      }
   }

   void ValueNotification(bool rx, uint16_t connection, BtBufferStream b)
   {
      auto& conn = m_connections[connection];
      uint16_t value_handle = b.U16();
      if (Notify)
         Notify(connection, value_handle, std::vector<uint8_t>(b.Data(), b.Data() + b.Size()));
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
         AttributeError(rx, handle, b.Sub("Attribute Error Response", 4));
         break;
      case 0x02: // Exchange MTU Request. Fallthrough intentional
      case 0x03: // Exchange MTU Response
         (rx ? conn.peripheral_mtu : conn.central_mtu) = b.Sub("Exchange MTU").U16();
         break;
      case 0x05: // Find Information Response
         FindInformationResponse(rx, handle, b.Sub("Find Information Response"));
         break;
      case 0x08: // Read by type request. Fallthrough intentional
      case 0x10: // Read by group type request
      {
         BtBufferStream bg = b.Sub("Read By Type Request");
         bg.Skip(4); // Starting/ending handle.
         conn.pending[rx].current_uuid = bg.UUID();
         break;
      }
      case 0x09: // Read By Type Response
         ReadByTypeResponse(rx, handle, false, b.Sub("Read By Type Response"));
         break;
      case 0x11: // Read By Group Type Response
         ReadByTypeResponse(rx, handle, true, b.Sub("Read By Group Type Response"));
         break;
      case 0x12: // Write Request
         WriteRequest(rx, handle, b.Sub("Write Request"));
         break;
      case 0x13: // Write Response
         WriteResponse(rx, handle, b.Sub("Write Response"));
         break;
      case 0x0a: // Read Request
         ReadRequest(rx, handle, b.Sub("Read Request"));
         break;
      case 0x0b: // Read Response
         ReadResponse(rx, handle, b.Sub("Read Response"));
         break;
      case 0x1b: // Handle Value Notification
         ValueNotification(rx, handle, b.Sub("Value Notification"));
         break;
      }
   }

   void ParseLECreditConnectionRequest(bool rx, uint16_t handle, uint8_t id, BtBufferStream b)
   {
      uint16_t psm = b.U16();
      uint16_t cid = b.U16();
      uint16_t mtu = b.U16();
      uint16_t mps = b.U16();
      uint16_t credits = b.U16();
      auto& conn = m_connections[handle];
      const uint16_t zero = 0;
      conn.pending[rx].ecred[id] = L2CapCreditConnection{
         .outgoing = !rx,
         .cids = StreamCids{.rx = rx ? cid : zero, .tx = rx ? zero : cid, },
         .psm = psm,
         .mtu = mtu,
         .mps = mps,
         .tx_credits = rx ? zero : credits,
      };
   }
   void ParseLECreditConnectionResponse(bool rx, uint16_t handle, uint8_t id, BtBufferStream b)
   {
      uint16_t cid = b.U16();
      uint16_t mtu = b.U16();
      uint16_t mps = b.U16();
      uint16_t credits = b.U16();
      uint16_t result = b.U16();
      auto& conn = m_connections[handle];
      const uint16_t zero = 0;
      auto pending = conn.pending[!rx].ecred[id];
      if (rx)
      {
         pending.outgoing = true;
         pending.cids.rx = cid;
         pending.tx_credits = credits;
      }
      else
      {
         pending.outgoing = false;
         pending.cids.tx = cid;
      }
      if (pending.mps)
         pending.mps = std::min(mps, pending.mps);
      else
         pending.mps = mps;
      if (pending.mtu)
         pending.mtu = std::min(mtu, pending.mtu);
      else
         pending.mtu = mtu;
      
      if (result == 0)
         conn.ecred[pending.cids] = pending;

      if (NewCreditConnection)
         NewCreditConnection(handle, result, pending);

      conn.pending[!rx].ecred.erase(id);
   }
   void ParseLECreditConnectionAddCredit(bool rx, uint16_t handle, uint8_t id, BtBufferStream b)
   {
      uint16_t cid = b.U16();
      uint16_t credits = b.U16();
      auto& conn = m_connections[handle];
      for (auto& kv: conn.ecred)
      {
         // This seems odd to me, but the credit is sent *from* the relevant
         // cid, not to the cid of the other side of the connection.
         if (rx && kv.first.tx == cid)
         {
            kv.second.tx_credits += credits;
            // std::cout << "+Credits: " << Hex(handle) << " " << Hex(kv.second.tx_cid) << " " << kv.second.tx_credits << '\n';
            break;
         }
      }
   }

   void ParseL2CapSignal(bool rx, uint16_t handle, BtBufferStream b)
   {
      uint8_t code = b.U8();
      uint8_t id = b.U8();
      uint16_t length = b.U16();
      switch (code)
      {
      case 0x14: ParseLECreditConnectionRequest(rx, handle, id, b.Sub("LE Credit Based Connection Request", length)); break;
      case 0x15: ParseLECreditConnectionResponse(rx, handle, id, b.Sub("LE Credit Based Connection Response", length)); break;
      case 0x16: ParseLECreditConnectionAddCredit(rx, handle, id, b.Sub("LE Flow Control Credit")); break;
      }
   }

   void ParseLECreditConnectionData(bool rx, uint16_t handle, L2CapCreditConnection& conn, BtBufferStream b)
   {
      if (!rx)
      {
         // If we parsed the header, then tx_credits should never be zero. If
         // we inferred the stream, then we don't know how many credits there
         // are, so in theory this could go negative.
         // assert(conn.tx_credits > 0);
         --conn.tx_credits;
      }
      uint16_t sdu_length = b.U16();
      if (sdu_length != b.Size())
         b.Error("Invalid SDU length");
      auto& base_conn = m_connections[handle];
      if (Data)
         Data(handle, rx, conn, b.Data(), b.Size(), base_conn.pending[rx].fragment_count);
   }

   void ParseL2CapDynamicData(bool rx, uint16_t handle, uint16_t cid, BtBufferStream b)
   {
      auto& conn = m_connections[handle];
      // Is this a known LE credit based connection?
      for (auto& kv: conn.ecred)
      {
         if ((rx ? kv.first.rx : kv.first.tx) == cid)
         {
            ParseLECreditConnectionData(rx, handle, kv.second, b.Sub("LE Credit Based Connection Payload"));
            return;
         }
      }

      // We must have missed the initialization of this stream. Try to guess.
      uint16_t sdu_length = b.U16();
      if (sdu_length == 161 && b.Size() == 161 && rx == false)
      {
         // This is the right size for a g.722 audio frame.
         auto& stream = conn.ecred[StreamCids{.tx = cid}] = {
            .outgoing = true,
            .cids = {
               .tx = cid
            },
         };
         if (Data)
            Data(handle, rx, stream, b.Data(), b.Size(), conn.pending[rx].fragment_count);
      }
   }
   
   void AclPkt(bool rx, BtBufferStream pkt)
   {
      uint16_t header = pkt.U16();
      uint16_t handle = header & 0x0fff;
      uint16_t length = pkt.U16();
      uint16_t boundary = (header >> 12) & 0x3;
      uint16_t broadcast = (header >> 14) & 0x3;
      
      auto& conn = m_connections[handle];
      if (conn.type == ConnectionInfo::CONN_UNKNOWN)
      {
         // This was default initialized, which means we missed the connection
         // negotiation. For now, just assume L2CAP
         conn.type = ConnectionInfo::L2CAP;
      }
      
      if (conn.type == ConnectionInfo::L2CAP)
      {
         BtBufferStream b = pkt.Sub("L2CAP", length);
         std::vector<uint8_t> fragment_data;
         // length is already correct.
         if (conn.pending[rx].fragment.empty())
         {
            if (b.Size() < 4)
               throw std::runtime_error("Truncated TX L2CAP packet header");
            // Peek at packet length in stream
            conn.pending[rx].expected_fragment_size = BtSwap(*(uint16_t*)(b.Data() + 0)) + 4;
            conn.pending[rx].fragment_count = 1;
            if (conn.pending[rx].expected_fragment_size > b.Size())
            {
               conn.pending[rx].fragment.assign(b.Data(), b.Data() + length);
               return; // Need to wait for more data.
            }
         }
         else
         {
            ++conn.pending[rx].fragment_count;
            conn.pending[rx].fragment.insert(conn.pending[rx].fragment.end(), b.Data(), b.Data() + b.Size());
            if (conn.pending[rx].fragment.size() < conn.pending[rx].expected_fragment_size)
               return; // Need to wait for more data.
            fragment_data.swap(conn.pending[rx].fragment);
            b = BtBufferStream("L2CAP", fragment_data.data(), fragment_data.size());
            length = conn.pending[rx].expected_fragment_size;
         }
         uint16_t l2cap_length = b.U16();
         uint16_t cid = b.U16();
         assert(b.Size() == l2cap_length); // In case we screwed up the reassembly logic.

         switch (cid)
         {
         case 0x0004: // Attribute Protocol
            ParseAttribute(rx, handle, b.Sub("Attribute Protocol"));
            break;
         case 0x0005: // LE L2CAP Signaling Channel
            ParseL2CapSignal(rx, handle, b.Sub("LE L2CAP Signaling Channel"));
            break;
         default:
            ParseL2CapDynamicData(rx, handle, cid, b.Sub("L2CAP Dynamic Channel"));
            break;
         }
      }
   }

private:
   BtDatabase m_db;

   struct ConnectionInfo
   {
      uint16_t handle = 0;
      enum {CONN_UNKNOWN, L2CAP} type = CONN_UNKNOWN;
      std::string mac;
      enum {PHY_UNKNOWN, PHY1M, PHY2M} phy{};
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
      std::map<StreamCids, L2CapCreditConnection> ecred;

      // These variables represent temporary state while parsing.
      struct {
         std::string current_uuid;        // UUID for group read requests
         std::vector<uint8_t> fragment;   // Reassembled l2cap fragment.
         size_t fragment_count = 0;
         uint16_t expected_fragment_size = 0;
         uint16_t handle = 0;
         std::map<uint8_t, L2CapCreditConnection> ecred;
      } pending[2];
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
   bool extract_audio = false;
   for (int i = 1; i < argc; ++i)
   {
      std::string k = argv[i];
      std::string v = i + 1 < argc ? argv[i + 1] : "";
      if (k == "--mac" && i + 1 < argc)
      {
         BtDatabase::SetDefaultMac(v);
         ++i;
      }
      else if (k.size() > 1 && k.front() != '-' || k == "-")
      {
         if (k != "-")
            snoop_filename = k;
      }
      else if (k == "--extract")
      {
         extract_audio = true;
      }
      else
      {
         std::cout << "Usage: " << argv[0] << " [opts] capture.snoop\n"
                   << "This tool will analyze a bluetooth capture to check for asha protocol usage, and\n"
                   << "will attempt to find common problems.\n"
                   << "Options:\n"
                   << "   --mac <mac_address>  Mac address to assume for remote device. This is used to\n"
                   << "                        look up characteristics that may have been discovered\n"
                   << "                        during a previous connection or dump file if the pairing\n"
                   << "                        is not part of the snoop file.\n"
                   << "   --extract            Extract audio into <cid>_<connid>.g722 files\n"
                   << "\n"
                   << "Parsed characteristics are cached in ~/.local/share/snoop_analyze/cache/ to be\n"
                   << "used in the future. These characteristics can also be manually copied by the\n"
                   << "user from the bluez cache at /var/lib/bluetooth/<hci-mac>/cache/\n"
                   << "\n"
                   << "Stream analysis output will look like this:\n"
                   << "     183 << 0e02 right      0     7(-7) 161 bytes   0 seq    +0.000 ms\n"
                   << "     184 << 0e01 left       7     7( 0) 161 bytes   0 seq    +0.000 ms\n"
                   << "     187 << 0e02 right      7     6( 1) 161 bytes   1 seq    +0.326 ms\n"
                   << "     188 << 0e01 left       6     6( 0) 161 bytes   1 seq    +0.304 ms\n"
                   << "   The columns are:\n"
                   << "      1. Packet number\n"
                   << "      2. << for transmit, >> for receive\n"
                   << "      3. Device id\n"
                   << "      4. Human readable device label (\"left\" or \"right\")\n"
                   << "      5. Current left credits\n"
                   << "      6. Current right credits\n"
                   << "      7. Delta between left or right (this should stay less than 4)\n"
                   << "      8. Size of data frame plus sequence header (should be 161 bytes)\n"
                   << "      9. One byte sequence number\n"
                   << "     10. Delta between the audio offset from the beginning of the stream\n"
            ;

         return 1;
      }
   }
   
   std::ifstream infile;
   if (!snoop_filename.empty())
   {
      infile.open(snoop_filename, std::ios::binary);
      if (!infile)
      {
         std::cout << "Unable to open file\n";
         return 1;
      }
   }

   BtSnoopFile snoop(snoop_filename.empty() ? std::cin : infile);

   struct DeviceInfo
   {
      uint16_t psm = 0;
      std::string description;
      enum {UNKNOWN, MONO, LEFT, RIGHT} side = UNKNOWN;
      uint64_t hisync;
   };
   std::map<uint16_t, DeviceInfo> device_info;

   struct StreamInfo
   {
      uint16_t device = 0;
      StreamCids cids{};
      DeviceInfo* dinfo = nullptr;
      StreamInfo* other = nullptr;

      int64_t credits = 0;
      uint8_t seq = 0;

      uint64_t expected_stamp = 0;

      std::unique_ptr<std::ofstream> outfile;
   };
   std::map<std::pair<uint16_t, StreamCids>, StreamInfo> asha_streams;
   uint64_t frame_idx = 0;
   uint64_t stamp = 0;

   BtParser parser;
   parser.NoteCallback = [](const std::string& s) {
      std::cout << "System Note: " << s.c_str() << '\n';
   };
   parser.ConnectionCallback = [](uint16_t connection, uint8_t status, const std::string& mac, uint16_t interval, uint16_t latency, uint16_t timeout) {
      std::cout << "New Connection: " << Hex(connection) << " " << mac << " params(" << interval << ", " << latency << ", " << timeout << ")\n";
   };
   parser.Disconnect = [&](uint16_t connection, uint8_t status, const std::string& mac, uint8_t reason)
   {
      std::cout << "Disconnect:     " << Hex(connection) << " " << mac << " " << Hex(reason) << '\n';
      for (auto it = asha_streams.begin(); it != asha_streams.end();)
      {
         if (it->first.first == connection)
            it = asha_streams.erase(it);
         else
            ++it;
      }
      device_info.erase(connection);
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
   std::map<uint16_t, bool> next_read_is_psm;
   parser.Read = [&](uint16_t connection, uint16_t handle, const std::vector<uint8_t>& bytes) {
      std::cout << "Read:           " << Hex(connection) << " " << Hex(handle) << " " << parser.HandleDescription(connection, handle) << " " << ToString(bytes) << '\n';
      auto info = parser.FindHandle(connection, handle);
      if (!info.characteristic)
      {
         // We don't know what this is, probably because the snoop file is
         // missing the discovery packets. Use some heuristics to guess.
         if (bytes.size() == 2 && next_read_is_psm[connection])
         {
            std::cout << "   Guessing that this is ASHA_LE_PSM_OUT\n";
            parser.AddCharacteristicGuess(connection, handle, ASHA_LE_PSM_OUT);
         }
         next_read_is_psm[connection] = false; // stop looking for it.
         if (bytes.size() == 17 && bytes[0] == 0x01 && bytes[10] == 0x01 && bytes[15] == 0x02)
         {
            //    uint8_t version;        // must be 0x01
            //    uint8_t capabilities;   // Mask. 0x01 is right side, 0x02 is binaural, 0x04 is CSIS support
            //    uint64_t hi_sync_id;    // Same for paired devices
            //    uint8_t feature_map;    // 0x1 is audio streaming supported.
            //    uint16_t render_delay;  // ms delay before audio is rendered. For synchronization purposes.
            //    uint16_t reserved;      // preparation delay
            //    uint16_t codecs;        // 0x2 is g.722. No others are defined.
            std::cout << "   Guessing that this is ASHA_READ_ONLY_PROPERTIES\n";
            parser.AddCharacteristicGuess(connection, handle, ASHA_READ_ONLY_PROPERTIES);
            // The next 2 byte value we read should be the psm
            next_read_is_psm[connection] = true;
         }

         // Retry the search with the guesses in.
         info = parser.FindHandle(connection, handle);
      }
      else
      {
         // If we know what the characterisic is, then don't guess a psm.
         next_read_is_psm[connection] = false;
      }

      if (info.characteristic)
      {
         if (info.characteristic->uuid == ASHA_LE_PSM_OUT && bytes.size() == 2)
         {
            device_info[connection].psm = bytes[0] | (bytes[1] << 8);
            std::cout << "   PSM: " << device_info[connection].psm << '\n';
         }
         else if (info.characteristic->uuid == DEVICE_NAME)
         {
            device_info[connection].description = std::string(bytes.begin(), bytes.end()).c_str();
            std::cout << "   Name: " << device_info[connection].description << '\n';
         }
         else if (info.characteristic->uuid == ASHA_READ_ONLY_PROPERTIES && bytes.size() == 17)
         {
            BtBufferStream props("ASHA_READ_ONLY_PROPERTIES", bytes.data(), bytes.size());
            uint8_t version = props.U8();
            uint8_t caps = props.U8();
            uint64_t hisync = props.U64();
            uint8_t features = props.U8();
            uint8_t delay = props.U16();
            props.Skip(2); // Reserved
            uint16_t codecs = props.U16();
            
            if (version == 1)
            {

               if ((caps & 2) == 0)
                  device_info[connection].side = DeviceInfo::MONO;
               else
                  device_info[connection].side = (caps & 1 ) ? DeviceInfo::RIGHT : DeviceInfo::LEFT;

               device_info[connection].hisync = hisync;
               std::cout << "   Props: " << ((caps & 2) ? "stereo " : "mono ") << ((caps & 1) ? "right " : "left ") << Hex(hisync) << '\n';
            }
         }
      }
   };
   parser.Notify = [&](uint16_t connection, uint16_t handle, const std::vector<uint8_t>& bytes) {
      std::cout << "Notify:         " << Hex(connection) << " " << Hex(handle) << " " << parser.HandleDescription(connection, handle) << " " << Hex(bytes);
      auto info = parser.FindHandle(connection, handle);
      if (info.characteristic)
      {
         if (info.characteristic->uuid == ASHA_AUDIO_STATUS && bytes.size() == 1)
         {
            switch((int8_t)bytes[0])
            {
            case 0: std::cout << " [Success]"; break;
            case -1: std::cout << " [Unknown Command]"; break;
            case -2: std::cout << " [Illegal Parameters]"; break;
            }
         }
      }
      std::cout << '\n';
   };
   parser.FailedWrite = [&](uint16_t connection, uint16_t handle, uint8_t code) {
      std::cout << "Failed Write:   " << Hex(connection) << " " << Hex(handle) << " " << parser.HandleDescription(connection, handle) << " " << code << '\n';
   };
   parser.FailedRead = [&](uint16_t connection, uint16_t handle, uint8_t code) {
      std::cout << "Failed Read:    " << Hex(connection) << " " << Hex(handle) << " " << parser.HandleDescription(connection, handle) << " " << code << '\n';
   };
   parser.NewCreditConnection = [&](uint16_t connection, uint16_t status, const L2CapCreditConnection& info) {
      if (status)
         std::cout << "Failed CoC:     " << Hex(connection) << " PSM: " << Hex(info.psm) << " Status: " << status << '\n';
      else
      {
         auto& dinfo = device_info[connection];
         if (dinfo.psm == info.psm && info.outgoing)
         {
            switch (dinfo.side)
            {
            case DeviceInfo::LEFT:    std::cout << "Left Stream:    "; break;
            case DeviceInfo::RIGHT:   std::cout << "Right Stream:   "; break;
            case DeviceInfo::MONO:    std::cout << "Mono Stream:    "; break;
            case DeviceInfo::UNKNOWN: std::cout << "Unknown Stream: "; break;
            }
            
            auto& sinfo = asha_streams[std::make_pair(connection, info.cids)];
            sinfo.cids = info.cids;
            sinfo.device = connection;
            sinfo.dinfo = &dinfo;

            for (auto& oinfo: asha_streams)
            {
               if (oinfo.second.device != connection && oinfo.second.dinfo && oinfo.second.dinfo->hisync == dinfo.hisync)
               {
                  sinfo.other = &oinfo.second;
                  oinfo.second.other = &sinfo;
                  break;
               }
            }
         }
         else
            std::cout << "New CoC:        ";
      
         std::cout << Hex(connection) << " PSM: " << Hex(info.psm) << " MTU: " << info.mtu << " MPS: " << info.mps << " Credits: " << info.tx_credits << '\n';
      }
   };
   parser.Data = [&](uint16_t connection, bool rx, const L2CapCreditConnection& info, const uint8_t* data, size_t len, size_t fragment_count)
   {
      // std::cout << "Data:        " << (rx ? ">> " : "<< ") << Hex(connection) << " " << info.tx_credits << " " << len << "bytes\n";
      auto itinfo = asha_streams.find(std::make_pair(connection, info.cids));
      if (itinfo == asha_streams.end())
      {
         if (len == 161 && rx == false)
         {
            // Its the right size, its probably an audio packet.
            std::cout << "   Guessing that connection " << connection << " stream " << info.cids.tx << " is g.722 audio\n";
            auto results = asha_streams.emplace(std::make_pair(connection, info.cids), StreamInfo{
               .cids = info.cids
            });
            itinfo = results.first;

            // Is there another stream from a different device that we guessed
            // at? Its probably meant to be paired with this one.
            for (auto& stream: asha_streams)
            {
               if (stream.first.first != connection && stream.first.second.rx == 0 && stream.second.other == nullptr)
               {
                  itinfo->second.other = &stream.second;
                  stream.second.other = &itinfo->second;

                  std::cout << "   Guessing that " << Hex(itinfo->first.first) << ':' << Hex(itinfo->first.second.tx)
                            << " and " << Hex(stream.first.first) << ':' << Hex(stream.first.second.tx)
                            << " are a stereo pair.\n";
                  break;
               }
            }
         }
      }

      if (itinfo != asha_streams.end())
      {
         if (len > 1 && extract_audio)
         {
            if (!itinfo->second.outfile)
            {
               std::string filename = Hex(connection) + "_" + Hex(info.cids.tx) + ".g722";
               itinfo->second.outfile = std::make_unique<std::ofstream>(filename, std::ios::binary);
            }
            // First byte is sequence number. Skip it.
            itinfo->second.outfile->write((const char*)data + 1, len - 1);
         }

         itinfo->second.credits = info.tx_credits;
         std::cout << std::setw(8) << frame_idx << (rx ? " >> " : " << ") << Hex(connection);
         if (itinfo->second.other)
         {
            if (itinfo->second.dinfo && itinfo->second.other->dinfo)
            {
               auto& left = itinfo->second.dinfo->side == DeviceInfo::LEFT ? itinfo->second : *itinfo->second.other;
               auto& right = itinfo->second.dinfo->side == DeviceInfo::LEFT ? *itinfo->second.other : itinfo->second;
               const char* side = itinfo->second.dinfo->side == DeviceInfo::LEFT ? " left  " : " right ";
               std::cout << side << std::setw(6) << left.credits
                                 << std::setw(6) << right.credits
                                 << '(' << std::setw(2) << left.credits - right.credits << ") "
                                 << len << " bytes";
            }
            else
            {
               // We don't know which is left and which is right (we guessed
               // that this is an audio stream)
               const StreamInfo* dev1 = itinfo->second.other;
               const StreamInfo* dev2 = &itinfo->second;
               const char* label = " dev2 ";
               if (&itinfo->second < itinfo->second.other)
               {
                  dev1 = &itinfo->second;
                  dev2 = itinfo->second.other;
                  label = " dev1 ";
               }
               std::cout << label << std::setw(6) << dev1->credits
                                 << std::setw(6) << dev2->credits
                                 << '(' << std::setw(2) << dev1->credits - dev2->credits << ") "
                                 << len << " bytes";
            }
         }
         else
         {
            std::cout << " mono " << info.tx_credits << " " << len << " bytes";
         }
         if (fragment_count > 1)
            std::cout << " " << fragment_count << " fragments";
         if (len > 1)
         {
            uint8_t seq = data[0];
            std::cout << " " << std::setw(3) << (unsigned)data[0] << " seq";
            if (seq != itinfo->second.seq + 1 && seq != 0)
               std::cout << " (Missing " << (unsigned)(seq - itinfo->second.seq + 1) << " frames)";
            itinfo->second.seq = seq;
         }
         if (itinfo->second.expected_stamp == 0)
            itinfo->second.expected_stamp = stamp;
         double dt = (int64_t)(stamp - itinfo->second.expected_stamp) / 1000.0;
         auto oldflags = std::cout.flags();
         std::cout << ' ' << std::fixed << std::setprecision(3) << std::showpos << std::setw(9) << dt << " ms";
         std::cout.flags(oldflags);
         itinfo->second.expected_stamp += 20000;

         std::cout << '\n';
      }
   };

   while (true)
   {
      auto& packet = snoop.Next();
      if (!packet)
         break;
      ++frame_idx;
      stamp = packet.stamp;
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