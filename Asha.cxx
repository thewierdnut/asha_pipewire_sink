#include "Asha.hh"

#include <chrono>
#include <dbus/dbus.h>
#include <unistd.h>
#include <sys/socket.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>

#include <cassert>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>


// This implements the google-specified "Hearing Aid Audio Support Using
// Bluetooth LE" protocol using the ASHA Gatt service, as specified here:
// https://source.android.com/docs/core/connect/bluetooth/asha

namespace
{
   const std::string ASHA_GATT_SERVICE_UUID    = "0000fdf0-0000-1000-8000-00805f9b34fb";
   const std::string ASHA_READ_ONLY_PROPERTIES = "6333651e-c481-4a3e-9169-7c902aad37bb";
   const std::string ASHA_AUDIO_CONTROL_POINT  = "f0d4de7e-4a88-476c-9d9f-1937b0996cc0";
   const std::string ASHA_AUDIO_STATUS         = "38663f1a-e711-4cac-b641-326b56404837";
   const std::string ASHA_VOLUME               = "00e4ca9e-ab14-41e4-8823-f9e70c7e91df";
   const std::string ASHA_LE_PSM_OUT           = "2d410339-82b6-42aa-b34e-e2e01df8cc1a";

   // Client characteristic configuration (needed for status notifications).
   const std::string DESCRIPTOR_CCC            = "00002902-0000-1000-8000-00805f9b34fb";

   // proprietary volume control?
   //    service 7d74f4bd-c74a-4431-862c-cce884371592
   //    characteristic 4603580D-3C15-4FEC-93BE-B86B243ADA64
   //       Write value from 0x00 to 0xff

   // "0000180d-0000-1000-8000-00805f9b34fb" heart rate?

   struct AshaProperties
   {
      uint8_t version;        // must be 0x01
      uint8_t capabilities;   // Mask. 0x01 is right side, 0x02 is binaural, 0x04 is CSIS support
      uint64_t hi_sync_id;    // Same for paired devices
      uint8_t feature_map;    // 0x1 is audio streaming supported.
      uint16_t render_delay;  // ms delay before audio is rendered. For synchronization purposes.
      uint16_t reserved;
      uint16_t codecs;        // 0x2 is g.722. No others are defined.
   } __attribute__((packed));

   constexpr uint8_t CAPABILITY_RIGHT_SIDE = 0x01;
   constexpr uint8_t CAPABILITY_BINAURAL = 0x02;
   constexpr uint8_t CAPABILITY_CSIS = 0x04;
   constexpr uint8_t FEATURE_STREAMING = 0x01;
   constexpr uint16_t CODEC_G722_16KHZ = 0x02;
   constexpr uint16_t CODEC_G722_24KHZ = 0x04;


   struct BasicVariant
   {
      char type = DBUS_TYPE_INVALID;
      DBusBasicValue v;
      std::string s;

      BasicVariant() = default;
      BasicVariant(int type, const std::string& s):type(type), s(s) {}

      bool operator<(const BasicVariant& o) const
      {
         if (o.type != type)
            throw std::runtime_error("Trying to compare unrelated variant types");
         switch(o.type)
         {
         case DBUS_TYPE_BYTE:    return v.byt < o.v.byt;
         case DBUS_TYPE_BOOLEAN: return v.bool_val < o.v.bool_val;
         case DBUS_TYPE_INT16:   return v.i16 < o.v.i16;
         case DBUS_TYPE_UINT16:  return v.u16 < o.v.u16;
         case DBUS_TYPE_INT32:   return v.i32 < o.v.i32;
         case DBUS_TYPE_UNIX_FD: return v.i32 < o.v.i32;
         case DBUS_TYPE_UINT32:  return v.u32 < o.v.u32;
         case DBUS_TYPE_INT64:   return v.i64 < o.v.i64;
         case DBUS_TYPE_UINT64:  return v.u64 < o.v.u64;
         case DBUS_TYPE_DOUBLE:  return v.i16 < o.v.i16;
         case DBUS_TYPE_OBJECT_PATH:    // fallthrough intentional
         case DBUS_TYPE_SIGNATURE:
         case DBUS_TYPE_STRING:  return s < o.s;
         default: throw std::runtime_error("Trying to compare a non-basic dbus type.");
         }
      }
      bool operator==(const BasicVariant& o) const
      {
         if (o.type != type)
            return false;
         switch(o.type)
         {
         case DBUS_TYPE_BYTE:    return v.byt == o.v.byt;
         case DBUS_TYPE_BOOLEAN: return v.bool_val == o.v.bool_val;
         case DBUS_TYPE_INT16:   return v.i16 == o.v.i16;
         case DBUS_TYPE_UINT16:  return v.u16 == o.v.u16;
         case DBUS_TYPE_INT32:   return v.i32 == o.v.i32;
         case DBUS_TYPE_UINT32:  return v.u32 == o.v.u32;
         case DBUS_TYPE_INT64:   return v.i64 == o.v.i64;
         case DBUS_TYPE_UINT64:  return v.u64 == o.v.u64;
         case DBUS_TYPE_DOUBLE:  return v.i16 == o.v.i16;
         case DBUS_TYPE_STRING:  return s < o.s;
         default: return false;
         }
      }

      std::string ToString() const
      {
         switch(type)
         {
         case DBUS_TYPE_BYTE:          return std::to_string((uint32_t)v.byt);
         case DBUS_TYPE_BOOLEAN:       return v.bool_val ? "true" : "false";
         case DBUS_TYPE_INT16:         return std::to_string(v.i16);
         case DBUS_TYPE_UINT16:        return std::to_string(v.u16);
         case DBUS_TYPE_INT32:         return std::to_string(v.i32);
         case DBUS_TYPE_UINT32:        return std::to_string(v.u32);
         case DBUS_TYPE_INT64:         return std::to_string(v.i64);
         case DBUS_TYPE_UINT64:        return std::to_string(v.u64);
         case DBUS_TYPE_DOUBLE:        return std::to_string(v.dbl);
         case DBUS_TYPE_STRING:        return v.str ? ("\"" + std::string(v.str) + "\"") : "null";
         case DBUS_TYPE_OBJECT_PATH:   return v.str ? ("\"" + std::string(v.str) + "\"") : "null";
         case DBUS_TYPE_SIGNATURE:     return v.str ? ("\"" + std::string(v.str) + "\"") : "null";
         case DBUS_TYPE_UNIX_FD:       return std::to_string(v.fd);
         }
         return "null";
      }

