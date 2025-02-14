#include <string.h>

#include "can_transmit.h"
#include "can_unpack.h"
#include "centre_console_events.h"
#include "event_queue.h"
#include "gpio.h"
#include "gpio_it.h"
#include "interrupt.h"
#include "ms_test_helper_can.h"
#include "ms_test_helpers.h"
#include "race_switch.h"
#include "status.h"
#include "test_helpers.h"
#include "unity.h"

static CanStorage s_can_storage = { 0 };

static RaceSwitchFsmStorage s_race_switch_fsm_storage;
static GpioState s_returned_state;
static GpioAddress s_race_switch_address = { .port = GPIO_PORT_A, .pin = 4 };
static GpioAddress s_voltage_monitor_address = { .port = GPIO_PORT_B, .pin = 7 };
static uint8_t s_can_race_mode_on = false;

StatusCode TEST_MOCK(gpio_get_state)(const GpioAddress *address, GpioState *state) {
  *state = s_returned_state;
  return STATUS_CODE_OK;
}

// verifies correct data is being sent on the race switch status message
static StatusCode prv_race_status_callback(const CanMessage *msg, void *context,
                                           CanAckStatus *ack_reply) {
  CAN_UNPACK_RACE_NORMAL_STATUS(msg, &s_can_race_mode_on);
  return STATUS_CODE_OK;
}

static bool prv_process_fsm_event_manually(void) {
  Event e;
  event_process(&e);
  bool transitioned = race_switch_fsm_process_event(&s_race_switch_fsm_storage, &e);
  return transitioned;
}

void setup_test(void) {
  initialize_can_and_dependencies(&s_can_storage, SYSTEM_CAN_DEVICE_CENTRE_CONSOLE,
                                  CENTRE_CONSOLE_EVENT_CAN_TX, CENTRE_CONSOLE_EVENT_CAN_RX,
                                  CENTRE_CONSOLE_EVENT_CAN_FAULT);
  gpio_it_init();
  TEST_ASSERT_OK(can_register_rx_handler(SYSTEM_CAN_MESSAGE_RACE_NORMAL_STATUS,
                                         prv_race_status_callback, NULL));
  memset(&s_race_switch_fsm_storage, 0, sizeof(s_race_switch_fsm_storage));
  TEST_ASSERT_OK(race_switch_fsm_init(&s_race_switch_fsm_storage));
}

void teardown_test(void) {}

void prv_assert_current_race_state(RaceState state) {
  TEST_ASSERT_EQUAL(state, race_switch_fsm_get_current_state(&s_race_switch_fsm_storage));
}

void test_can_state_on(void) {
  prv_assert_current_race_state(RACE_STATE_OFF);

  CAN_TRANSMIT_RACE_NORMAL_SWITCH_MODE(RACE_STATE_ON);
  MS_TEST_HELPER_CAN_TX_RX(CENTRE_CONSOLE_EVENT_CAN_TX, CENTRE_CONSOLE_EVENT_CAN_RX);

  TEST_ASSERT_TRUE(prv_process_fsm_event_manually());
  // sending CAN message out to notify other devices e.g telemetry
  MS_TEST_HELPER_CAN_TX_RX(CENTRE_CONSOLE_EVENT_CAN_TX, CENTRE_CONSOLE_EVENT_CAN_RX);

  TEST_ASSERT_TRUE(s_can_race_mode_on);
  TEST_ASSERT_EQUAL(RACE_STATE_ON, race_switch_fsm_get_current_state(&s_race_switch_fsm_storage));
}

void test_can_state_off(void) {
  prv_assert_current_race_state(RACE_STATE_OFF);

  CAN_TRANSMIT_RACE_NORMAL_SWITCH_MODE(RACE_STATE_ON);
  MS_TEST_HELPER_CAN_TX_RX(CENTRE_CONSOLE_EVENT_CAN_TX, CENTRE_CONSOLE_EVENT_CAN_RX);

  TEST_ASSERT_TRUE(prv_process_fsm_event_manually());
  MS_TEST_HELPER_CAN_TX_RX(CENTRE_CONSOLE_EVENT_CAN_TX, CENTRE_CONSOLE_EVENT_CAN_RX);

  TEST_ASSERT_TRUE(s_can_race_mode_on);
  prv_assert_current_race_state(RACE_STATE_ON);

  CAN_TRANSMIT_RACE_NORMAL_SWITCH_MODE(RACE_STATE_OFF);
  MS_TEST_HELPER_CAN_TX_RX(CENTRE_CONSOLE_EVENT_CAN_TX, CENTRE_CONSOLE_EVENT_CAN_RX);

  TEST_ASSERT_TRUE(prv_process_fsm_event_manually());
  MS_TEST_HELPER_CAN_TX_RX(CENTRE_CONSOLE_EVENT_CAN_TX, CENTRE_CONSOLE_EVENT_CAN_RX);

  TEST_ASSERT_FALSE(s_can_race_mode_on);
  TEST_ASSERT_EQUAL(RACE_STATE_OFF, race_switch_fsm_get_current_state(&s_race_switch_fsm_storage));
}

