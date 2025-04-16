#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <thread>

#include "asha/Bluetooth.hh"
#include "asha/BluetoothMonitor.hh"
#include "asha/Buffer.hh"
#include "asha/Device.hh"
#include "asha/Side.hh"
#include "asha/Config.hh"

#include <alsa/asoundlib.h>
#include <glib-2.0/glib.h>
#include <glib-2.0/glib-unix.h>



// TODO: Something fancier without ringing artifacts?
// gcc should be able to unroll this entire loop to simple integer arithmetic,
// but according to godbolt, it really doesn't. We should probably profile this
// code.
// inline void Resample44100(size_t stride, const int16_t* in, int16_t* out)
// {
//    // 1 to 2.75625. (441:160).
//    constexpr size_t SAMPLES_IN = 882;
//    for (size_t i = 0; i < RawS16::SAMPLE_COUNT; ++i)
//    {
//       const size_t j0 = i * SAMPLES_IN / RawS16::SAMPLE_COUNT;
//       const size_t j1 = (i + 1) * SAMPLES_IN / RawS16::SAMPLE_COUNT;
//       int32_t sample = 0;
//       for (size_t j = j0; j < j1; ++j)
//          sample += in[j];
//       out[i] = sample / (j1 - j0);
//    }
// }

inline void Resample48000(size_t stride, const int16_t* in, int16_t* out)
{
   // 1 to 3. Simple.
   size_t j = 0;
   for (size_t i = 0; i < RawS16::SAMPLE_COUNT; ++i)
   {
      int32_t sample = 0;
      sample += in[j]; j+= stride;
      sample += in[j]; j+= stride;
      sample += in[j]; j+= stride;
      out[i] = sample / 3;
   }
}

inline void Resample32000(size_t stride, const int16_t* in, int16_t* out)
{
   // 1:2
   size_t j = 0;
   for (size_t i = 0; i < RawS16::SAMPLE_COUNT; ++i)
   {
      int32_t sample = 0;
      sample += in[j]; j+= stride;
      sample += in[j]; j+= stride;
      out[i] = sample / 2;
   }
}

inline void Resample16000(size_t stride, const int16_t* in, int16_t* out)
{
   // 1:1 :P
   size_t j = 0;
   for (size_t i = 0; i < RawS16::SAMPLE_COUNT; ++i)
   {
      out[i] = in[j];
      j += stride;
   }
}


class CapturePlay
{
public:
   CapturePlay():
      m_b(
         [this](const asha::Bluetooth::BluezDevice& d) { OnAddSide(d); },
         [this](const std::string& p) { OnRemoveDevice(p); }
      ),
      m_device(new asha::Device("LineInPlay"))
   {
      std::string device_name = asha::Config::Extra("device");
      if (device_name.empty())
         device_name = "default";
      snd_pcm_t* handle;
      int err = snd_pcm_open(&handle, device_name.c_str(), SND_PCM_STREAM_CAPTURE, 0);
      if (err < 0)
      {
         std::cerr << "Cannot open audio device: " << snd_strerror(err) << '\n';
         throw std::runtime_error(std::string("Cannot open audio device: ") + snd_strerror(err));
      }
      m_pcm.reset(handle,  snd_pcm_close);
      SelectFormat();
      g_info("Capturing from %s %s at %d hz", device_name.c_str(), m_channels == 2 ? "stereo" : "mono", m_sample_rate);
   }

   ~CapturePlay()
   {
      Stop();
   }

   bool Start()
   {
      if (!m_running)
      {
         m_running = true;
         m_thread = std::thread(&CapturePlay::FeederThread, this);
      }

      return true;
   }

   void Stop()
   {
      if (m_running)
      {
         m_running = false;
         m_thread.join();
      }
   }

