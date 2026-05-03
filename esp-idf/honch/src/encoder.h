// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Honch Dev

#pragma once

#include "honch.h"

// Encode a single event into a JSON string. Caller must free the result.
// Merges auto-stamped properties with user-supplied properties_json.
char *honch_encode_event(const char *event_name,
                         const char *distinct_id,
                         const char *properties_json);

// Build the batch wrapper JSON. Caller must free the result.
// events is an array of JSON event strings, count is the number.
char *honch_encode_batch(const char *api_key, char **events, int count);
