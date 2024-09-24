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
      m_d->RemoveSide(LEFT);
      m_d->RemoveSide(RIGHT);
      m_left->Reset();
      m_right->Reset();
      ASSERT_TRUE(m_d->State() == Device::STOPPED) << "Could not transition to STOPPED";
      ASSERT_TRUE(m_left->State() == Side::STOPPED);
      ASSERT_TRUE(m_right->State() == Side::STOPPED);
      if (state == Device::STOPPED) return;

      m_d->AddSide(LEFT, m_left);
      ASSERT_TRUE(m_left->Called(MockSide::START));
      ASSERT_TRUE(m_left->State() == Side::WAITING_FOR_READY);
      if (both)
      {
         m_d->AddSide(RIGHT, m_right);
         ASSERT_TRUE(m_right->Called(MockSide::START));
         ASSERT_TRUE(m_right->State() == Side::WAITING_FOR_READY);
      }
      ASSERT_TRUE(m_d->State() == Device::STREAM_INIT) << "Could not transition to STREAM_INIT";
      if (state == Device::STREAM_INIT) return;

      m_left->FinishCall(MockSide::START, true);
      ASSERT_TRUE(m_left->State() == Side::READY);
      if (both)
      {
         m_right->FinishCall(MockSide::START, true);
         ASSERT_TRUE(m_right->State() == Side::READY);
      }
      ASSERT_TRUE(m_d->State() == Device::STREAMING) << "Could not transition to STREAMING";
      if (state == Device::STREAMING) return;

      assert(0 && "You forgot to add a state.");
   }

   void test_Init()
   {
      ASSERT_TRUE(m_d->State() == Device::STOPPED);
      ASSERT_TRUE(m_d->SideCount() == 0);
      ASSERT_TRUE(m_d->Name() == "MockDevice");
   }

   void test_RemoveAll(Device::AudioState state, bool both)
   {
      InitToState(state, both);
      m_d->RemoveSide(LEFT);
      m_d->RemoveSide(RIGHT);
      ASSERT_TRUE(m_d->State() == Device::STOPPED)
         << "state: " << state << " both: " << both;
   }

   void test_RemoveOneInit()
   {
      InitToState(Device::STREAM_INIT, true);
      m_d->RemoveSide(RIGHT);
      // Shouldn't modify the state (We haven't started yet)
      ASSERT_TRUE(m_d->State() == Device::STREAM_INIT);
      m_left->FinishCall(MockSide::START, true);
      ASSERT_TRUE(m_d->State() == Device::STREAMING);
   }

   void test_RemoveOneStreaming()
   {
      InitToState(Device::STREAMING, true);
      m_d->RemoveSide(RIGHT);
      // Already streaming, if we remove a side, it should stop and restart.
      ASSERT_TRUE(m_d->State() == Device::STREAM_INIT);
      ASSERT_TRUE(m_left->State() == Side::WAITING_FOR_STOP);
      m_left->FinishCall(MockSide::STOP, true);
      ASSERT_TRUE(m_left->State() == Side::WAITING_FOR_READY);
      m_left->FinishCall(MockSide::START, true);
      ASSERT_TRUE(m_d->State() == Device::STREAMING);
   }

   void test_AddOneInit()
   {
      InitToState(Device::STREAM_INIT, false);
      m_d->AddSide(RIGHT, m_right);
      // Shouldn't modify the state (We haven't started yet)
      ASSERT_TRUE(m_d->State() == Device::STREAM_INIT);
      m_left->FinishCall(MockSide::START, true);
      ASSERT_TRUE(m_d->State() == Device::STREAM_INIT);
      m_right->FinishCall(MockSide::START, true);
      ASSERT_TRUE(m_d->State() == Device::STREAMING);
   }

   void test_AddOneStreaming()
   {
      InitToState(Device::STREAMING, false);
      m_d->AddSide(RIGHT, m_right);
      // Already streaming, if we add a side, it should stop and restart.
      ASSERT_TRUE(m_d->State() == Device::STREAM_INIT);
      ASSERT_TRUE(m_left->State() == Side::WAITING_FOR_STOP);
      m_left->FinishCall(MockSide::STOP, true);

      ASSERT_TRUE(m_d->State() == Device::STREAM_INIT);
      ASSERT_TRUE(m_left->State() == Side::WAITING_FOR_READY);
      ASSERT_TRUE(m_right->State() == Side::WAITING_FOR_READY);

      m_left->FinishCall(MockSide::START, true);
      ASSERT_TRUE(m_left->State() == Side::READY);
      ASSERT_TRUE(m_d->State() == Device::STREAM_INIT);
      m_right->FinishCall(MockSide::START, true);
      ASSERT_TRUE(m_right->State() == Side::READY);
      ASSERT_TRUE(m_d->State() == Device::STREAMING);
   }

   void test_FailOneOnInit()
   {
      InitToState(Device::STREAM_INIT, false);

      // On init, if a device fails, it should retry to reinitialize it.
      m_left->ClearCalls();
      m_left->FinishCall(MockSide::START, false);
      ASSERT_TRUE(m_left->Called(MockSide::START));
      ASSERT_TRUE(m_left->State() == MockSide::WAITING_FOR_READY);
      ASSERT_TRUE(m_d->State() == Device::STREAM_INIT);

      // Allow it to finish this time, and verify that it made it to streaming.
      m_left->FinishCall(MockSide::START, true);
      ASSERT_TRUE(m_left->State() == MockSide::READY);
      ASSERT_TRUE(m_d->State() == Device::STREAMING);
   }

   void test_FailTwoOnInit()
   {
      InitToState(Device::STREAM_INIT, true);
      m_left->ClearCalls();
      m_right->ClearCalls();

      // Allow left to finish, but fail right.
      m_left->FinishCall(MockSide::START, true);
      m_right->FinishCall(MockSide::START, false);
      ASSERT_TRUE(m_left->State() == MockSide::READY);
      ASSERT_TRUE(!m_left->Called(MockSide::START));
      ASSERT_TRUE(m_right->Called(MockSide::START));
      ASSERT_TRUE(m_right->State() == MockSide::WAITING_FOR_READY);
      ASSERT_TRUE(m_d->State() == Device::STREAM_INIT);

      // Allow it to finish this time, and verify that it made it to streaming.
      m_right->FinishCall(MockSide::START, true);
      ASSERT_TRUE(m_right->State() == MockSide::READY);
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
   test_Device().test_RemoveAll(Device::STOPPED, false);
   test_Device().test_RemoveAll(Device::STREAM_INIT, false);
   test_Device().test_RemoveAll(Device::STREAMING, false);
   test_Device().test_RemoveAll(Device::STOPPED, true);
   test_Device().test_RemoveAll(Device::STREAM_INIT, true);
   test_Device().test_RemoveAll(Device::STREAMING, true);

   test_Device().test_RemoveOneInit();
   test_Device().test_RemoveOneStreaming();
   test_Device().test_AddOneInit();
   test_Device().test_AddOneStreaming();

   test_Device().test_FailOneOnInit();
   test_Device().test_FailTwoOnInit();

   return 0;
}