      std::string Signature() const
      {
         return std::string(1, (char)type);
      }
   };

   struct DbusObject
   {
      BasicVariant value;
      std::vector<DbusObject> array;
      std::multimap<BasicVariant, DbusObject> dict;

      std::string Signature() const
      {
         std::string ret;
         switch (value.type)
         {
         case DBUS_TYPE_ARRAY:
            if (!array.empty())
               ret = "a" + array.front().Signature();
            else if (!dict.empty())
            {
               ret = "a{";
               ret += dict.begin()->first.Signature();
               ret += dict.begin()->second.Signature();
               ret += "}";
            }
            else
            {
               // We didn't actually pack anything. Lets default to an dict of
               // variants.
               ret = "a{sv}";
            }
            break;
         case DBUS_TYPE_STRUCT:  // Fixed number of children with different types, like a tuple.
            ret = "(";
            for (auto& a: array)
            {
               ret += a.Signature();
            }
            ret += ")";
            break;
         case DBUS_TYPE_DICT_ENTRY: // A pair of types, the first one has to be trivial.
            // We don't typically use this, but if we do, it will be an array of two items.
            if (array.size() == 2)
            {
               ret = "{";
               ret += array[0].Signature();
               ret += array[1].Signature();
               ret += "}";
            }
            break;
         default:
            ret = value.Signature();
         }
         return ret;
      }

      std::string ToString() const { return ToString(""); }

      std::string ToString(const std::string& padding) const
      {
         if (array.empty() && dict.empty() && value.type == DBUS_TYPE_INVALID)
            return "[]";
         if (array.empty() && dict.empty())
            return value.ToString();

         // Its bad form, but I know nothing preventing us from interleaving
         // dictionary and array entries.
         std::stringstream ss;
         if (!array.empty())
         {
            ss << "[";
            bool first = true;
            for (auto& o: array)
            {
               ss << (first ? "\n" : ",\n") << padding << "  " << o.ToString(padding + "  ");
               first = false;
            }
            ss << "\n" << padding << "]";
         }
         if (!dict.empty())
         {
            ss << "{";
            bool first = true;
            for (auto& kv: dict)
            {
               ss << (first ? "\n" : ",\n") << padding << "  " << kv.first.ToString() << ": " << kv.second.ToString(padding + "  ");
               first = false;
            }
            ss << "\n" << padding << "}";
         }
         return ss.str();
      }

      const DbusObject& Child(const std::string& key) const
      {
         auto it = dict.find(BasicVariant(DBUS_TYPE_STRING, key));
         if (it != dict.end())
            return it->second;
         static const DbusObject empty{};
         return empty;
      }
      bool Bool() const { return value.type == DBUS_TYPE_BOOLEAN && value.v.bool_val; }
      bool Null() const { return value.type == DBUS_TYPE_INVALID; }
      const std::string& String() const { return value.s; }

      void SetVariant(uint16_t u)
      {
         value.type = DBUS_TYPE_VARIANT;
         array.resize(1);
         array.back().value.type = DBUS_TYPE_UINT16;
         array.back().value.v.u16 = u;
      }
      void SetVariant(void* bytes, size_t size)
      {
         value.type = DBUS_TYPE_VARIANT;
         array.resize(1);
         array.back().SetBytes(bytes, size);
      }
      void SetVariant(const char* s)
      {
         value.type = DBUS_TYPE_VARIANT;
         array.resize(1);
         array.back().value.type = DBUS_TYPE_STRING;
         array.back().value.v.str = const_cast<char*>(s);
      }
      void SetVariant(const DbusObject& o)
      {
         value.type = DBUS_TYPE_VARIANT;
         array.push_back(o);
      }
      template<typename T>
      void AddVariant(const std::string& key, const T& v)
      {
         value.type = DBUS_TYPE_ARRAY;
         BasicVariant vk;
         vk.type = DBUS_TYPE_STRING;
         vk.s = key;
         dict.emplace(vk, DbusObject())->second.SetVariant(v);
      }
      template<typename T>
      void AddVariant(const T& v)
      {
         value.type = DBUS_TYPE_ARRAY;
         array.resize(array.size() + 1);
         array.back().SetVariant(v);
      }
      void SetBytes(const void* bytes, size_t size)
      {
         value.type = DBUS_TYPE_ARRAY;
         array.resize(size);
         for (size_t i = 0; i < size; ++i)
         {
            array[i].value.type = DBUS_TYPE_BYTE;
            array[i].value.v.byt = ((uint8_t*)bytes)[i];
         }
      }

      void AddBytes(const void* bytes, size_t size)
      {
         value.type = DBUS_TYPE_ARRAY;
         array.resize(array.size() + 1);
         // Pack array of bytes as a variant.
         // array.back().SetVariant(bytes, size);
         // Pack native array of bytes.
         array.back().SetBytes(bytes, size);
         if (array.front().Signature() != array.back().Signature())
            value.type = DBUS_TYPE_STRUCT;
      }
      void AddObject(const DbusObject& o)
      {
         value.type = DBUS_TYPE_ARRAY;
         array.resize(array.size() + 1);
         array.back() = o;
         if (array.front().Signature() != array.back().Signature())
            value.type = DBUS_TYPE_STRUCT;
      }
   };

