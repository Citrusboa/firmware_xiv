#include "update_id.h"

#include <pb_common.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <stdio.h>

#include "bootloader_can.h"
#include "bootloader_datagram_defs.h"
#include "can_datagram.h"
#include "config.h"
#include "crc32.h"
#include "dispatcher.h"
#include "log.h"
#include "reset.h"
#include "update_id.pb.h"

// Note: Certain functionality will be given to the client script to control
// The client script will make sure only one board is edited per command and
// will make sure the id given is not currently used

static CanDatagramTxConfig s_response_config = {
  .dgram_type = BOOTLOADER_DATAGRAM_STATUS_RESPONSE,
  .destination_nodes_len = 0,
  .destination_nodes = NULL,
  .data_len = 1,
  .data = NULL,
  .tx_cb = bootloader_can_transmit,
  .tx_cmpl_cb = tx_cmpl_cb,
};

static StatusCode prv_callback_update_id(uint8_t *data, uint16_t data_len, void *context) {
  UpdateId id_proto = UpdateId_init_zero;
  pb_istream_t stream = pb_istream_from_buffer(data, data_len);
  bool status = pb_decode(&stream, UpdateId_fields, &id_proto);

  uint8_t response_data = STATUS_CODE_OK;

  if (!status) {
    LOG_WARN("Decoding failed: %s\n", PB_GET_ERROR(&stream));
    status_response(STATUS_CODE_INTERNAL_ERROR, tx_cmpl_cb);
    return STATUS_CODE_INTERNAL_ERROR;
  }

  BootloaderConfig previous_config = { 0 };
  config_get(&previous_config);

  BootloaderConfig new_board_config = { .crc32 = 0,
                                        .controller_board_id = (uint8_t)id_proto.new_id,
                                        .project_present = previous_config.project_present,
                                        .application_crc32 = previous_config.application_crc32,
                                        .application_size = previous_config.application_size };

  memcpy(new_board_config.controller_board_name, previous_config.controller_board_name,
         sizeof(new_board_config.controller_board_name));
  memcpy(new_board_config.project_name, previous_config.project_name,
         sizeof(new_board_config.project_name));
  memcpy(new_board_config.project_info, previous_config.project_info,
         sizeof(new_board_config.project_info));
  memcpy(new_board_config.git_version, previous_config.git_version,
         sizeof(new_board_config.git_version));

  new_board_config.crc32 = crc32_arr((uint8_t *)&new_board_config, sizeof(BootloaderConfig));

  if (config_commit(&new_board_config) == STATUS_CODE_INTERNAL_ERROR) {
    status_response(STATUS_CODE_INTERNAL_ERROR, tx_cmpl_cb);
    return STATUS_CODE_INTERNAL_ERROR;
  }

  // Will send STATUS_CODE_OK back in datagram and
  // upon tx completion will reset software
  status_response(STATUS_CODE_OK, reset);

  // Because of the reset, the return statement will never be reached
  return STATUS_CODE_OK;
}

StatusCode update_id_init(void) {
  return dispatcher_register_callback(BOOTLOADER_DATAGRAM_UPDATE_ID, prv_callback_update_id, NULL);
}
