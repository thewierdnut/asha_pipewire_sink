#include "asha/Bluetooth.hh"
#include "asha/RawHci.hh"
#include "asha/Side.hh"

#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>
#include <glib-2.0/glib.h>
#include <glib-2.0/glib-unix.h>

#include <cstdlib>
#include <iostream>
#include <vector>
#include <fstream>


class ConnectTest
{
public:
   ConnectTest():
      m_b(
         [this](const asha::Bluetooth::BluezDevice& d) { OnAddDevice(d); },
         [this](const std::string& p) { OnRemoveDevice(p); }
      )
   {
      CheckConfig();
   }

protected:
   void OnAddDevice(const asha::Bluetooth::BluezDevice& d)
   {
      std::shared_ptr<asha::Side> side = asha::Side::CreateIfValid(d);
      if (side && side->ReadProperties())
      {
         auto& props = side->GetProperties();

         g_info("%s", side->Description().c_str());
         g_info("    Name:      %s", side->Name().c_str());
         g_info("    Mac:       %s", side->Mac().c_str());
         g_info("    HiSyncId:  %lu", props.hi_sync_id);
         if (side->Name() != side->Alias())
            g_info("    Alias:     %s", side->Alias().c_str());
         g_info("    Side:      %s %s",
            ((props.capabilities & 0x01) ? "right" : "left"),
            ((props.capabilities & 0x02) ? "(binaural)" : "(monaural)"));
         g_info("    Delay:     %hu ms", props.render_delay);
         g_info("    Streaming: %s", (props.feature_map & 0x01 ? "supported" : "not supported" ));
         std::string codecs;
         if (props.codecs & 0x02)
            codecs += " G.722@16kHz";
         if (props.codecs & 0x04)
            codecs += " G.722@24kHz";
         g_info("    Codecs:   %s", codecs.c_str());
         bool connected = side->Connect();
         g_info("    Connected: %s", connected ? "true" : "false");

         m_devices[d.path] = side;

         // sockopts
         CheckConnInfo(side);
         // BT_SECURITY? returns BT_SECURITY_* constants, plus key size.
         // BT_DEFER_SETUP? meant for listening sockets.
         // BT_FLUSHABLE? gets the FLAG_FLUSHABLE flag
         // BT_POWER? gets FLAG_FORCE_ACTIVE flag
         CheckMTU(side);
         CheckPHY(side);
         CheckMODE(side);

         // Raw socket ioctls
         RawHci hci_connection(side->Mac(), side->Sock());
         CheckHciConnInfo(hci_connection);
         CheckCfgDefaults(hci_connection);
      }
      else
      {
         g_info("%s is not an asha-enabled device", d.name.c_str());
      }
   }
   void OnRemoveDevice(const std::string& path)
   {
      auto it = m_devices.find(path);
      if (it != m_devices.end())
      {
         g_info("Removing %s", it->second->Description().c_str());
         m_devices.erase(it);
      }
   }

protected:
   void CheckConnInfo(const std::shared_ptr<asha::Side>& device)
   {
      struct l2cap_conninfo ci{};
      socklen_t size = sizeof(ci);
      int err = getsockopt(device->Sock(), SOL_L2CAP, L2CAP_CONNINFO, &ci, &size);
      if (err < 0)
      {
         g_warning("    Error retrieving L2CAP_CONNINFO: %s %u", strerror(-err), -err);
         return;
      }
      g_info("    L2CAP_CONNINFO:");
      g_info("       handle: %hu", ci.hci_handle); // This is the hci connection id that you would need for raw hci stanzas.
      g_info("       class:  [%hhu, %hhu, %hhu]", ci.dev_class[0], ci.dev_class[1], ci.dev_class[2]);

      // HCIGETCONNINFO ioctl... we already have this information via RawHci::ConnectionInfo()
      // std::unique_ptr<char[]> buffer(new char[sizeof(struct hci_conn_info_req) + sizeof(hci_conn_info)]);
      // auto* cr = (struct hci_conn_info_req*)buffer.get();
      // sscanf(device->Mac().c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
      //    &cr->bdaddr.b[5],&cr->bdaddr.b[4],&cr->bdaddr.b[3],
      //    &cr->bdaddr.b[2],&cr->bdaddr.b[1],&cr->bdaddr.b[0]
      // );
      // cr->type = ACL_LINK;
   }