   void UnpackDBusObject(DbusObject& obj, DBusMessageIter& iter);
   std::pair<BasicVariant, DbusObject> UnpackDBusField(DBusMessageIter& iter)
   {
      DbusObject obj;
      DBusMessageIter recurse;
      int obj_type = dbus_message_iter_get_arg_type(&iter);
      obj.value.type = obj_type;

      switch (obj_type)
      {
      case DBUS_TYPE_BYTE:    // Fallthrough intentional.
      case DBUS_TYPE_BOOLEAN:
      case DBUS_TYPE_INT16:
      case DBUS_TYPE_UINT16:
      case DBUS_TYPE_INT32:
      case DBUS_TYPE_UINT32:
      case DBUS_TYPE_INT64:
      case DBUS_TYPE_UINT64:
      case DBUS_TYPE_DOUBLE:
      case DBUS_TYPE_UNIX_FD:
         dbus_message_iter_get_basic(&iter, &obj.value.v);
         return {BasicVariant{}, obj};
         break;
      case DBUS_TYPE_STRING:        // Fallthrough intentional.
      case DBUS_TYPE_SIGNATURE:
      case DBUS_TYPE_OBJECT_PATH:
         dbus_message_iter_get_basic(&iter, &obj.value.v);
         if (obj.value.v.str)
            obj.value.s = obj.value.v.str;
         return {BasicVariant{}, obj};
         break;
      case DBUS_TYPE_VARIANT: // A struct/array with one entry
         dbus_message_iter_recurse(&iter, &recurse);
         return UnpackDBusField(recurse);
         break;
      case DBUS_TYPE_ARRAY:   // All children should be the same type, but we don't actually care.
      case DBUS_TYPE_STRUCT:  // Children can have different types. Fallthrough intentional
         dbus_message_iter_recurse(&iter, &recurse);
         UnpackDBusObject(obj, recurse);
         return {BasicVariant{}, obj};
         break;
      case DBUS_TYPE_DICT_ENTRY: // The type used by ARRAY when we really want a dictionary
      {
         // recursing will pull out a tuple, where the first is the key and the second is the value
         dbus_message_iter_recurse(&iter, &recurse);
         auto key = UnpackDBusField(recurse);
         if (dbus_message_iter_next(&recurse))
            return {key.second.value, UnpackDBusField(recurse).second};
         break;
      }
      }
      return {};
   }
   void UnpackDBusObject(DbusObject& obj, DBusMessageIter& iter)
   {
      int current_type = dbus_message_iter_get_arg_type(&iter);
      while (current_type != DBUS_TYPE_INVALID)
      {
         auto field = UnpackDBusField(iter);
         if (field.first.type == DBUS_TYPE_INVALID)
         {
            if (field.second.value.type != DBUS_TYPE_INVALID)
               obj.array.emplace_back(field.second);
         }
         else
            obj.dict.insert(field);

         if (!dbus_message_iter_next(&iter))
            break;

         current_type = dbus_message_iter_get_arg_type(&iter);
      }
   }

   DbusObject UnpackDBusMessage(const std::shared_ptr<DBusMessage>& msg)
   {
      DBusMessageIter iter;
      if (dbus_message_iter_init(msg.get(), &iter))
         return UnpackDBusField(iter).second;
      
      return DbusObject{};
   }

   
   void DumpDBusMessage(const std::shared_ptr<DBusMessage>& msg)
   {
      auto obj = UnpackDBusMessage(msg);
      std::cout << obj.ToString() << '\n';
   }


   void PackArg(DBusMessageIter& iter, const std::string& s)
   {
      if (!dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, s.c_str()))
         throw std::runtime_error("Out of memory");
   }
   void PackArg(DBusMessageIter& iter, const char* s)
   {
      if (!dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, s))
         throw std::runtime_error("Out of memory");
   }
   void PackArg(DBusMessageIter& iter, uint16_t u)
   {
      if (!dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT16, &u))
         throw std::runtime_error("Out of memory");
   }
   void PackArg(DBusMessageIter& iter, int16_t i)
   {
      if (!dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT16, &i))
         throw std::runtime_error("Out of memory");
   }
   void PackArg(DBusMessageIter& iter, const DbusObject& obj)
   {
      DBusMessageIter recurse;
      switch(obj.value.type)
      {
      case DBUS_TYPE_VARIANT:
      {
         assert(obj.array.size() == 1);
         if (obj.array.size() == 1)
         {
            if (!dbus_message_iter_open_container(&iter, obj.value.type, obj.array.front().Signature().c_str(), &recurse))
               throw std::runtime_error("Out of memory");
         
            PackArg(recurse, obj.array.front());
            dbus_message_iter_close_container(&iter, &recurse);
         }
         break;
      }
      case DBUS_TYPE_STRUCT:
         if (!dbus_message_iter_open_container(&iter, obj.value.type, nullptr, &recurse))
            throw std::runtime_error("Out of memory");
         for(auto& child: obj.array)
            PackArg(recurse, child);
         dbus_message_iter_close_container(&iter, &recurse);
         break;
      case DBUS_TYPE_ARRAY:
         if (!obj.dict.empty())
         {
            // Pack the dict as an array of dict entries.
            // For some reason dbus infers an 'a' if you create an array, so
            // strip that off of the signature
            std::string sig = obj.Signature().substr(1);
            if (!dbus_message_iter_open_container(&iter, obj.value.type, sig.c_str(), &recurse))
               throw std::runtime_error("Out of memory");
            for(auto& child: obj.dict)
            {
               DBusMessageIter de_iter;
               if (!dbus_message_iter_open_container(&recurse, DBUS_TYPE_DICT_ENTRY, nullptr, &de_iter))
                  throw std::runtime_error("Out of memory");
               // Add key.
               DBusBasicValue v = child.first.v;
               if (child.first.type == DBUS_TYPE_STRING ||
                   child.first.type == DBUS_TYPE_OBJECT_PATH ||
                   child.first.type == DBUS_TYPE_SIGNATURE)
                  v.str = const_cast<char*>(child.first.s.data());
               if (!dbus_message_iter_append_basic(&de_iter, child.first.type, &v))
                  throw std::runtime_error("Out of memory");
               // Add Value
               PackArg(de_iter, child.second);
               dbus_message_iter_close_container(&recurse, &de_iter);
            }
         }
         else if (!obj.array.empty())
         {
            // For some reason dbus infers an a if you create an array, so
            // strip that off of the signature
            std::string sig = obj.Signature().substr(1);
            if (!dbus_message_iter_open_container(&iter, obj.value.type, sig.c_str(), &recurse))
               throw std::runtime_error("Out of memory");
            // A real array.
            for(auto& child: obj.array)
               PackArg(recurse, child);
         }
         else
         {
            // Pack an empty dict.
            // TODO: How to infer the real signature?
            if (!dbus_message_iter_open_container(&iter, obj.value.type, "{sv}", &recurse))
               throw std::runtime_error("Out of memory");
         }
         dbus_message_iter_close_container(&iter, &recurse);
         break;
      case DBUS_TYPE_STRING:        // fallthrough intentional
      case DBUS_TYPE_OBJECT_PATH:
      case DBUS_TYPE_SIGNATURE:
      {
         // Need to put string pointer in the value object before packing it.
         DBusBasicValue v = obj.value.v;
         v.str = const_cast<char*>(obj.value.s.data());
         if (!dbus_message_iter_append_basic(&iter, obj.value.type, &obj.value.v))
            throw std::runtime_error("Out of memory");

         break;
      }
      default:
         if (!dbus_message_iter_append_basic(&iter, obj.value.type, &obj.value.v))
            throw std::runtime_error("Out of memory");
         break;
      }
   }
   // Add more specializations as you need them.

   void PackArgsImpl(DBusMessageIter&) {}


   template<typename ARG, typename... ARGS>
   void PackArgsImpl(DBusMessageIter& iter, const ARG& a, ARGS... args)
   {
      PackArg(iter, a);
      PackArgsImpl(iter, args...);
   }

   template<typename... ARGS>
   void PackArgs(const std::shared_ptr<DBusMessage>& msg, ARGS... args)
   {
      DBusMessageIter iter;
      dbus_message_iter_init_append(msg.get(), &iter);
      PackArgsImpl(iter, args...);
   }

   void PackArgs(const std::shared_ptr<DBusMessage>& msg)
   {
   }
}

