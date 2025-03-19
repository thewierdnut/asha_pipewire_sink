#pragma once

#include "../asha/Side.hh"

#include <memory>

#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/scale.h>
#include <gtkmm/gestureclick.h>


class DeviceWidget: public Gtk::Grid
{
public:
   DeviceWidget();
   virtual ~DeviceWidget();

   void UpdateDevice(asha::Side* s);

protected:
   void OnStreamVolumeChanged();
   void OnMicrophoneVolumeChanged();

private:
   Gtk::Label m_name_label;
   Gtk::Label m_name;

   Gtk::Label m_battery_label;
   Gtk::Label m_battery;

   Gtk::Label m_volume_label;
   Gtk::Scale m_volume;

   Gtk::Label m_microphone_label;
   Gtk::Scale m_microphone;

   asha::Side* m_side = nullptr;
   
   // If you send events as fast as the slider is dragging, then about half of
   // them will be rejected. Instead, we want to delay them about 250ms or so,
   // so that they are aggregated.
   class PendingEvent: public sigc::trackable
   {
   public:
      void Post(std::function<void()> event);

   private:
      void Emit();

      uint32_t m_previous = 0;
      bool m_pending = false;
      static const uint32_t DELAY = 500; // ms

      std::function<void()> m_event;
   };

   PendingEvent m_volume_throttle;
   PendingEvent m_microphone_throttle;
};