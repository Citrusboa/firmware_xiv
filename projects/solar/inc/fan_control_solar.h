#pragma once

// Handles solar fault events including overtemperature and fail faults. Also drives full speed
// pin accordingly. Requires GPIO, interrupts, soft timers, the event queue, CAN, and data_store,
// and fault_handler to be initialized.

#include <stdbool.h>

#include "fault_monitor.h"
#include "gpio.h"
#include "gpio_it.h"
#include "solar_events.h"

typedef struct FanControlSolarSettings {
  GpioAddress overtemp_addr;
  GpioAddress fan_fail_addr;
  GpioAddress full_speed_addr;
  uint16_t full_speed_temp_threshold_dC;  // in deciCelsius

  SolarMpptCount mppt_count;
} FanControlSolarSettings;

StatusCode fan_control_init(FanControlSolarSettings *settings);

bool fan_control_process_event(Event *e);
