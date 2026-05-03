// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#pragma once

#include "honch.h"

honch_err_t honch_lifecycle_init(void);
void honch_lifecycle_deinit(void);

// Emit $device_boot event with reset reason.
honch_err_t honch_lifecycle_emit_boot(void);

// Emit $device_shutdown event.
honch_err_t honch_lifecycle_emit_shutdown(void);

// Check and emit $firmware_update if version changed.
honch_err_t honch_lifecycle_check_firmware(const char *current_version);

// Check battery level and emit $battery_low if needed.
void honch_lifecycle_check_battery(int level);
