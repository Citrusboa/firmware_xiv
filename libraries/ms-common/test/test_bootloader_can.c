#include <string.h>

#include "bootloader_can.h"
#include "event_queue.h"
#include "gpio.h"
#include "interrupt.h"
#include "ms_test_helper_can.h"
#include "ms_test_helpers.h"
#include "soft_timer.h"
#include "test_helpers.h"
#include "unity.h"

#define CLIENT_SCRIPT_CONTROLLER_BOARD_ID 0
#define NON_CLIENT_SCRIPT_CONTROLLER_BOARD_ID 2

typedef enum {
  TEST_CAN_EVENT_RX = 0,
  TEST_CAN_EVENT_TX,
  TEST_CAN_EVENT_FAULT,
} TestCanEvent;

static CanStorage s_can_storage;
static uint8_t s_times_callback_called;
static uint8_t s_received_data[8];
static size_t s_received_len;
static bool s_received_is_start_message;
static uint16_t s_board_id;

static CanSettings s_can_settings = {
  .device_id = SYSTEM_CAN_DEVICE_BOOTLOADER,
  .bitrate = CAN_HW_BITRATE_500KBPS,
  .rx_event = TEST_CAN_EVENT_RX,
  .tx_event = TEST_CAN_EVENT_TX,
  .fault_event = TEST_CAN_EVENT_FAULT,
  .tx = { GPIO_PORT_A, 12 },
  .rx = { GPIO_PORT_A, 11 },
  .loopback = true,
};

static StatusCode prv_bootloader_rx_callback(uint8_t *data, size_t len, bool is_start_msg) {
  s_times_callback_called++;
  memcpy(s_received_data, data, len);
  s_received_len = len;
  s_received_is_start_message = is_start_msg;
  return STATUS_CODE_OK;
}

void setup_test(void) {
  s_times_callback_called = 0;
  memset(s_received_data, 0, sizeof(s_received_data));
  s_received_len = 0;
  s_received_is_start_message = false;
  s_board_id = 0;

  event_queue_init();
  gpio_init();
  interrupt_init();
  soft_timer_init();

  TEST_ASSERT_OK(bootloader_can_register_handler(prv_bootloader_rx_callback));
}

void teardown_test(void) {}

// Test that a bootloader CAN message with client board ID can be succesfully sent
// and the appropriate callback called when received.
void test_bootloader_can_received(void) {
  TEST_ASSERT_OK(
      bootloader_can_init(&s_can_storage, &s_can_settings, CLIENT_SCRIPT_CONTROLLER_BOARD_ID));
  uint8_t data[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
  TEST_ASSERT_OK(bootloader_can_transmit(data, 8, true));

  // process bootloader message
  MS_TEST_HELPER_CAN_TX_RX(TEST_CAN_EVENT_TX, TEST_CAN_EVENT_RX);
  TEST_ASSERT_EQUAL(1, s_times_callback_called);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(data, s_received_data, 8);
  TEST_ASSERT_EQUAL(8, s_received_len);
  TEST_ASSERT_EQUAL(true, s_received_is_start_message);
}

// Test that a bootloader CAN message sent with non-client board ID is not received.
void test_bootloader_can_not_received(void) {
  TEST_ASSERT_OK(
      bootloader_can_init(&s_can_storage, &s_can_settings, NON_CLIENT_SCRIPT_CONTROLLER_BOARD_ID));
  uint8_t data[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
  TEST_ASSERT_OK(bootloader_can_transmit(data, 8, false));

  // process bootloader message, and callback should not be called
  MS_TEST_HELPER_CAN_TX_RX(TEST_CAN_EVENT_TX, TEST_CAN_EVENT_RX);
  TEST_ASSERT_EQUAL(0, s_times_callback_called);
}

// Test that a bootloader CAN message with len < 8 is received succesfully,
// and that a message with len > 8 is not sent.
void test_bootloader_can_various_len(void) {
  TEST_ASSERT_OK(
      bootloader_can_init(&s_can_storage, &s_can_settings, CLIENT_SCRIPT_CONTROLLER_BOARD_ID));
  // test when data length is 5
  uint8_t data[5] = { 0, 4, 0, 1, 5 };
  TEST_ASSERT_OK(bootloader_can_transmit(data, 5, true));

  // process bootloader message
  MS_TEST_HELPER_CAN_TX_RX(TEST_CAN_EVENT_TX, TEST_CAN_EVENT_RX);
  TEST_ASSERT_EQUAL(1, s_times_callback_called);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(data, s_received_data, 5);
  TEST_ASSERT_EQUAL(5, s_received_len);
  TEST_ASSERT_EQUAL(true, s_received_is_start_message);

  // test when data length is 0, none of the data should be transmitted
  uint8_t *empty_data = NULL;
  TEST_ASSERT_OK(bootloader_can_transmit(empty_data, 0, false));

  // process bootloader message
  MS_TEST_HELPER_CAN_TX_RX(TEST_CAN_EVENT_TX, TEST_CAN_EVENT_RX);
  TEST_ASSERT_EQUAL(2, s_times_callback_called);
  // no change in the static recived_data variable, since no new data was transmitted
  TEST_ASSERT_EQUAL_UINT8_ARRAY(data, s_received_data, 5);
  TEST_ASSERT_EQUAL(0, s_received_len);
  TEST_ASSERT_EQUAL(false, s_received_is_start_message);

  // test for transmitting message with len > 8, should return STATUS_CODE_INVALID_ARGS
  TEST_ASSERT_NOT_OK(bootloader_can_transmit(data, 9, false));
}