struct Asha::AshaImpl
{
   struct SupportedDevice
   {
      std::string name;
      std::string alias;

      struct Side
      {
         struct
         {
            std::string device;  // /org/bluez/hci0/dev_MA_CA_DD_RE_SS_00
            std::string service; // /org/bluez/hci0/dev_MA_CA_DD_RE_SS_00/service0abc
            std::string property;      // These should all be prefixed by service
            std::string audio_control;
            std::string status;
            std::string status_ccc; // Used to inform the client that we want status updates.
            std::string volume;
            std::string le_psm_out;
         } path;
         std::string name;
         std::string alias;
         std::string mac;
         AshaProperties properties{};
         
         AshaImpl* ai = nullptr;
         uint16_t psm_id = 0;
         int8_t volume = -10;
         uint8_t sequence = 0;
         int sock = -1;

         enum Status {STATUS_OK = 0, UNKNOWN_COMMAND = -1, ILLEGAL_PARAMTER = -2, CALL_FAILED = -128};

         std::string Description() const
         {
            if (properties.capabilities & CAPABILITY_BINAURAL)
            {
               if (properties.capabilities & CAPABILITY_RIGHT_SIDE)
                  return name + " (Right)";
               else
                  return name + " (Left)";
            }
            else
               return name;
         }

         Status ReadStatus()
         {
            assert(ai);
            if (ai)
            {
               DbusObject args;
               args.AddVariant<uint16_t>("offset", 0);
               const auto result = UnpackDBusMessage(
                  ai->DBusCall(path.status, "org.bluez.GattCharacteristic1", "ReadValue", args)
               );
               if (result.value.type != DBUS_TYPE_ARRAY)
                  return CALL_FAILED;
               if (result.array.size() != 1)
                  return CALL_FAILED;
               return (Status)(int8_t)result.array[0].value.v.byt;
            }
            return CALL_FAILED;
         }

         bool ReadProperties()
         {
            // Query the device properties.
            DbusObject args;
            args.AddVariant<uint16_t>("offset", 0);
            const auto property_value = UnpackDBusMessage(
               ai->DBusCall(path.property, "org.bluez.GattCharacteristic1", "ReadValue", args)
            );
            if (property_value.value.type != DBUS_TYPE_ARRAY)
               return false;

            uint8_t* p = (uint8_t*)&properties;
            for (size_t i = 0; i < sizeof(properties) && i < property_value.array.size(); ++i)
               p[i] = property_value.array[i].value.v.byt;
            return true;
         }

         void Disconnect()
         {
            if (sock != -1)
            {
               close(sock);
               sock = -1;
            }
         }