   // Direct hardware sources listed by alsa. This will include stuff that it
   // thinks is not available, but will not include plughw-based sources that
   // will resample audio.
   static void EnumerateHardwareSources(std::vector<std::pair<std::string, std::string>>& ret)
   {
      snd_ctl_card_info_t* info;
      snd_pcm_info_t* pcminfo;
      snd_ctl_card_info_alloca(&info);
      snd_pcm_info_alloca(&pcminfo);

      for (int card_idx = -1; snd_card_next(&card_idx) == 0 && card_idx != -1; )
      {
         std::string card_name = "hw:" + std::to_string(card_idx);

         snd_ctl_t *handle;
         std::shared_ptr<snd_ctl_t> card;
         int err;
         if ((err = snd_ctl_open(&handle, card_name.c_str(), 0)) < 0)
         {
            std::cerr << "Unable to open " << card_name << ": " << snd_strerror(err) << '\n';
            continue;
         }
         card.reset(handle, snd_ctl_close);
         if ((err = snd_ctl_card_info(handle, info)) < 0)
         {
            std::cerr << "Unable to retrieve info from " << card_name << ": " << snd_strerror(err) << '\n';
            continue;
         }
         for (int dev_idx = -1; snd_ctl_pcm_next_device(handle, &dev_idx) == 0 && dev_idx != -1;)
         {
            snd_pcm_info_set_device(pcminfo, dev_idx);
            snd_pcm_info_set_subdevice(pcminfo, 0);
            snd_pcm_info_set_stream(pcminfo, SND_PCM_STREAM_CAPTURE);
            if ((err = snd_ctl_pcm_info(handle, pcminfo)) < 0)
            {
               continue;
            }

            // std::stringstream ss;
            // ss << card_name << "," << dev_idx << "\t"
            //    << snd_ctl_card_info_get_name(info) << " "
            //    << snd_pcm_info_get_name(pcminfo);
            ret.emplace_back(card_name + "," + std::to_string(dev_idx), std::string(snd_ctl_card_info_get_name(info)) + ", " + snd_pcm_info_get_name(pcminfo));
         }
      }
   }

   // Includs direct hardware, but limited by what pulse/pipewire thinks is
   // enabled.
   static void EnumeratePCMSources(std::vector<std::pair<std::string, std::string>>& ret)
   {
      int idx = -1;
      struct CaptureDevice
      {
         std::string card;
         std::string description;
      };
      while (snd_card_next(&idx) == 0 && idx != -1)
      {
         std::string card_name;
         char* s = nullptr;
         if (0 == snd_card_get_name(idx, &s))
         {
            card_name = s;
            free(s);
         }
         std::string card_long_name;
         if (0 == snd_card_get_longname(idx, &s))
         {
            card_long_name = s;
            free(s);
         }
         void** hints = nullptr;
         if (0 == snd_device_name_hint(idx, "pcm", &hints))
         {
            for (size_t hint_idx = 0; hints[hint_idx]; ++hint_idx)
            {
               std::string name;
               std::string desc;
               std::string ioid;
               char* s = snd_device_name_get_hint(hints[hint_idx], "NAME");
               if (s)
               {
                  name = s;
                  free(s);
               }
               s = snd_device_name_get_hint(hints[hint_idx], "DESC");
               if (s)
               {
                  // "USB 2.0 Camera, USB Audio\nDefault Audio Device"
                  // We only want the first part, so remove the newline
                  for (char *c = s; *c; ++c) if (*c == '\n') *c = '\0';
                  desc = s;
                  free(s);
               }
               s = snd_device_name_get_hint(hints[hint_idx], "IOID");
               if (s)
               {
                  ioid = s;
                  free(s);
               }
               // We want input sources, using either plugins or direct hardware access
               if (ioid == "Input" && ((name.size() > 7 && name.substr(0, 7) == "plughw:") || (name.size() > 3 && name.substr(0, 3) == "hw:")))
               {
                  ret.emplace_back(name, desc);
               }
            }
            snd_device_name_free_hint(hints);
         }
      }
   }

protected:
   void SelectFormat()
   {
      // TODO: noninterleaved data would be more efficient... if it were the right block size and frame rate.
      // snd_pcm_access_t ACCESS_TYPES[] = {SND_PCM_ACCESS_RW_NONINTERLEAVED, SND_PCM_ACCESS_RW_INTERLEAVED};
      uint32_t SAMPLE_RATES[] = {16000, 32000, 48000 /*, 44100 Don't want to support this if I don't have to. */};

      for (uint32_t channel_count = 2; channel_count > 0; --channel_count)
      {
         for (uint32_t sample_rate_idx = 0; sample_rate_idx < (sizeof(SAMPLE_RATES)/sizeof(*SAMPLE_RATES)); ++sample_rate_idx)
         {
            int err = snd_pcm_set_params(m_pcm.get(),
               SND_PCM_FORMAT_S16_LE,
               SND_PCM_ACCESS_RW_INTERLEAVED,
               channel_count,
               SAMPLE_RATES[sample_rate_idx],
               true,
               SAMPLE_RATES[sample_rate_idx] * 20 / 1000 // We preferably get 20ms of data at a time.
            );
            if (err == 0)
            {
               m_sample_rate = SAMPLE_RATES[sample_rate_idx];
               m_channels = channel_count;
               return;
            }
         }
      }
      throw std::runtime_error("Unable to select suitable input parameters.");
   }


