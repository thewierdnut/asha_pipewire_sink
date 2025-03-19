#include "../Side.hh"


namespace asha
{

class MockSide: public Side
{
public:
   MockSide(): Side("MockSide") {}
   using Side::SetProps;
   using Side::SetState;
   using Side::OnStatusNotify;

   virtual void SetStreamVolume(int8_t volume) {}
   virtual void SetMicrophoneVolume(uint8_t volume) {}
   
   virtual bool Start(bool otherstate, std::function<void(bool)> OnDone)
   {
      SetState(WAITING_FOR_READY);
      return LogCall(START, otherstate, [this, OnDone](bool a) {
         // Normally, we would wait for bluez to respond when the status
         // characteristic sends a notification, but here, lets just set
         // it to ready.
         SetState(a ? READY : STOPPED);
         OnDone(a);
      });
   }
   virtual bool Stop(std::function<void(bool)> OnDone)
   {
      SetState(WAITING_FOR_STOP);
      return LogCall(STOP, 0, [this, OnDone](bool a) {
         SetState(a ? STOPPED : STOPPED);
         OnDone(a);
      });
   }
   virtual WriteStatus WriteAudioFrame(const AudioPacket& packet)
   {
      m_last_audio_seq = packet.seq;
      return WRITE_OK;
   }
   virtual bool UpdateOtherConnected(bool connected) { return LogCall(OTHER, connected); }
   virtual bool UpdateConnectionParameters(uint8_t interval) { return LogCall(PARAM, interval); }

   enum Call { START, STOP, OTHER, PARAM, CALL_COUNT };
   void Reset()
   {
      for (size_t i = START; i < CALL_COUNT; ++i)
         m_call[i] = CallInfo{};
      SetState(STOPPED);
   }
   void ClearCalls()
   {
      for (size_t i = START; i < CALL_COUNT; ++i)
         m_call[i].called = false;
   }
   bool Called(Call c) { return m_call[c].called; }
   bool Arg(Call c) { return m_call[c].arg; }
   void FinishCall(Call c, bool status) { return m_call[c].finish(status); }

protected:
   bool LogCall(Call c, std::function<void(bool)> f)
   {
      m_call[c].called = true;
      m_call[c].finish = f;
      return true;
   }
   template <typename T>
   bool LogCall(Call c, const T& v, std::function<void(bool)> f = {})
   {
      m_call[c].called = true;
      m_call[c].arg = v;
      m_call[c].finish = f;
      return true;
   }

private:
   uint16_t m_last_audio_seq = 0;

   struct CallInfo
   {
      bool called = false;
      uint8_t arg = false;
      std::function<void(bool)>finish;
   };

   CallInfo m_call[CALL_COUNT];
};

}