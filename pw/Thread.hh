#pragma once

#include <memory>

struct pw_thread_loop;
struct pw_context;
struct pw_core;

namespace pw
{

class Stream;

// Wrap a pipewire thread
class Thread final
{
public:
   static std::shared_ptr<Thread> Get();

   struct pw_core* Core() { return m_core.get(); }

   // std::shared_ptr<Stream> AddStream(const std::string& name, const std::string& alias);

   class LoopLock
   {
   public:
      LoopLock(const std::shared_ptr<struct pw_thread_loop>& tl);
      LoopLock(LoopLock&& l);
      LoopLock(const LoopLock& l) = delete;
      ~LoopLock();

   private:
      std::shared_ptr<Thread> m_thread; // So that we don't delete the thread while a lock is held.
      pw_thread_loop* m_thread_loop;
   };
   LoopLock Lock();

protected:
   Thread();
   ~Thread();

   static void Deref();

   void Start();
   void Stop();

private:
   std::shared_ptr<void> m_init_guard;
   std::shared_ptr<struct pw_thread_loop> m_thread_loop;
   std::shared_ptr<struct pw_context> m_context;
   std::shared_ptr<struct pw_core> m_core;

   static Thread* s_instance;
   static int s_ref;
};


}