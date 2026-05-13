// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#pragma once

#include "honch.h"
#include "encoder.h"
#include <stddef.h>

honch_err_t honch_queue_init(void);
void honch_queue_deinit(void);

// Push a serialized CBOR event blob onto the queue. Takes ownership of payload.
honch_err_t honch_queue_push(honch_payload_t *payload);

// Pop up to max_events from the queue into the provided array.
// Returns number of events popped. Caller must free each payload.
int honch_queue_pop(honch_payload_t *events_out, int max_events);

// Confirm removal of the last popped batch (deletes from NVS).
honch_err_t honch_queue_confirm(int count);

// Return events to the front of the queue on flush failure.
honch_err_t honch_queue_requeue(honch_payload_t *events, int count);

size_t honch_queue_depth(void);

honch_err_t honch_queue_clear(void);
