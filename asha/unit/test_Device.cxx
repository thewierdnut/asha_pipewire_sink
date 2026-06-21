#include "unit_test.hh"

#include "MockSide.hh"
#include "../Device.hh"
#include <cassert>

using namespace asha;

class MockDevice: public Device
{

};

class test_Device
{
public:
   test_Device():
      m_d(new Device("MockDevice")),
      m_left(new MockSide),
      m_right(new MockSide)
   {
      m_left->SetProps(true, HISYNC);
      m_right->SetProps(false, HISYNC);
   }

   ~test_Device()
   {
      m_d->RemoveSide(LEFT);
      m_d->RemoveSide(RIGHT);
   }

   // Stably progress the device to the indicated state.
   void InitToState(Device::AudioState state, bool both)
   {
      // Note that sides are not passed to the Device class until they have
      // already transitioned to the STOPPED state.
      m_d->RemoveSide(LEFT);
      m_d->RemoveSide(RIGHT);
      m_left->Reset();
      m_right->Reset();
      ASSERT_TRUE(m_d->State() == Device::UNINITIALIZED) << "Could not transition to UNINITIALIZED";
      ASSERT_TRUE(m_left->State() == Side::STOPPED);
      ASSERT_TRUE(m_right->State() == Side::STOPPED);
      if (state == Device::UNINITIALIZED) return;

      m_d->AddSide(LEFT, m_left);
      ASSERT_TRUE(m_left->State() == Side::STOPPED);
      if (both)
      {
         m_d->AddSide(RIGHT, m_right);
         ASSERT_TRUE(m_right->State() == Side::STOPPED);
      }
      ASSERT_TRUE(m_d->State() == Device::STOPPED) << "Could not transition to STOPPED";
      if (state == Device::STOPPED) return;

      m_d->StreamStart();
      ASSERT_TRUE(m_d->State() == Device::START_STREAMING) << "Could not transition to START_STREAMING";
      if (state == Device::START_STREAMING) return;

      ASSERT_TRUE(m_left->Called(MockSide::START));
      ASSERT_TRUE(both == m_left->Arg(MockSide::START));
      m_left->FinishCall(MockSide::START, true);
      ASSERT_TRUE(m_left->State() == Side::STREAMING);
      if (both)
      {
         ASSERT_TRUE(m_left->Called(MockSide::START));
         // ASSERT_TRUE(false == m_left->Arg(MockSide::START)); // Don't know if this should be true or false.
         m_right->FinishCall(MockSide::START, true);
         ASSERT_TRUE(m_right->State() == Side::STREAMING);
      }
      ASSERT_TRUE(m_d->State() == Device::STREAMING) << "Could not transition to STREAMING";
      if (state == Device::STREAMING) return;

      assert(0 && "You forgot to add a state.");
   }

   void test_Init()
   {
      ASSERT_TRUE(m_d->State() == Device::UNINITIALIZED);
      ASSERT_TRUE(m_d->SideCount() == 0);
      ASSERT_TRUE(m_d->Name() == "MockDevice");
   }

   void test_RemoveAll(Device::AudioState state, bool both)
   {
      InitToState(state, both);
      m_d->RemoveSide(LEFT);
      m_d->RemoveSide(RIGHT);
      ASSERT_TRUE(m_d->State() == Device::UNINITIALIZED)
         << "state: " << state << " both: " << both;
   }

   void test_RemoveOneInit()
   {
      InitToState(Device::STOPPED, true);
      m_d->RemoveSide(RIGHT);

      // Left should have been notified.
      ASSERT_TRUE(m_left->Called(MockSide::OTHER));
      ASSERT_TRUE(false == m_left->Arg(MockSide::OTHER));

      // Shouldn't modify the state (We haven't started yet)
      ASSERT_TRUE(m_d->State() == Device::STOPPED);
   }

   void test_RemoveOneStopped()
   {
      InitToState(Device::STOPPED, true);
      m_d->RemoveSide(RIGHT);

      // Left should have been notified.
      ASSERT_TRUE(m_left->Called(MockSide::OTHER));
      ASSERT_TRUE(false == m_left->Arg(MockSide::OTHER));

      // Shouldn't modify the state
      ASSERT_TRUE(m_d->State() == Device::STOPPED);
   }