   void OnAddSide(const asha::Bluetooth::BluezDevice& d)
   {
      // Called when we get a new device.
      auto side = asha::Side::CreateIfValid(d);
      if (side)
      {
         g_info("Adding %s", d.path.c_str());

         std::weak_ptr<asha::Side> weak_side = side;
         std::string path = d.path;
         m_sides.emplace(path, side);

         // TODO: Do we need to handle a timeout in case the device never becomes
         //       ready? For now, I'm assuming that a timeout means a missing
         //       device, and bluez will call OnRemoveDevice later.
         side->SetOnConnectionReady([this, path, weak_side](){
            auto side = weak_side.lock();
            if (side)
            {
               SideReady(path, side);
            }
         });
      }
      else
      {
         g_info("%s is not an asha-enabled device", d.name.c_str());
      }
   }

   void SideReady(const std::string& path, const std::shared_ptr<asha::Side>& side)
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

      m_device->AddSide(path, side);
   }
   void OnRemoveDevice(const std::string& path)
   {
      auto it = m_sides.find(path);
      if (it != m_sides.end())
      {
         g_info("Removing %s", it->second->Description().c_str());
         m_sides.erase(it);
         m_device->RemoveSide(path);
      }
   }

   void FeederThread()
   {
      auto buffer = asha::Buffer::Create([this](const RawS16& s) { return m_device->SendAudio(s); });

      const size_t SAMPLE_COUNT = m_sample_rate * 20 / 1000;
      int16_t frames[SAMPLE_COUNT * 2];

      size_t frames_to_read = SAMPLE_COUNT;
      size_t frames_already_read = 0;
      size_t prepared_sample_count = 0;
      RawS16 prepared_samples{};

      while (m_running)
      {
         snd_pcm_sframes_t count = snd_pcm_readi(m_pcm.get(), frames + frames_already_read * 2, frames_to_read - frames_already_read);
         if (count < 0)
         {
            // -EBADFD: stream needs re-allocated?
            // -EPIPE: we were too slow and it had to drop frames.
            // -ESTRPIPE: stream is suspended
            // I'm not sure how to handle these... probably just need to close
            // and reopen the stream.
            g_warning("snd_pcm_readi returned %ld... exiting Feeder Thread", count);
            break;
         }
         else if (count + frames_already_read < SAMPLE_COUNT)
         {
            frames_already_read += count;
            continue;
         }

         const int16_t* left = frames;
         const int16_t* right = left + 1;
         size_t stride = 2;

         if (m_channels == 1)
         {
            // Duplicate to send stereo
            right = left;
            stride = 1;
         }
         else
         {
            right = left + 1;
            stride = 2;
         }

         RawS16* next = buffer->NextBuffer();

         // TODO: better resampling method? For now, keeping it dead simple.
         if (m_sample_rate == 16000)
         {
            Resample16000(stride, left, next->l);
            Resample16000(stride, right, next->r);
         }
         else if (m_sample_rate == (32000))
         {
            Resample32000(stride, left, next->l);
            Resample32000(stride, right, next->r);
         }
         else if (m_sample_rate == (48000))
         {
            Resample48000(stride, left, next->l);
            Resample48000(stride, right, next->r);
         }
         // else if (m_smample_rate == (44100))
         // else This shouldn't be possible.
         buffer->SendBuffer();
      }
   }

private:
   std::map<std::string, std::shared_ptr<asha::Side>> m_sides;

   std::shared_ptr<snd_pcm_t> m_pcm;
   bool m_interleaved = false;
   uint32_t m_sample_rate = 0;
   uint32_t m_channels = 0;

   volatile bool m_running = false;
   std::thread m_thread;

   std::shared_ptr<asha::Device> m_device;
   asha::Bluetooth m_b; // needs to be last
};


int main(int argc, char** argv)
{
   std::vector<std::pair<std::string, std::string>> sources;
   // CapturePlay::EnumerateHardwareSources(sources);
   CapturePlay::EnumeratePCMSources(sources);

   asha::Config::SetHelpDescription("Capture audio from alsa and stream it to a hearing device.");
   asha::Config::AddExtraStringOption("device", "Alsa device to read audio from [Default: default]");
   asha::Config::AddExtraStringOption("list", "List all available inputs, then quit.");
   asha::Config::ReadArgs(argc, argv);

   setenv("G_MESSAGES_DEBUG", "all", false);

   if (asha::Config::ExtraBool("list"))
   {
      sources.emplace_back("default", "The system defined default capture source");
      for (auto& s: sources)
      {
         std::cout << std::setw(30) << std::left << s.first << " " << s.second << '\n';
      }
      return 0;
   }

   std::shared_ptr<GMainLoop> loop(g_main_loop_new(nullptr, true), g_main_loop_unref);
   auto quitter = g_unix_signal_add(SIGINT, [](void* ml) {
      g_main_loop_quit((GMainLoop*)ml);
      return (int)G_SOURCE_CONTINUE;
   }, loop.get());

   CapturePlay cp;
   asha::BluetoothMonitor bm;

   cp.Start();

   g_main_loop_run(loop.get());
   g_source_remove(quitter);

   std::cout << "Stopping...\n";
}