#include "MainWindow.hh"

#include "DeviceWidget.hh"

#include <string>
#include <sstream>
#include <iomanip>
#include <iostream>

#include <gtkmm/label.h>
#include <gtkmm/box.h>

#include "../asha/Device.hh"
#include "../asha/Config.hh"

MainWindow::MainWindow()
{
   int row = 0;
   set_size_request(320);
   m_pw_name.set_label("No device found");
   m_pw_name.set_hexpand();
   m_layout.attach(m_pw_name, 0, row, 2);
   ++row;

   m_hisync_label.set_label("Id:");
   m_hisync_label.set_justify(Gtk::Justification::RIGHT);
   m_hisync_label.set_halign(Gtk::Align::END);
   m_hisync.set_hexpand();
   m_layout.attach(m_hisync_label, 0, row);
   m_layout.attach(m_hisync, 1, row);
   ++row;

   m_rssi_reconnect_label.set_label("Reconnect Rssi:");
   m_rssi_reconnect_label.set_justify(Gtk::Justification::RIGHT);
   m_rssi_reconnect_label.set_halign(Gtk::Align::END);
   m_rssi_reconnect.set_range(-127, 0);
   m_rssi_reconnect.set_value(asha::Config::RssiPaired());
   m_rssi_reconnect.set_round_digits(0);
   m_rssi_reconnect.signal_value_changed().connect(sigc::mem_fun(*this, &MainWindow::OnReconnectRssiChanged), true);
   m_rssi_reconnect.set_draw_value();
   m_rssi_reconnect.set_value_pos(Gtk::PositionType::RIGHT);
   m_layout.attach(m_rssi_reconnect_label, 0, row);
   m_layout.attach(m_rssi_reconnect, 1, row);
   ++row;

   m_left.set_hexpand();
   m_right.set_hexpand();
   m_layout.attach(m_left, 0, row);
   m_layout.attach(m_right, 1, row);
   ++row;

   m_layout.set_column_spacing(6);
   m_layout.set_row_spacing(6);
   m_layout.set_expand();
   
   set_child(m_layout);


   auto OnDeviceUpdated = [this](uint64_t id, asha::Device& device)
   {
      SetId(id);
      m_pw_name.set_label(device.Name());
      auto* left = device.Left();
      if (left)
         left->SetUpdateCallback([this, left]{ m_left.UpdateDevice(left); });

      auto* right = device.Right();
      if (right)
         right->SetUpdateCallback([this, right]{ m_right.UpdateDevice(right); });

      m_left.UpdateDevice(left);
      m_right.UpdateDevice(right);
   };

   // TODO: making the assumption here that there will only ever be one hearing
   //       device, even though the Asha class does not make that assumption.
   m_asha.SetDeviceAddedCallback(OnDeviceUpdated);
   m_asha.SetDeviceUpdatedCallback(OnDeviceUpdated);
   m_asha.SetDeviceRemovedCallback([this](uint64_t id) {
      m_hisync.set_label("");
      m_pw_name.set_label("");
      m_left.UpdateDevice(nullptr);
      m_right.UpdateDevice(nullptr);
   });
}

MainWindow::~MainWindow()
{

}

void MainWindow::SetId(uint64_t id)
{
   std::stringstream ss;

   ss << "0x" << std::hex << std::setw(16) << std::setfill('0') << id;
   m_hisync.set_label(ss.str());
}

void MainWindow::OnReconnectRssiChanged()
{
   m_monitor.SetRssiPaired(m_rssi_reconnect.get_value());
}