   void test_RemoveOneStreaming()
   {
      InitToState(Device::STREAMING, true);
      m_d->RemoveSide(RIGHT);

      // Left should have been notified.
      ASSERT_TRUE(m_left->Called(MockSide::OTHER));
      ASSERT_TRUE(false == m_left->Arg(MockSide::OTHER));

      // This should stop and restart the stream.
      ASSERT_TRUE(m_left->Called(MockSide::STOP));

      // Already streaming, if we remove a side, it should stop and restart.
      ASSERT_TRUE(m_d->State() == Device::START_STREAMING);         // ie, waiting for sides to enter stopped
      ASSERT_TRUE(m_left->State() == Side::WAITING_FOR_STOP);
      m_left->FinishCall(MockSide::STOP, true);                // Stop only remaining side.
      ASSERT_TRUE(m_d->State() == Device::START_STREAMING);
      ASSERT_TRUE(m_left->Called(MockSide::START));            // It should have attempted to restart.
      ASSERT_TRUE(false == m_left->Arg(MockSide::START));
      ASSERT_TRUE(m_left->State() == Side::WAITING_FOR_STREAM);
      m_left->FinishCall(MockSide::START, true);
      ASSERT_TRUE(m_d->State() == Device::STREAMING);
   }

   void test_AddOneInit()
   {
      InitToState(Device::UNINITIALIZED, false);
      m_d->AddSide(RIGHT, m_right);
      // Shouldn't modify the state (We haven't started yet)
      ASSERT_TRUE(m_d->State() == Device::STOPPED);
      ASSERT_TRUE(m_right->State() == asha::Side::STOPPED);
   }

   void test_AddOneStopped()
   {
      InitToState(Device::STOPPED, false);
      m_d->AddSide(RIGHT, m_right);
      
      // Should remain in STOPPED, since we never started yet.
      ASSERT_TRUE(m_d->State() == Device::STOPPED);
      ASSERT_TRUE(m_right->State() == asha::Side::STOPPED);
      ASSERT_TRUE(m_left->State() == asha::Side::STOPPED);
   }

   void test_AddOneStreaming()
   {
      InitToState(Device::STREAMING, false);
      m_d->AddSide(RIGHT, m_right);

      // Already streaming, if we add a side, it should stop and restart.
      ASSERT_TRUE(m_left->Called(MockSide::STOP));
      ASSERT_TRUE(m_d->State() == Device::START_STREAMING);
      ASSERT_TRUE(m_left->State() == Side::WAITING_FOR_STOP);
      ASSERT_TRUE(m_right->State() == Side::STOPPED);
      m_left->FinishCall(MockSide::STOP, true);

      // Once the right side has finished connecting, the device should
      // request that they both start again.
      ASSERT_TRUE(m_left->Called(MockSide::START));
      ASSERT_TRUE(m_right->Called(MockSide::START));
      ASSERT_TRUE(m_d->State() == Device::START_STREAMING);
      ASSERT_TRUE(m_left->State() == Side::WAITING_FOR_STREAM);
      ASSERT_TRUE(m_right->State() == Side::WAITING_FOR_STREAM);

      // Once both sides are ready, it should transition to STREAMING
      // TODO: I haven't figured out to do with start::otherstate here.
      //       Is it supposed to be true if we are connected? Or only if
      //       the other side has been sent a start command? Or should
      //       the other side have acknowledged it first?
      // TODO: Also... if acp start has been sent with otherstate false
      //       on both of them, should we send a status other side
      //       connected? This unit test should be filled in with the
      //       correct assumptions if we ever figure it out.
      m_left->FinishCall(MockSide::START, true);
      ASSERT_TRUE(m_left->State() == Side::STREAMING);
      ASSERT_TRUE(m_d->State() == Device::START_STREAMING);
      m_right->FinishCall(MockSide::START, true);
      ASSERT_TRUE(m_right->State() == Side::STREAMING);
      ASSERT_TRUE(m_d->State() == Device::STREAMING);
   }

   void test_FailOneStreamStart()
   {
      InitToState(Device::START_STREAMING, false);

      // On start, if a device fails, it should retry to reinitialize it.
      m_left->ClearCalls();
      m_left->FinishCall(MockSide::START, false);
      ASSERT_TRUE(m_left->Called(asha::MockSide::START));
      ASSERT_TRUE(m_left->State() == MockSide::WAITING_FOR_STREAM);
      ASSERT_TRUE(m_d->State() == Device::START_STREAMING);

      // Allow it to finish this time, and verify that it made it to streaming.
      m_left->FinishCall(MockSide::START, true);
      ASSERT_TRUE(m_left->State() == MockSide::STREAMING);
      ASSERT_TRUE(m_d->State() == Device::STREAMING);
   }

