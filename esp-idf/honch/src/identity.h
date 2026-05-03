// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#pragma once

#include "honch.h"

// Device ID: "dev_" + 12 hex chars from MAC
#define HONCH_DEVICE_ID_LEN 17  // "dev_" (4) + 12 hex + null

// Session ID: "sess_" + UUID v4
#define HONCH_SESSION_ID_LEN 42 // "sess_" (5) + 36 uuid + null

honch_err_t honch_identity_init(void);
void honch_identity_deinit(void);

const char *honch_identity_get_device_id(void);
const char *honch_identity_get_distinct_id(void);
honch_err_t honch_identity_set_distinct_id(const char *distinct_id);

const char *honch_identity_get_session_id(void);
honch_err_t honch_identity_start_session(void);
void honch_identity_end_session(void);

honch_err_t honch_identity_reset(void);

honch_err_t honch_identity_check_firmware(const char *current_version,
                                           char *prev_version_out,
                                           size_t prev_version_size,
                                           bool *changed);