         bool Connect()
         {
            DbusObject args;
            args.AddVariant<uint16_t>("offset", 0);
            const auto psm_value = UnpackDBusMessage(
               ai->DBusCall(path.le_psm_out, "org.bluez.GattCharacteristic1", "ReadValue", args)
            );
            if (psm_value.value.type != DBUS_TYPE_ARRAY)
               return false;

            // Should be two bytes.
            if (psm_value.array.empty())
               return false;
            psm_id = psm_value.array[0].value.v.byt;
            if (psm_value.array.size() > 1)
               psm_id |= psm_value.array[1].value.v.byt << 8;

            sock = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
            struct sockaddr_l2 addr{};
            addr.l2_family = AF_BLUETOOTH;
            addr.l2_bdaddr_type = BDADDR_LE_PUBLIC;
            if (0 != bind(sock, (struct sockaddr*)&addr, sizeof(addr)))
            {
               int e = errno;
               std::cout << "Failed to bind l2cap socket: " << e << '\n';
               return false;
            }
            
            addr.l2_psm = htobs(psm_id);
            sscanf(mac.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
               &addr.l2_bdaddr.b[5],&addr.l2_bdaddr.b[4],&addr.l2_bdaddr.b[3],
               &addr.l2_bdaddr.b[2],&addr.l2_bdaddr.b[1],&addr.l2_bdaddr.b[0]
            );
            // str2ba(mac.c_str(), &addr.l2_bdaddr);

            // Default mtu is 27 bytes (23 bytes of payload). Lets increase
            // this to 160 + 4 bytes payload + 2 byte optional sdu + 1 byte
            // sequence = 167
            //struct l2cap_options opts{};
            //socklen_t optlen = sizeof(opts);
            //if (0 == getsockopt(sock, SOL_L2CAP, L2CAP_OPTIONS, &opts, &optlen))
            //{
            //   // opts.omtu = opts.imtu = 167;
            //   opts.mode = 0x80; // L2CAP_MODE_LE_FLOWCTL (CoC mode, should be this by default anyways)
            //   if (0 != setsockopt(sock, SOL_L2CAP, L2CAP_OPTIONS, &opts, optlen))
            //   {
            //      int err = errno;
            //      std::cout << "Failed to set l2cap channel mode: " << err << "\n";
            //   }
            //   if (0 == getsockopt(sock, SOL_L2CAP, L2CAP_OPTIONS, &opts, &optlen))
            //   {
            //      std::cout << "imtu: " << (int)opts.imtu << '\n';
            //      std::cout << "omtu: " << (int)opts.omtu << '\n';
            //      std::cout << "mode: " << (int)opts.mode << '\n';
            //   }
            //}

            // SOL_L2CAP options don't handle the new CoC mode. Use SOL_BLUETOOTH instead
            uint8_t mode = BT_MODE_LE_FLOWCTL;
            if (0 != setsockopt(sock, SOL_BLUETOOTH, BT_MODE, &mode, sizeof(mode)))
            {
               int e = errno;
               std::cout << "Unable to set CoC flow control mode " << e << '\n';
               return 1;
            }


            if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0)
            {
               int err = errno;
               std::cout << "Failed to connect l2cap channel: " << err << "\n";
               close(sock);
               sock = -1;
               return false;
            }

            return true;
         }

         void SetVolume(int volume)
         {
            this->volume = volume;

            // This one is optional.
            if (!path.volume.empty())
            {
               DbusObject byte_args;
               byte_args.SetBytes(&volume, 1);

               DbusObject dict_args;
               dict_args.AddVariant<uint16_t>("offset", 0);
               dict_args.AddVariant("type", "command");


               auto ret = UnpackDBusMessage(
                  ai->DBusCall(path.volume, "org.bluez.GattCharacteristic1", "WriteValue", byte_args, dict_args)
               );

               if (!ret.Null())
                  std::cout << "Volume response: " << ret.ToString() << '\n';


               auto status = ReadStatus();
               if (status != STATUS_OK)
                  std::cout << "Volume ReadStatus returned " << (int)status << '\n';
            }
         }


         bool EnableStatusNotifications()
         {
            // Turn on status notifications.
            const auto notifyret = UnpackDBusMessage(
               ai->DBusCall(path.status, "org.bluez.GattCharacteristic1", "StartNotify")
            );
            if (!notifyret.Null())
            {
               std::cout << "Failed to subscribe to status notifications: " << notifyret.ToString();
               return false;
            }
            
            // Stack overflow says that bluez will do this for me when I call StartNotify
            // DbusObject byte_args;
            // const uint8_t enable_notify = 0x02;
            // byte_args.SetBytes(&enable_notify, sizeof(enable_notify));
            //
            // DbusObject dict_args;
            // dict_args.AddVariant<uint16_t>("offset", 0);
            // const auto status_ccc_ret = UnpackDBusMessage(
            //    ai->DBusCall(path.status_ccc, "org.bluez.GattDescriptor1", "WriteValue", byte_args, dict_args)
            // );
            // if (!status_ccc_ret.Null())
            // {
            //    std::cout << "Failed to enable status notifications: " << status_ccc_ret.ToString() << '\n';
            //    return false;
            // }

            return true;
         }
         bool DisableStatusNotifications()
         {
            // Turn on status notifications.
            const auto notifyret = UnpackDBusMessage(
               ai->DBusCall(path.status, "org.bluez.GattCharacteristic1", "StopNotify")
            );
            if (!notifyret.Null())
            {
               std::cout << "Failed to unsubscribe to status notifications: " << notifyret.ToString();
               return false;
            }
            
            DbusObject byte_args;
            const uint8_t disable_notify = 0x00;
            byte_args.SetBytes(&disable_notify, sizeof(disable_notify));
            
            DbusObject dict_args;
            dict_args.AddVariant<uint16_t>("offset", 0);
            const auto status_ccc_ret = UnpackDBusMessage(
               ai->DBusCall(path.status_ccc, "org.bluez.GattDescriptor1", "WriteData", byte_args, dict_args)
            );
            if (!status_ccc_ret.Null())
            {
               std::cout << "Failed to disable status notifications: " << status_ccc_ret.ToString();
               return false;
            }

            return true;
         }

         bool Start(PlaybackType type, bool otherstate)
         {
            uint8_t codec = 1; // G.722 No other protocols listed in doc.

            EnableStatusNotifications();

            struct {
               uint8_t opcode;
               uint8_t codec;
               uint8_t audiotype;
               int8_t volume;
               int8_t otherstate;
            } startargs {
               1, codec, (uint8_t)type, volume, otherstate
            };

            DbusObject byte_args;
            byte_args.SetBytes(&startargs, sizeof(startargs));

            DbusObject dict_args;
            dict_args.AddVariant<uint16_t>("offset", 0);
            dict_args.AddVariant("type", "request");

            const auto ret = UnpackDBusMessage(
               ai->DBusCall(path.audio_control, "org.bluez.GattCharacteristic1", "WriteValue", byte_args, dict_args)
            );
            if (!ret.Null())
               std::cout << "Start response: " << ret.ToString() << '\n';

            auto status = ReadStatus();
            if (status != STATUS_OK)
               std::cout << "Start ReadStatus returned " << (int)status << '\n';

            // Directly setting the volume just in case.
            SetVolume(volume);

            sequence = 0;

            return true;
         }

