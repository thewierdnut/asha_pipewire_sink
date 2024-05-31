#pragma once

#include "Convert.hh"

#include "../asha/AudioPacket.hh"

#include <spa/param/audio/raw.h>
#include <spa/utils/hook.h>

#include <memory>
#include <functional>
#include <string>

struct spa_hook;
struct pw_core;
struct pw_stream;

namespace pw
{

class Thread;

// Wrap a pipewire stream object. This will show up in the list of visible
// pulseaudio/pipewire sinks that a user can select.
class Stream
{
public:
   typedef std::function<void(AudioPacket&, AudioPacket&)> DataCallback;
   typedef std::function<void(void)> EventCallback;

   Stream(const std::string& name, const std::string& alias,
      EventCallback on_connect,     // When the node is connected to the graph.
      EventCallback on_disconnect,  // When the node is disconnected from the graph.
      EventCallback on_start,       // When sound data will start playing.
      EventCallback on_stop,        // When sound data has stopped.
      DataCallback on_data          // Will get called with audio data.
   );
   ~Stream();

private:
   void Process();

   std::shared_ptr<Thread> m_thread;
   
   struct spa_audio_info_raw m_info{};

   // TODO: pw will destroy this, except when they don't. How to make sure it
   //       doesn't leak?
   struct pw_stream* m_stream = nullptr;
   struct spa_hook m_stream_listener{};

   EventCallback m_connect_cb;
   EventCallback m_disconnect_cb;
   EventCallback m_start_cb;
   EventCallback m_stop_cb;
   DataCallback m_data_cb;

   Convert m_encoder;

   // double m_prev_stamp = 0;
   // size_t m_count = 0;
};

}