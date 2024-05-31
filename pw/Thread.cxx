#include "Thread.hh"
#include "Stream.hh"

#include <pipewire/pipewire.h>

using namespace pw;

Thread* Thread::s_instance = nullptr;
int Thread::s_ref = 0;

std::shared_ptr<Thread> Thread::Get()
{
   if (!s_instance)
      s_instance = new Thread;
   ++s_ref;
   return std::shared_ptr<Thread>(s_instance, [](Thread*) { Deref(); });
}

void Thread::Deref()
{
   --s_ref;
   if (s_ref <= 0)
   {
      if (s_instance)
      {
         delete s_instance;
         s_instance = nullptr;
      }
      s_ref = 0;
   }
}


Thread::Thread()
{
   pw_init(nullptr, nullptr);
   m_init_guard.reset((void*)nullptr, [](void*) { pw_deinit(); });
   m_thread_loop.reset(pw_thread_loop_new("asha pw thread", nullptr), pw_thread_loop_destroy);
   m_context.reset(pw_context_new(pw_thread_loop_get_loop(m_thread_loop.get()), nullptr, 0), pw_context_destroy);
   m_core.reset(pw_context_connect(m_context.get(), NULL, 0), pw_core_disconnect);
   Start();
}


Thread::~Thread()
{
   Stop(); // Ok if thread is already stopped.
}


void Thread::Start()
{
   pw_thread_loop_start(m_thread_loop.get());
}


void Thread::Stop()
{
   pw_thread_loop_stop(m_thread_loop.get());
}


// std::shared_ptr<Stream> Thread::AddStream(const std::string& name, const std::string& alias)
// {
//    // This needs created and deleted with the lock held.
//    auto lock = Lock();
//    return std::shared_ptr<Stream>(
//       new Stream(m_core.get(), name, alias),
//       [this] (Stream* d) {
//          auto lock = Lock();
//          delete d;
//       }
//    );
// }


Thread::LoopLock Thread::Lock()
{
   return LoopLock(m_thread_loop);
}


Thread::LoopLock::LoopLock(const std::shared_ptr<struct pw_thread_loop>& tl)
{
   m_thread = Get();
   m_thread_loop = tl.get();
   pw_thread_loop_lock(tl.get());
}


Thread::LoopLock::LoopLock(LoopLock&& o)
{
   std::swap(m_thread_loop, o.m_thread_loop);
}


Thread::LoopLock::~LoopLock()
{
   pw_thread_loop_unlock(m_thread_loop);
}