   void CheckHciConnInfo(const RawHci& hci)
   {
      auto state_str = [](uint16_t state) {
         switch(state) {
         case BT_CONNECTED: return "CONNECTED";
         case BT_OPEN: return "OPEN";
         case BT_BOUND: return "BOUND";
         case BT_LISTEN: return "LISTEN";
         case BT_CONNECT: return "CONNECT";
         case BT_CONNECT2: return "CONNECT2";
         case BT_CONFIG: return "CONFIG";
         case BT_DISCONN: return "DISCONN";
         case BT_CLOSED: return "CLOSED";
         default: return "invalid state";
         }
      };
      auto type_str = [](uint8_t type) {
         switch(type) {
         case SCO_LINK: return "SCO";
         case ACL_LINK: return "ACL";
         case ESCO_LINK: return "ESCO";
         case 0x80: return "LE";    // defined in include/net/bluetooth/hci.h in kernel
         case 0x81: return "AMP";
         case 0x82: return "ISO";
         case 0xff: return "INVALID";
         default: return "invalid link type";
         }
      };
      auto link_mode_str = [](uint32_t mode) {
         std::string s;
         if (mode & HCI_LM_ACCEPT) s += " ACCEPT";
         if (mode & HCI_LM_MASTER) s += " MASTER";
         if (mode & HCI_LM_AUTH) s += " AUTH";
         if (mode & HCI_LM_ENCRYPT) s += " ENCRYPT";
         if (mode & HCI_LM_TRUSTED) s += " TRUSTED";
         if (mode & HCI_LM_RELIABLE) s += " RELIABLE";
         if (mode & HCI_LM_SECURE) s += " SECURE";
         if (mode & 0x40 /*HCI_LM_FIPS*/) s += " FIPS";
         return s;
      };
      auto& info = hci.ConnectionInfo();
      g_info("    Hci Connection Info:");
      g_info("       type:   %hhu %s", info.type, type_str(info.type));
      g_info("       out:    %hhu %s", info.out, info.out ? "true" : "false");
      g_info("       state:  %hu %s", info.state, state_str(info.state));
      g_info("       mode:   %u%s", info.link_mode, link_mode_str(info.link_mode).c_str());
   }

   void CheckHciLq(RawHci& hci)
   {
      uint8_t quality = 0;
      if (hci.ReadLinkQuality(&quality))
         g_info("    Quality: %hhu", quality);
      else
         g_info("    Unable to read link quality");
   }

   void CheckHciRssi(RawHci& hci)
   {
      uint8_t rssi = 0;
      if (hci.ReadLinkQuality(&rssi))
         g_info("    Rssi: %hhd", rssi);
      else
         g_info("    Unable to read rssi");
   }


   void CheckCfgDefaults(RawHci& hci)
   {
      RawHci::SystemConfig config;
      if (hci.ReadSysConfig(config))
      {
         g_info("    min_connection_interval: %hu", config.min_conn_interval);
         g_info("    max_connection_interval: %hu", config.max_conn_interval);
      }
   }


   void CheckMTU(const std::shared_ptr<asha::Side>& device)
   {
      uint16_t imtu = 0;
      uint16_t omtu = 0;
      socklen_t size = sizeof(omtu);
      int err = getsockopt(device->Sock(), SOL_BLUETOOTH, BT_SNDMTU, &omtu, &size);
      if (err < 0)
      {
         g_warning("    Error retrieving BT_SNDMTU: %s %u", strerror(-err), -err);
         return;
      }
      size = sizeof(imtu);
      err = getsockopt(device->Sock(), SOL_BLUETOOTH, BT_RCVMTU, &imtu, &size);
      if (err < 0)
      {
         g_warning("    Error retrieving BT_RCVMTU: %s %u", strerror(-err), -err);
         return;
      }
      g_info("    MTU:       SND %hu RCV %hu", omtu, imtu);
      if (omtu < 167)
         g_warning("               Send MTU must be at least 167 bytes to work correctly");
   }
   
