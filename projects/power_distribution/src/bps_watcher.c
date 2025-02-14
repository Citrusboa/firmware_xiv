#include "bps_watcher.h"

#include "event_queue.h"
#include "log.h"
#include "pd_events.h"

#define NO_FAULT 0

StatusCode prv_bps_watcher_callback_handler(const CanMessage *msg, void *context,
                                            CanAckStatus *ack_reply) {
  uint8_t data = 0;
  CAN_UNPACK_BPS_HEARTBEAT(msg, &data);
  if (data != NO_FAULT) {
    LOG_DEBUG("Got BPS fault! Starting strobe and switching to what's powered on aux.\n");
    event_raise_priority(PD_BPS_STROBE_EVENT_PRIORITY, PD_STROBE_EVENT, 1);
    // raising a TURN EVERYTHING AUX because we need to turn main off and switch to aux
    // in the case of BPS fault
    event_raise_priority(PD_ACTION_EVENT_PRIORITY, PD_POWER_AUX_SEQUENCE_EVENT_TURN_ON_EVERYTHING,
                         1);
  }
  return STATUS_CODE_OK;
}

StatusCode bps_watcher_init(void) {
  return can_register_rx_handler(SYSTEM_CAN_MESSAGE_BPS_HEARTBEAT, prv_bps_watcher_callback_handler,
                                 NULL);
}