   void test_FailTwoStreamStart()
   {
      InitToState(Device::START_STREAMING, true);
      m_left->ClearCalls();
      m_right->ClearCalls();

      // Allow left to finish, but fail right.
      m_left->FinishCall(MockSide::START, true);
      m_right->FinishCall(MockSide::START, false);
      ASSERT_TRUE(m_left->State() == MockSide::STREAMING);
      ASSERT_TRUE(!m_left->Called(MockSide::START));
      ASSERT_TRUE(m_right->Called(MockSide::START));
      ASSERT_TRUE(m_right->State() == MockSide::WAITING_FOR_STREAM);
      ASSERT_TRUE(m_d->State() == Device::START_STREAMING);

      // Allow it to finish this time, and verify that it made it to streaming.
      m_right->FinishCall(MockSide::START, true);
      ASSERT_TRUE(m_right->State() == MockSide::STREAMING);
      ASSERT_TRUE(m_d->State() == Device::STREAMING);
   }

   void test_StopStartSingle()
   {
      InitToState(Device::STREAMING, false);

      m_d->StreamStop();
      ASSERT_TRUE(m_d->State() == Device::UNINITIALIZED);
      ASSERT_TRUE(m_left->State() == Side::WAITING_FOR_STOP);
      ASSERT_TRUE(m_left->Called(asha::MockSide::STOP));
      m_left->FinishCall(asha::MockSide::STOP, true);

      ASSERT_TRUE(m_d->State() == Device::STOPPED);

      m_d->StreamStart();
      ASSERT_TRUE(m_d->State() == Device::START_STREAMING);
      ASSERT_TRUE(m_left->State() == Side::WAITING_FOR_STREAM);
      m_left->FinishCall(asha::MockSide::START, true);

      ASSERT_TRUE(m_d->State() == Device::STREAMING);
   }

   void test_StopStartBoth()
   {
      InitToState(Device::STREAMING, true);

      m_d->StreamStop();
      ASSERT_TRUE(m_d->State() == Device::UNINITIALIZED);
      ASSERT_TRUE(m_left->State() == Side::WAITING_FOR_STOP);
      ASSERT_TRUE(m_right->State() == Side::WAITING_FOR_STOP);
      ASSERT_TRUE(m_left->Called(asha::MockSide::STOP));
      ASSERT_TRUE(m_right->Called(asha::MockSide::STOP));
      m_left->FinishCall(asha::MockSide::STOP, true);
      ASSERT_TRUE(m_d->State() == Device::UNINITIALIZED);
      m_right->FinishCall(asha::MockSide::STOP, true);
      ASSERT_TRUE(m_d->State() == Device::STOPPED);

      m_d->StreamStart();
      ASSERT_TRUE(m_d->State() == Device::START_STREAMING);
      ASSERT_TRUE(m_left->State() == Side::WAITING_FOR_STREAM);
      ASSERT_TRUE(m_right->State() == Side::WAITING_FOR_STREAM);
      ASSERT_TRUE(m_left->Called(asha::MockSide::START));
      ASSERT_TRUE(m_right->Called(asha::MockSide::START));
      m_left->FinishCall(asha::MockSide::START, true);
      ASSERT_TRUE(m_d->State() == Device::START_STREAMING);
      m_right->FinishCall(asha::MockSide::START, true);

      ASSERT_TRUE(m_d->State() == Device::STREAMING);
   }

private:
   static constexpr uint64_t HISYNC = 1234;
   static const std::string LEFT;
   static const std::string RIGHT;
   std::shared_ptr<Device> m_d;
   std::shared_ptr<MockSide> m_left;
   std::shared_ptr<MockSide> m_right;
};

const std::string test_Device::LEFT = "left";
const std::string test_Device::RIGHT = "right";


int main()
{
   setenv("G_MESSAGES_DEBUG", "all", false);

   test_Device().test_Init();
   test_Device().test_RemoveAll(Device::UNINITIALIZED, false);
   test_Device().test_RemoveAll(Device::STOPPED, false);
   test_Device().test_RemoveAll(Device::STREAMING, false);
   test_Device().test_RemoveAll(Device::UNINITIALIZED, true);
   test_Device().test_RemoveAll(Device::STOPPED, true);
   test_Device().test_RemoveAll(Device::STREAMING, true);

   test_Device().test_RemoveOneInit();
   test_Device().test_RemoveOneStreaming();
   test_Device().test_AddOneInit();
   test_Device().test_AddOneStopped();
   test_Device().test_AddOneStreaming();

   test_Device().test_FailOneStreamStart();
   test_Device().test_FailTwoStreamStart();

   test_Device().test_StopStartSingle();
   test_Device().test_StopStartBoth();

   std::cout << "All test passed\n";

   return 0;
}