   void CheckPHY(const std::shared_ptr<asha::Side>& device)
   {
      uint32_t phys = 0;
      for (int i = 0; i < 60; ++i)
      {
         socklen_t size = sizeof(phys);
         int err = getsockopt(device->Sock(), SOL_BLUETOOTH, BT_PHY, &phys, &size);
         if (err < 0)
         {
            std::cout << "    Error retrieving BT_PHY: " << strerror(-err) << " (" <<  -err << ")\n";
            return;
         }

         if (phys & BT_PHY_LE_2M_TX)
            break;

         usleep(10000);
      }

      std::string phystr = std::to_string(phys);
      if (phys & BT_PHY_BR_1M_1SLOT) phystr += " BR_1M_1SLOT";
      if (phys & BT_PHY_BR_1M_3SLOT) phystr += " BR_1M_3SLOT";
      if (phys & BT_PHY_BR_1M_5SLOT) phystr += " BR_1M_5SLOT";
      if (phys & BT_PHY_EDR_2M_1SLOT) phystr += " EDR_2M_1SLOT";
      if (phys & BT_PHY_EDR_2M_3SLOT) phystr += " EDR_2M_3SLOT";
      if (phys & BT_PHY_EDR_2M_5SLOT) phystr += " EDR_2M_5SLOT";
      if (phys & BT_PHY_EDR_3M_1SLOT) phystr += " EDR_3M_1SLOT";
      if (phys & BT_PHY_EDR_3M_3SLOT) phystr += " EDR_3M_3SLOT";
      if (phys & BT_PHY_EDR_3M_5SLOT) phystr += " EDR_3M_5SLOT";
      if (phys & BT_PHY_LE_1M_TX) phystr += " LE_1M_TX";
      if (phys & BT_PHY_LE_1M_RX) phystr += " LE_1M_RX";
      if (phys & BT_PHY_LE_2M_TX) phystr += " LE_2M_TX";
      if (phys & BT_PHY_LE_2M_RX) phystr += " LE_2M_RX";
      if (phys & BT_PHY_LE_CODED_TX) phystr += " LE_CODED_TX";
      if (phys & BT_PHY_LE_CODED_RX) phystr += " LE_CODED_RX";
      g_info("    PHY:    %s", phystr.c_str());
   }
   
   void CheckMODE(const std::shared_ptr<asha::Side>& device)
   {
      int mode = 0;
      socklen_t size = sizeof(mode);
      int err = getsockopt(device->Sock(), SOL_BLUETOOTH, BT_MODE, &mode, &size);
      if (err == -ENOPROTOOPT)
      {
         g_warning("    enable_ecred does not appear to be set on the bluetooth module.");
         return;
      }
      if (err < 0)
      {
         g_warning("    Error retrieving BT_MODE: %s %u", strerror(-err), -err);
         return;
      }
      switch(mode)
      {
      case BT_MODE_BASIC:       g_info("    MODE: BASIC (This isn't right)"); break;
      case BT_MODE_ERTM:        g_info("    MODE: ERTM (This isn't right)"); break;
      case BT_MODE_STREAMING:   g_info("    MODE: STREAMING (This isn't right)"); break;
      case BT_MODE_LE_FLOWCTL:  g_info("    MODE: LE_FLOWCTL"); break;
      case BT_MODE_EXT_FLOWCTL: g_info("    MODE: EXT_FLOWCTL"); break;
      }
   }

   void CheckConfig()
   {
      std::shared_ptr<GKeyFile> in(g_key_file_new(), g_key_file_free);
      g_key_file_load_from_file(in.get(), "/etc/bluetooth/main.conf", G_KEY_FILE_NONE, nullptr);

      if (g_key_file_get_integer(in.get(), "LE", "MinConnectionInterval", nullptr) != 16
       || g_key_file_get_integer(in.get(), "LE", "MaxConnectionInterval", nullptr) != 16)
      {
         g_info("MinConnectionInterval and MaxConnectionInterval do not appear to be set correctly in the bluetooth config file.");
         g_info("Please edit /etc/bluetooth/main.conf to uncomment and set the following values:");
         g_info("    MinConnectionInterval=16");
         g_info("    MaxConnectionInterval=16");
         g_info("    ConnectionLatency=10");
         g_info("    ConnectionSupervisionTimeout=100");
         g_info("You will need to restart bluez when you are done");
      }

   }


private:
   std::map<std::string, std::shared_ptr<asha::Side>> m_devices;

   asha::Bluetooth m_b; // needs to be last
};


int main()
{
   setenv("G_MESSAGES_DEBUG", "all", false);
   ConnectTest c;


   std::shared_ptr<GMainLoop> loop(g_main_loop_new(nullptr, true), g_main_loop_unref);
   auto quitter = g_unix_signal_add(SIGINT, [](void* ml) {
      g_main_loop_quit((GMainLoop*)ml);
      return (int)G_SOURCE_CONTINUE;
   }, loop.get());

   g_main_loop_run(loop.get());
   g_source_remove(quitter);

   std::cout << "Stopping...\n";

   return 0;
}