#include "regen_braking.h"

#include "can.h"
#include "can_msg_defs.h"
#include "can_unpack.h"
#include "status.h"

static bool s_regen_braking_state;

// Callback function to set the regen braking state
static StatusCode prv_regen_braking_callback(const CanMessage *msg, void *context,
                                             CanAckStatus *ack_reply) {
  CAN_UNPACK_REGEN_BRAKING(msg, (uint8_t *)&s_regen_braking_state);
  return STATUS_CODE_OK;
}

StatusCode regen_braking_init(void) {
  // Default to enabled
  s_regen_braking_state = true;
  can_register_rx_handler(SYSTEM_CAN_MESSAGE_REGEN_BRAKING, prv_regen_braking_callback, NULL);
  return STATUS_CODE_OK;
}

bool get_regen_braking_state(void) {
  return s_regen_braking_state;
}