         bool Stop()
         {
            uint8_t opcode = 2;

            DbusObject byte_args;
            byte_args.SetBytes(&opcode, 1);

            DbusObject dict_args;
            dict_args.AddVariant<uint16_t>("offset", 0);
            dict_args.AddVariant("type", "request");

            const auto ret = UnpackDBusMessage(
               ai->DBusCall(path.audio_control, "org.bluez.GattCharacteristic1", "WriteValue", byte_args, dict_args)
            );
            if (!ret.Null())
               std::cout << "Stop response: " << ret.ToString() << '\n';

            sequence = 0;

            auto status = ReadStatus();
            if (status != STATUS_OK)
               std::cout << "Stop ReadStatus returned " << (int)status << '\n';

            DisableStatusNotifications();

            return true;
         }

         bool WriteAudioFrame(uint8_t* data, size_t size)
         {
            // TODO: should we write 10ms or 20ms of data? It depends on the
            //       connection paramters? Default to 20ms for now

            assert(size <= 160);
            assert(sock != -1);
            if (sock != -1)
            {
               if (size > 160)
                  size = 160;
               struct {
                  //uint16_t sdu_length;
                  uint8_t seq;
                  uint8_t data[161];
               } packet;
               // packet.sdu_length = htobs(size + 1);
               packet.seq = sequence++;
               memcpy(packet.data, data, size);
               if (send(sock, &packet, size + 1, 0) == size + 1)
                  return true;
               else
                  std::cout << "Disconnected by " << Description() << '\n';
            }
            return false;
         }
      };

      // These devices must all have the same properties.hi_sync_id
      std::vector<Side> devices;
   };

   void EnumerateDevices();

   // Should there be a sync/async variant? This is synchronous.
   template<typename... ARGS>
   std::shared_ptr<DBusMessage> DBusCall(const std::string& path, const std::string& iface, const std::string& method, ARGS...);

   std::shared_ptr<DBusConnection> dbus;
   std::map<uint64_t, SupportedDevice> supported_devices;

   SupportedDevice* current_device = nullptr;

   uint64_t selected_id = 0;
};


Asha::Asha():
   m(new AshaImpl)
{
   DBusError err;
   dbus_error_init(&err);
   std::shared_ptr<DBusError> perr(&err, dbus_error_free);

   m->dbus.reset(dbus_bus_get(DBUS_BUS_SYSTEM, &err), [](DBusConnection*){});
   //m->dbus.reset(dbus_bus_get(DBUS_BUS_SESSION, &err), dbus_connection_close); // DBUS spits out a message telling me not to close this connection.
   if (dbus_error_is_set(&err))
      throw std::runtime_error("Unable to access dbus session: " + std::string(err.message));

   if (m->dbus == nullptr)
      throw std::runtime_error("Failed to get dbus session.");

   // int ret = dbus_bus_request_name(m->dbus.get(), DBUS_NAME.c_str(), DBUS_NAME_FLAG_REPLACE_EXISTING, &err);
   // if (dbus_error_is_set(&err))
   //    throw std::runtime_error("Unable to get dbus name: " + std::string(err.message));
   // if (DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER != ret)
   //    throw std::runtime_error("Failed to get dbus name");

   dbus_connection_add_filter(m->dbus.get(), [](DBusConnection*, DBusMessage* msg, void*p) {
      //auto* self = (Asha*)p;
      
      //if (dbus_message_get_type(msg) == DBUS_MESSAGE_TYPE_SIGNAL)
      {
         int msg_type = dbus_message_get_type(msg);
         if (msg_type == DBUS_MESSAGE_TYPE_METHOD_CALL) std::cout << "Call: ";
         else if (msg_type == DBUS_MESSAGE_TYPE_METHOD_RETURN) std::cout << "Return: ";
         else if (msg_type == DBUS_MESSAGE_TYPE_SIGNAL) std::cout << "Signal: ";
         else std::cout << "Unknown " << msg_type << ": ";
         std::cout << dbus_message_get_path(msg)
                  << " " << dbus_message_get_interface(msg)
                  << " " << dbus_message_get_member(msg);

         DBusMessageIter iter;
         if (dbus_message_iter_init(msg, &iter))
            std::cout << " " << UnpackDBusField(iter).second.ToString();
         std::cout << '\n';
      }
      return DBUS_HANDLER_RESULT_NOT_YET_HANDLED; // or DBUS_HANDLER_RESULT_HANDLED to remove it
   }, this, nullptr);

   // "If you don't set a dispatch status function, you must call
   // dbus_connection_status on every iteration of your main loop",
   // ... which is what we are doing.
   // dbus_connection_set_dispatch_status_function(m->dbus.get(),
   //    DBusDispatchStatusFunction function, this, nullptr
   // )

   // Used to give us file descriptors to select() or poll() on
   // dbus_connection_set_watch_functions(m->dbus.get(),
   //    DBusAddWatchFunction add_function,
   //    DBusRemoveWatchFunction remove_function,
   //    DBusWatchToggledFunction toggled_function,
   //    this, nullptr);

   // Used to set up timers. Call dbus_timeout_handle() when it is meant to trigger
   // dbus_connection_set_timeout_functions(m->dbus.get(),
   //    DBusAddTimeoutFunction,
   //    DBusRemoveTimeoutFunction,
   //    DBusTimeoutToggledFunction,
   //    this, nullptr);
   
   m->EnumerateDevices();
}

template<typename... ARGS>
std::shared_ptr<DBusMessage> Asha::AshaImpl::DBusCall(const std::string& path, const std::string& iface, const std::string& method, ARGS... args)
{
   //std::cout << "Dbus call: org.bluez " << path << " " << iface << " " << method << '\n';
   std::shared_ptr<DBusMessage> msg(
      dbus_message_new_method_call("org.bluez", path.c_str(), iface.c_str(), method.c_str()),
      dbus_message_unref
   );
   if (!msg) throw
      std::runtime_error("Out of memory");

   PackArgs(msg, args...);

   DBusPendingCall* pending = nullptr;
   if (!dbus_connection_send_with_reply(dbus.get(), msg.get(), &pending, -1))
      throw std::runtime_error("Out of memory");
   std::shared_ptr<DBusPendingCall> pending_ptr(pending, dbus_pending_call_unref);

   dbus_connection_flush(dbus.get());

   // This blocks until it is finished. If this were an asynchronous function,
   // we would return pending here instead.
   dbus_pending_call_block(pending);

   msg.reset(
      dbus_pending_call_steal_reply(pending),
      dbus_message_unref
   );

   return msg;
}

