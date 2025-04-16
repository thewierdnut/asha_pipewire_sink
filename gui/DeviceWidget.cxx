#include "DeviceWidget.hh"

#include <gtkmm.h>

uint32_t NowTicks()
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}


DeviceWidget::DeviceWidget()
{
   int row = 0;
   m_name_label.set_label("Name:");
   m_name_label.set_justify(Gtk::Justification::RIGHT);
   m_name_label.set_halign(Gtk::Align::END);
   attach(m_name_label, 0, row);
   m_name.set_label("");
   m_name.set_hexpand();
   attach(m_name, 1, row);
   ++row;

   m_battery_label.set_label("Battery:");
   m_battery_label.set_justify(Gtk::Justification::RIGHT);
   m_battery_label.set_halign(Gtk::Align::END);
   attach(m_battery_label, 0, row);
   m_battery.set_hexpand();
   attach(m_battery, 1, row);
   ++row;

   m_rssi_label.set_label("Rssi:");
   m_rssi_label.set_justify(Gtk::Justification::RIGHT);
   m_rssi_label.set_halign(Gtk::Align::END);
   attach(m_rssi_label, 0, row);
   m_rssi.set_hexpand();
   attach(m_rssi, 1, row);
   ++row;

   m_volume_label.set_label("Volume:");
   m_volume_label.set_justify(Gtk::Justification::RIGHT);
   m_volume_label.set_halign(Gtk::Align::END);
   attach(m_volume_label, 0, row);
   m_volume.set_orientation(Gtk::Orientation::HORIZONTAL);
   m_volume.set_range(-128, 0);
   m_volume.property_round_digits().set_value(0);
   m_volume.signal_value_changed().connect(sigc::mem_fun(*this, &DeviceWidget::OnStreamVolumeChanged), true);
   m_volume.set_hexpand();
   attach(m_volume, 1, row);
   ++row;

   m_microphone_label.set_label("Microphone:");
   m_microphone_label.set_justify(Gtk::Justification::RIGHT);
   m_microphone_label.set_halign(Gtk::Align::END);
   attach(m_microphone_label, 0, row);
   m_microphone.set_orientation(Gtk::Orientation::HORIZONTAL);
   m_microphone.set_range(0, 255);
   m_microphone.property_round_digits().set_value(0);
   m_microphone.signal_value_changed().connect(sigc::mem_fun(*this, &DeviceWidget::OnMicrophoneVolumeChanged), true);
   m_microphone.set_hexpand();
   attach(m_microphone, 1, row);
   ++row;

   // TODO: Hide this widget unless a device has been set?


}

DeviceWidget::~DeviceWidget()
{

}

void DeviceWidget::UpdateDevice(asha::Side* s)
{
   m_side = s;
   if (s)
   {
      m_name.set_label(s->Alias());
      m_battery.set_label(std::to_string(s->Battery()) + "%");
      m_rssi.set_label(std::to_string(s->Rssi()) + " dB");
      m_volume.set_value(s->StreamVolume());
      m_microphone.set_value(s->MicrophoneVolume());
   }
   else
   {
      m_name.set_label("");
      m_battery.set_label("");
      m_rssi.set_label("");
      m_volume.set_value(-128);
      m_microphone.set_value(0);
   }
}

void DeviceWidget::OnStreamVolumeChanged()
{
   g_info("OnStreamVolumeChanged()");
   int8_t volume = m_volume.get_value();
   m_volume_throttle.Post([this, volume]() {
      if (m_side)
         m_side->SetStreamVolume(volume);
   });
}

void DeviceWidget::OnMicrophoneVolumeChanged()
{
   g_info("OnMicrophoneVolumeChanged()");
   uint8_t volume = m_microphone.get_value();
   g_info("Posting volume %hhu", volume);
   m_microphone_throttle.Post([this, volume]() {
      if (m_side)
         m_side->SetMicrophoneVolume(volume);
   });
}

void DeviceWidget::PendingEvent::Post(std::function<void()> fn)
{
   uint32_t now = NowTicks();
   // If there is a pending call, clear it.
   m_event = std::function<void()>();
   if (now - m_previous > DELAY)
   {
      // Its been a long time since the previous post. Just post it now.
      fn();
      m_previous = now;
   }
   else
   {
      m_event = fn;
      if (!m_pending)
      {
         // Schedule a call in the future
         m_pending = true;
         Glib::signal_timeout().connect_once(sigc::mem_fun(*this, &DeviceWidget::PendingEvent::Emit), DELAY - (now - m_previous));
      }
      // else a call to Emit() is already scheduled. Just wait.
   }
}

void DeviceWidget::PendingEvent::Emit()
{
   if (m_event)
      m_event();
   m_pending = false;
   m_previous = NowTicks();
}