void test_transition_to_race(void) {
  // Test state machine when normal -> race
  // Initially the module begins in normal mode so PA4 is low
  s_returned_state = GPIO_STATE_LOW;

  prv_assert_current_race_state(RACE_STATE_OFF);

  // Mock rising edge on race switch pin
  s_returned_state = GPIO_STATE_HIGH;

  // Trigger interrupt to change fsm state from normal to race
  TEST_ASSERT_OK(gpio_it_trigger_interrupt(&s_race_switch_address));
  TEST_ASSERT_TRUE(prv_process_fsm_event_manually());

  MS_TEST_HELPER_CAN_TX_RX(CENTRE_CONSOLE_EVENT_CAN_TX, CENTRE_CONSOLE_EVENT_CAN_RX);

  TEST_ASSERT_TRUE(s_can_race_mode_on);
  prv_assert_current_race_state(RACE_STATE_ON);

  // Test if no error when interrupt is triggered with same edge multiple times
  TEST_ASSERT_OK(gpio_it_trigger_interrupt(&s_race_switch_address));

  // No fsm state transition will occur so race_switch_fsm_process_event will return false
  // no tx needed if state transition didn't occur
  TEST_ASSERT_FALSE(prv_process_fsm_event_manually());

  prv_assert_current_race_state(RACE_STATE_ON);
}

void test_transition_to_normal(void) {
  // Test state machine when race -> normal
  // Initially the module begins in race mode so PA4 is high
  s_returned_state = GPIO_STATE_HIGH;
  prv_assert_current_race_state(RACE_STATE_ON);

  // Mock falling edge on race switch pin
  s_returned_state = GPIO_STATE_LOW;

  // Trigger interrupt to change fsm state from race to normal
  TEST_ASSERT_OK(gpio_it_trigger_interrupt(&s_race_switch_address));
  TEST_ASSERT_TRUE(prv_process_fsm_event_manually());

  MS_TEST_HELPER_CAN_TX_RX(CENTRE_CONSOLE_EVENT_CAN_TX, CENTRE_CONSOLE_EVENT_CAN_RX);

  TEST_ASSERT_FALSE(s_can_race_mode_on);
  prv_assert_current_race_state(RACE_STATE_OFF);
}

void test_voltage_during_transition(void) {
  // Test voltage regulator when normal -> race -> normal
  // Initially the car is in normal mode so the voltage regulator is enabled
  s_returned_state = GPIO_STATE_LOW;
  prv_assert_current_race_state(RACE_STATE_OFF);

  GpioState voltage_monitor_state;
  // Mock high state on voltage monitor pin
  s_returned_state = GPIO_STATE_HIGH;

  gpio_get_state(&s_voltage_monitor_address, &voltage_monitor_state);
  TEST_ASSERT_EQUAL(voltage_monitor_state, s_returned_state);

  // Switch to race mode
  TEST_ASSERT_OK(gpio_it_trigger_interrupt(&s_race_switch_address));
  TEST_ASSERT_TRUE(prv_process_fsm_event_manually());

  MS_TEST_HELPER_CAN_TX_RX(CENTRE_CONSOLE_EVENT_CAN_TX, CENTRE_CONSOLE_EVENT_CAN_RX);

  TEST_ASSERT_TRUE(s_can_race_mode_on);
  prv_assert_current_race_state(RACE_STATE_ON);

  // Mock low state on voltage monitor pin
  s_returned_state = GPIO_STATE_LOW;
  gpio_get_state(&s_voltage_monitor_address, &voltage_monitor_state);
  TEST_ASSERT_EQUAL(voltage_monitor_state, s_returned_state);

  // Switch to normal mode
  TEST_ASSERT_OK(gpio_it_trigger_interrupt(&s_race_switch_address));
  TEST_ASSERT_TRUE(prv_process_fsm_event_manually());

  MS_TEST_HELPER_CAN_TX_RX(CENTRE_CONSOLE_EVENT_CAN_TX, CENTRE_CONSOLE_EVENT_CAN_RX);

  TEST_ASSERT_FALSE(s_can_race_mode_on);
  prv_assert_current_race_state(RACE_STATE_OFF);

  // Mock high state on voltage monitor pin
  s_returned_state = GPIO_STATE_HIGH;
  gpio_get_state(&s_voltage_monitor_address, &voltage_monitor_state);
  TEST_ASSERT_EQUAL(voltage_monitor_state, s_returned_state);
}
