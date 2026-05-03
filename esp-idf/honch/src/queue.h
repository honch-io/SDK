// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#pragma once

#include "honch.h"
#include <stddef.h>

honch_err_t honch_queue_init(void);
void honch_queue_deinit(void);

// Push a serialized JSON event string onto the queue. Takes ownership of event_json.
honch_err_t honch_queue_push(char *event_json);

// Pop up to max_events from the queue into the provided array.
// Returns number of events popped. Caller must free each string.
int honch_queue_pop(char **events_out, int max_events);

// Confirm removal of the last popped batch (deletes from NVS).
honch_err_t honch_queue_confirm(int count);

// Return events to the front of the queue on flush failure.
honch_err_t honch_queue_requeue(char **events, int count);

size_t honch_queue_depth(void);

honch_err_t honch_queue_clear(void);
