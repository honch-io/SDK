// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#pragma once

#include "honch.h"

honch_err_t honch_scheduler_init(uint32_t flush_interval_seconds,
                                  uint32_t flush_event_threshold);
void honch_scheduler_deinit(void);

// Notify the worker that the threshold was crossed or connectivity restored.
void honch_scheduler_notify(void);

// Perform a synchronous flush with timeout (milliseconds). Used by shutdown.
honch_err_t honch_scheduler_flush_sync(uint32_t timeout_ms);
