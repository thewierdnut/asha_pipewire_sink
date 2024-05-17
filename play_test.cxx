#include "asha/Asha.hh"

#include <glib-2.0/glib.h>
#include <glib-2.0/glib-unix.h>

#include <cassert>
#include <fstream>
#include <iostream>
#include <memory>

std::vector<uint8_t> ReadFile(const std::string& path)
{
   std::vector<uint8_t> ret;
   std::ifstream in(path, std::ios::binary);
   if (!in)
   {
      std::cout << "Unable to read " << path << '\n';
      exit(1);
   }
   in.seekg(0, std::ios::end);
   ret.resize(in.tellg());
   in.seekg(0);
   in.read((char*)ret.data(), ret.size());
   return ret;
}


class AudioSource
{
public:
   static constexpr size_t BYTES_PER_20_MS = 160;

   AudioSource(asha::Asha& a):m_a(a)
   {
      m_left = ReadFile("test_audio_left.g722");
      m_right = ReadFile("test_audio_right.g722");

      g_return_if_fail(m_left.size() == m_right.size());
      g_return_if_fail(m_left.size() > BYTES_PER_20_MS);

      m_timer.reset(g_timer_new(), g_timer_destroy);
      g_timer_start(m_timer.get());
      m_sound_position = 0;
   }

   static gboolean OnTimer(gpointer user_data)
   {
      // The timing for this is horrifyingly bad. We need to track our own time
      // and catch up if we have fallen behind. We have room for 6 packets.
      auto* self = (AudioSource*)user_data;
      g_return_val_if_fail(self->m_left.size() == self->m_right.size(), G_SOURCE_REMOVE);
      g_return_val_if_fail(self->m_left.size() > BYTES_PER_20_MS, G_SOURCE_REMOVE);


      double delta = g_timer_elapsed(self->m_timer.get(), nullptr) - self->m_sound_position;

      while (delta > .02)
      {
         if (self->m_offset + BYTES_PER_20_MS > self->m_left.size())
            self->m_offset = 0;

         self->m_a.SendAudio(
            self->m_left.data() + self->m_offset,
            self->m_right.data() + self->m_offset,
            BYTES_PER_20_MS
         );

         self->m_offset += BYTES_PER_20_MS;
         self->m_sound_position += .02;
         delta -= .02;
      }

      
      // std::cout << "delta: " << delta << '\n';

      return G_SOURCE_CONTINUE;
   }

private:
   std::vector<uint8_t> m_left;
   std::vector<uint8_t> m_right;

   size_t m_offset = 0;

   asha::Asha& m_a;

   std::shared_ptr<GTimer> m_timer;

   double m_sound_position = 0;
};

int main()
{
   asha::Asha a;
   a.EnumerateDevices();
   uint64_t id = 0;
   for (auto& d: a.Devices())
   {
      std::cout << d.name << ": " << d.id << '\n';
      id = d.id;
   }

   a.SelectDevice(id);
   usleep(1000000);
   a.SetVolume(-120);

   // Send audio start
   AudioSource audio(a);
   a.Start();

   // Queue up two packets immediately.
   audio.OnTimer(&audio);
   audio.OnTimer(&audio);
   
   // Then start a timer to feed the rest of it.
   auto audio_timer = g_timeout_add(20, &AudioSource::OnTimer, &audio);
   
   std::shared_ptr<GMainLoop> loop(g_main_loop_new(nullptr, true), g_main_loop_unref);
   auto quitter = g_unix_signal_add(SIGINT, [](void* ml){
      g_main_loop_quit((GMainLoop*)ml);
      return G_SOURCE_REMOVE;
   }, loop.get());

   g_main_loop_run(loop.get());
   g_source_remove(audio_timer);
   g_source_remove(quitter);

   std::cout << "Stopping...\n";
   a.Stop();

   return 0;
}