void Asha::AshaImpl::EnumerateDevices()
{
   auto msg = DBusCall("/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
   if (!msg)
      throw std::runtime_error("GetManagedObjects call failed");

   // DumpDBusMessage(msg);

   // Returns a structure that looks like this:
   // [
   //    ... other stuff ...
   //    "/org/bluez/hci0/dev_MA_CA_DD_RE_SS_00": {
   //       "org.bluez.Device1": {
   //          "Address": "MA:CA:DD:RE:SS:00",
   //          "AddressType": "public",
   //          "Name": "Cool Device",
   //          "Alias": "Cool Device",
   //          "Paired": true,
   //          "Bonded": true,
   //          "Trusted": true,
   //          "Blocked": false,
   //          "LegacyPairing": false,
   //          "Connected": true,
   //          "UUIDs": [
   //             ...
   //             "0000fdf0-0000-1000-8000-00805f9b34fb", // <-- asha gatt service {0xfdf0}
   //          ],
   //          "Modalias": "bluetooth:vGIBBERISH",
   //          "Adapter": "/org/bluez/hci0",
   //          "ServicesResolved": true
   //       }
   //    },
   //    "/org.bluez/hci0/dev_MA_CA_DD_RE_SS_00/service/service00b8": {
   //       "org.freedesktop.DBus.Introspectable": {}
   //       "org.bluez.GattService1": {
   //          "UUID": "0000fdf0-0000-1000-8000-00805f9b34fb",
   //          "Device": "/org/bluez/hci0/MA_CA_DD_RE_SS_00",
   //          "Primary": true
   //       "org.freedesktop.DBus.Properties": {}
   //    },
   //    "/org.bluez/hci0/dev_MA_CA_DD_RE_SS_00/service/service00b8/char00c6": {
   //       "org.bluez.GattCharacteristic1": {
   //          "UUID": {2d410339-82b6-42aa-b34e-e2e01df8cc1a},
   //          "FLAGS": ["read"]
   //       }
   //    }
   // ]
   //
   // In general, bluez dbus paths are composed of generalized objects that
   // have a hierarchy. Note that this hierarchy is not reflected in the nested
   // structure of the data, which instead represents generic objects.
   // In this case, the path hierarchy looks like this:
   // Bluez:
   //    bluetooth adaptor
   //       peripheral device 1
   //          Services
   //             characteristic interface 1
   //                Descriptor 1
   //                Descriptor 2
   //             characteristic interface 2
   //       peripheral device 2
   //          Services
   //             Service interface 1
   //             Service interface 2
   //
   // Our goal is to find all devices that implement the asha gatt service,
   // and arrange them in pairs.

   std::vector<SupportedDevice::Side> found_devices;

   // Build a list of devices that support the asha gatt service
   auto response = UnpackDBusMessage(msg);
   for (const auto& device: response.dict)
   {
      // Check for Device1 interface
      // https://git.kernel.org/pub/scm/bluetooth/bluez.git/plain/doc/org.bluez.Device.rst
      auto& device1 = device.second.Child("org.bluez.Device1");
      if (device1.Null())
         continue;

      // Is the device currently connected?
      if (!device1.Child("Connected").Bool())
         continue;

      // Does the device support the asha gatt?
      bool supports_asha = false;
      for (auto& uuid: device1.Child("UUIDs").array)
      {
         if (uuid.value.type == DBUS_TYPE_STRING && uuid.value.s == ASHA_GATT_SERVICE_UUID)
         {
            supports_asha = true;
            break;
         }
      }
      if (!supports_asha)
         continue;

      std::cout << device1.ToString() << '\n';
      // This is a valid device. Copy down its properties.
      SupportedDevice::Side side;
      side.path.device = device.first.s;
      side.name = device1.Child("Name").String();
      side.alias = device1.Child("Alias").String();
      side.mac = device1.Child("Address").String();
      side.ai = this;

      found_devices.push_back(side);
   }

   // Find the interface paths for each device.
   for (auto& side: found_devices)
   {
      // Find the asha gatt.
      for (const auto& device: response.dict)
      {
         if (device.first.s.substr(0, side.path.device.size()) == side.path.device)
         {
            // https://git.kernel.org/pub/scm/bluetooth/bluez.git/plain/doc/org.bluez.GattService.rst
            auto& gatt1 = device.second.Child("org.bluez.GattService1");
            if (gatt1.Null())
               continue;
            if (gatt1.Child("UUID").String() == ASHA_GATT_SERVICE_UUID)
            {
               // this is it
               side.path.service = device.first.s;
               break;
            }
         }
      }

      if (!side.path.service.empty())
      {
         // Find all the characteristics associated with this service.
         for (const auto& device: response.dict)
         {
            if (device.first.s.substr(0, side.path.service.size()) == side.path.service)
            {
               //https://git.kernel.org/pub/scm/bluetooth/bluez.git/plain/doc/org.bluez.GattCharacteristic.rst
               auto& characteristic = device.second.Child("org.bluez.GattCharacteristic1");
               if (characteristic.Null())
                  continue;
               auto& uuid = characteristic.Child("UUID").String();
               if (uuid == ASHA_READ_ONLY_PROPERTIES)
                  side.path.property = device.first.s;
               else if (uuid == ASHA_AUDIO_CONTROL_POINT)
                  side.path.audio_control = device.first.s;
               else if (uuid == ASHA_AUDIO_STATUS)
                  side.path.status = device.first.s;
               else if (uuid == ASHA_VOLUME)
                  side.path.volume = device.first.s;
               else if (uuid == ASHA_LE_PSM_OUT)
                  side.path.le_psm_out = device.first.s;
            }
         }

         // Find all the ccc descriptor associated with the status update
         if (!side.path.status.empty())
         {
            for (const auto& obj: response.dict)
            {
               if (obj.first.s.substr(0, side.path.status.size()) == side.path.status)
               {
                  // https://git.kernel.org/pub/scm/bluetooth/bluez.git/tree/doc/org.bluez.GattDescriptor.rst
                  auto& descriptor = obj.second.Child("org.bluez.GattDescriptor1");
                  if (descriptor.Null())
                     continue;
                  auto& uuid = descriptor.Child("UUID").String();
                  if (uuid == DESCRIPTOR_CCC)
                     side.path.status_ccc = obj.first.s;
               }
            }
         }
      }

      // Volume is optional, but make sure we found everything else we need.
      if (side.path.service.empty() ||
          side.path.property.empty() ||
          side.path.audio_control.empty() ||
          side.path.status.empty() ||
          side.path.status_ccc.empty() ||
          side.path.le_psm_out.empty())
         continue;

      // Query the device properties.
      if (!side.ReadProperties())
         continue;

      side.ReadStatus();

      // Check for required features.
      if ((side.properties.feature_map & FEATURE_STREAMING) == 0)
         continue;
      if ((side.properties.codecs & CODEC_G722_16KHZ) == 0)
         continue;

      // Looks like we are good to go!
      auto& sd = supported_devices[side.properties.hi_sync_id];
      sd.name = side.name;
      sd.alias = side.alias;
      sd.devices.push_back(side);
   }


   if (supported_devices.empty())
   {
      std::cout << "No supported devices found\n";
   }
   else
   {
      for (auto& sd: supported_devices)
      {
         std::cout << "HiSyncId " << sd.first << "\n";
         for (auto& side: sd.second.devices)
         {
            std::cout << "  Name:      " << side.name << '\n';
            if (side.name != side.alias)
               std::cout << "    Alias:     " << side.alias << '\n';
            std::cout << "    Side:      "
                        << ((side.properties.capabilities & 0x01) ? "right" : "left")
                        << ((side.properties.capabilities & 0x02) ? " (binaural)" : " (monaural)")
                        << '\n';
            std::cout << "    Delay:     " << side.properties.render_delay << "ms\n";
            std::cout << "    Streaming: " << (side.properties.feature_map & 0x01 ? "supported" : "not supported" ) << '\n';
            std::cout << "    Codecs:    "
                  << (side.properties.codecs & 0x02 ? "G.722" : "" )
                  << '\n';
         }
      }
   }
}


std::vector<Asha::AshaDevice> Asha::Devices() const
{
   std::vector<Asha::AshaDevice> ret;
   for (auto& kv: m->supported_devices)
   {
      ret.emplace_back(Asha::AshaDevice{kv.first, kv.second.name});
   }
   return ret;
}


void Asha::SelectDevice(uint64_t id)
{
   // TODO: clean up any old devices selected
   if (m->selected_id)
   {
      // TODO: If playing, stop

      // Close the data channels.
      auto it = m->supported_devices.find(m->selected_id);
      for (auto& side: it->second.devices)
         side.Disconnect();
   }

   m->selected_id = 0;
   m->current_device = nullptr;
   auto it = m->supported_devices.find(id);
   if (it == m->supported_devices.end())
   {
      std::cout << "Unsupported device\n";
      return;
   }

   m->selected_id = id;
   m->current_device = &it->second;

   for (auto& side: m->current_device->devices)
   {
      side.Disconnect();
      if (!side.Connect())
      {
         std::cout << "Failed to connect to " << side.Description() << '\n';
         continue;
      }
   }
}


void Asha::Start()
{
   for (auto& side: m->current_device->devices)
      side.Start(UNKNOWN, m->current_device->devices.size() > 1);
}

void Asha::Stop()
{
   bool first = false;
   for (auto& side: m->current_device->devices)
      side.Stop();
}

bool Asha::SendAudio(uint8_t* left, uint8_t* right, size_t size)
{
   bool success = true;
   for (auto& side: m->current_device->devices)
   {
      
      if ((side.properties.capabilities & CAPABILITY_RIGHT_SIDE) != 0 && right)
         success = side.WriteAudioFrame(right, size) && success;
      if ((side.properties.capabilities & CAPABILITY_RIGHT_SIDE) == 0 && left)
         success = side.WriteAudioFrame(left, size) && success;
   }
   return success;
}

void Asha::SetVolume(int8_t v)
{
   for (auto& side: m->current_device->devices)
      side.SetVolume(v);
}


void Asha::Process(int timeout_ms)
{
   if (m->dbus)
   {
      // Waits until an event is ready, then dispatches at most one event.
      // TODO: Returns false when dbus is disconnected.
      //dbus_connection_read_write_dispatch(m->dbus.get(), timeout_ms);

      dbus_connection_read_write(m->dbus.get(), timeout_ms);

      while (dbus_connection_get_dispatch_status(m->dbus.get()) == DBUS_DISPATCH_DATA_REMAINS)
      {
         // Lock the message and get a pointer to it.
         DBusMessage* msg = dbus_connection_borrow_message(m->dbus.get());
         // {
         //    int msg_type = dbus_message_get_type(msg);
         //    if (msg_type == DBUS_MESSAGE_TYPE_METHOD_CALL) std::cout << "Call2: ";
         //    else if (msg_type == DBUS_MESSAGE_TYPE_METHOD_RETURN) std::cout << "Return2: ";
         //    else if (msg_type == DBUS_MESSAGE_TYPE_SIGNAL) std::cout << "Signal2: ";
         //    else std::cout << "Unknown2 " << msg_type << ": ";
         //    std::cout << dbus_message_get_path(msg)
         //             << " " << dbus_message_get_interface(msg)
         //             << " " << dbus_message_get_member(msg);
         //    DBusMessageIter iter;
         //    if (dbus_message_iter_init(msg, &iter))
         //       std::cout << " " << UnpackDBusField(iter).second.ToString();
         //    std::cout << '\n';
         // }

         // Give the message back and dispatch it.
         dbus_connection_return_message(m->dbus.get(), msg);
         dbus_connection_dispatch(m->dbus.get());
      }
   }
}