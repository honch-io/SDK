#include "honch/core/wire_v2.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static honch_wire_v2_batch_context_t test_context(void)
{
    return (honch_wire_v2_batch_context_t) {
        .distinct_id = "user-1",
        .device_id = "device-1",
        .device_model = "model-x",
        .firmware_version = "1.0.0",
        .sdk_platform = "posix",
        .sdk_version = "0.2.0",
        .environment = "production"
    };
}

static bool bytes_end_with(const uint8_t *bytes, size_t bytes_size, const uint8_t *suffix, size_t suffix_size)
{
    return bytes_size >= suffix_size && memcmp(bytes + bytes_size - suffix_size, suffix, suffix_size) == 0;
}

static bool bytes_contain(const uint8_t *bytes, size_t bytes_size, const uint8_t *needle, size_t needle_size)
{
    if (needle_size == 0u || bytes_size < needle_size) {
        return false;
    }
    for (size_t i = 0u; i <= bytes_size - needle_size; i++) {
        if (memcmp(bytes + i, needle, needle_size) == 0) {
            return true;
        }
    }
    return false;
}

static void test_crc16_ccitt_false_matches_check_value(void)
{
    static const uint8_t input[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};

    assert(honch_wire_v2_crc16(input, sizeof(input)) == 0x29b1u);
}

static void test_single_final_frame_uses_v2_header_varint_id_and_message_crc(void)
{
    static const uint8_t message[] = {0x02u, 0x01u, 0x01u};
    uint8_t frame[16] = {0};
    size_t frame_size = 0u;

    honch_wire_v2_frame_spec_t spec = {
        .message_id = 300u,
        .total_message_length = sizeof(message),
        .offset = 0u,
        .payload = message,
        .payload_size = sizeof(message),
        .source_type = HONCH_WIRE_V2_SOURCE_EVENTS,
        .continuation = false,
        .more = false
    };

    assert(honch_wire_v2_encode_frame(&spec, frame, sizeof(frame), &frame_size) == HONCH_OK);

    static const uint8_t expected[] = {
        0x02u,
        0xacu, 0x02u,
        0x02u, 0x01u, 0x01u,
        0xecu, 0x81u
    };
    assert(frame_size == sizeof(expected));
    assert(memcmp(frame, expected, sizeof(expected)) == 0);
}

static void test_init_frame_includes_total_length_and_no_crc_when_more_frames_follow(void)
{
    static const uint8_t payload[] = {0x02u, 0x01u};
    uint8_t frame[16] = {0};
    size_t frame_size = 0u;

    honch_wire_v2_frame_spec_t spec = {
        .message_id = 7u,
        .total_message_length = 5u,
        .offset = 0u,
        .payload = payload,
        .payload_size = sizeof(payload),
        .source_type = HONCH_WIRE_V2_SOURCE_EVENTS,
        .continuation = false,
        .more = true
    };

    assert(honch_wire_v2_encode_frame(&spec, frame, sizeof(frame), &frame_size) == HONCH_OK);

    static const uint8_t expected[] = {
        0x42u,
        0x07u,
        0x05u,
        0x02u, 0x01u
    };
    assert(frame_size == sizeof(expected));
    assert(memcmp(frame, expected, sizeof(expected)) == 0);
}

static void test_final_continuation_uses_offset_and_crc_over_complete_message(void)
{
    static const uint8_t complete_message[] = {0x01u, 0x02u, 0x03u, 0x04u, 0x05u};
    static const uint8_t payload[] = {0x03u, 0x04u, 0x05u};
    uint8_t frame[16] = {0};
    size_t frame_size = 0u;

    honch_wire_v2_frame_spec_t spec = {
        .message_id = 7u,
        .total_message_length = sizeof(complete_message),
        .offset = 2u,
        .payload = payload,
        .payload_size = sizeof(payload),
        .crc_message = complete_message,
        .crc_message_size = sizeof(complete_message),
        .source_type = HONCH_WIRE_V2_SOURCE_EVENTS,
        .continuation = true,
        .more = false
    };

    assert(honch_wire_v2_encode_frame(&spec, frame, sizeof(frame), &frame_size) == HONCH_OK);

    static const uint8_t expected[] = {
        0x22u,
        0x07u,
        0x02u,
        0x03u, 0x04u, 0x05u,
        0x04u, 0x93u
    };
    assert(frame_size == sizeof(expected));
    assert(memcmp(frame, expected, sizeof(expected)) == 0);
}

static void test_final_frame_rejects_incomplete_payload(void)
{
    static const uint8_t complete_message[] = {0x01u, 0x02u, 0x03u, 0x04u, 0x05u};
    static const uint8_t payload[] = {0x03u, 0x04u};
    uint8_t frame[16] = {0};
    size_t frame_size = 99u;

    honch_wire_v2_frame_spec_t spec = {
        .message_id = 7u,
        .total_message_length = sizeof(complete_message),
        .offset = 2u,
        .payload = payload,
        .payload_size = sizeof(payload),
        .crc_message = complete_message,
        .crc_message_size = sizeof(complete_message),
        .source_type = HONCH_WIRE_V2_SOURCE_EVENTS,
        .continuation = true,
        .more = false
    };

    assert(honch_wire_v2_encode_frame(&spec, frame, sizeof(frame), &frame_size) ==
        HONCH_ERROR_INVALID_ARGUMENT);
    assert(frame_size == 0u);
}

static void test_more_frame_rejects_complete_payload(void)
{
    static const uint8_t payload[] = {0x01u, 0x02u, 0x03u};
    uint8_t frame[16] = {0};
    size_t frame_size = 99u;

    honch_wire_v2_frame_spec_t spec = {
        .message_id = 7u,
        .total_message_length = sizeof(payload),
        .offset = 0u,
        .payload = payload,
        .payload_size = sizeof(payload),
        .source_type = HONCH_WIRE_V2_SOURCE_EVENTS,
        .continuation = false,
        .more = true
    };

    assert(honch_wire_v2_encode_frame(&spec, frame, sizeof(frame), &frame_size) ==
        HONCH_ERROR_INVALID_ARGUMENT);
    assert(frame_size == 0u);
}

static void test_chunker_splits_message_into_ascending_v2_frames(void)
{
    static const uint8_t message[] = {0x01u, 0x02u, 0x03u, 0x04u, 0x05u};
    honch_wire_v2_chunker_t chunker = {0};
    uint8_t first[6] = {0};
    uint8_t second[6] = {0};
    uint8_t third[6] = {0};
    uint8_t fourth[6] = {0};
    size_t first_size = 0u;
    size_t second_size = 0u;
    size_t third_size = 0u;
    size_t fourth_size = 0u;
    bool complete = true;

    assert(honch_wire_v2_chunker_begin(
        &chunker,
        7u,
        message,
        sizeof(message),
        HONCH_WIRE_V2_SOURCE_EVENTS,
        sizeof(first)) == HONCH_OK);
    assert(honch_wire_v2_chunker_next(&chunker, first, sizeof(first), &first_size, &complete) == HONCH_OK);
    assert(!complete);
    assert(honch_wire_v2_chunker_next(&chunker, second, sizeof(second), &second_size, &complete) == HONCH_OK);
    assert(!complete);
    assert(honch_wire_v2_chunker_next(&chunker, third, sizeof(third), &third_size, &complete) == HONCH_OK);
    assert(complete);
    assert(honch_wire_v2_chunker_next(&chunker, fourth, sizeof(fourth), &fourth_size, &complete) ==
        HONCH_ERROR_INVALID_ARGUMENT);

    static const uint8_t expected_first[] = {0x42u, 0x07u, 0x05u, 0x01u, 0x02u, 0x03u};
    static const uint8_t expected_second[] = {0x62u, 0x07u, 0x03u, 0x04u};
    static const uint8_t expected_third[] = {0x22u, 0x07u, 0x04u, 0x05u, 0x04u, 0x93u};

    assert(first_size == sizeof(expected_first));
    assert(second_size == sizeof(expected_second));
    assert(third_size == sizeof(expected_third));
    assert(memcmp(first, expected_first, sizeof(expected_first)) == 0);
    assert(memcmp(second, expected_second, sizeof(expected_second)) == 0);
    assert(memcmp(third, expected_third, sizeof(expected_third)) == 0);
}

static void test_event_batch_encoder_writes_required_context_and_single_event(void)
{
    uint8_t message[128] = {0};
    size_t message_size = 0u;

    honch_wire_v2_batch_context_t context = test_context();
    honch_wire_v2_event_t event = {
        .event_name = "boot",
        .timestamp_ms = 1700000000000ULL,
        .properties = NULL,
        .property_count = 0u
    };

    assert(honch_wire_v2_encode_event_batch(
        &context,
        1700000000000ULL,
        &event,
        1u,
        message,
        sizeof(message),
        &message_size) == HONCH_OK);

    static const uint8_t expected[] = {
        0x02u, 0x01u, 0x01u, 0x80u, 0xd0u, 0x95u, 0xffu, 0xbcu, 0x31u, 0x08u,
        0x06u, 0x75u, 0x73u, 0x65u, 0x72u, 0x2du, 0x31u, 0x08u, 0x64u, 0x65u,
        0x76u, 0x69u, 0x63u, 0x65u, 0x2du, 0x31u, 0x07u, 0x6du, 0x6fu, 0x64u,
        0x65u, 0x6cu, 0x2du, 0x78u, 0x05u, 0x31u, 0x2eu, 0x30u, 0x2eu, 0x30u,
        0x05u, 0x70u, 0x6fu, 0x73u, 0x69u, 0x78u, 0x05u, 0x30u, 0x2eu, 0x32u,
        0x2eu, 0x30u, 0x0au, 0x70u, 0x72u, 0x6fu, 0x64u, 0x75u, 0x63u, 0x74u,
        0x69u, 0x6fu, 0x6eu, 0x04u, 0x62u, 0x6fu, 0x6fu, 0x74u, 0x07u, 0x01u,
        0x07u, 0x00u, 0x02u, 0x07u, 0x01u, 0x03u, 0x07u, 0x02u, 0x04u, 0x07u,
        0x03u, 0x05u, 0x07u, 0x04u, 0x06u, 0x07u, 0x05u, 0x07u, 0x07u, 0x06u,
        0x01u, 0x07u, 0x00u, 0x00u
    };
    assert(message_size == sizeof(expected));
    assert(memcmp(message, expected, sizeof(expected)) == 0);
}

static void test_event_batch_encoder_matches_single_required_context_fixture_frame(void)
{
    uint8_t message[128] = {0};
    uint8_t frame[160] = {0};
    size_t message_size = 0u;
    size_t frame_size = 0u;
    honch_wire_v2_batch_context_t context = test_context();
    honch_wire_v2_event_t event = {
        .event_name = "boot",
        .timestamp_ms = 1700000000000ULL,
        .properties = NULL,
        .property_count = 0u
    };

    assert(honch_wire_v2_encode_event_batch(
        &context,
        1700000000000ULL,
        &event,
        1u,
        message,
        sizeof(message),
        &message_size) == HONCH_OK);

    honch_wire_v2_frame_spec_t spec = {
        .message_id = 1u,
        .total_message_length = message_size,
        .offset = 0u,
        .payload = message,
        .payload_size = message_size,
        .source_type = HONCH_WIRE_V2_SOURCE_EVENTS,
        .continuation = false,
        .more = false
    };
    assert(honch_wire_v2_encode_frame(&spec, frame, sizeof(frame), &frame_size) == HONCH_OK);

    static const uint8_t expected_fixture_frame[] = {
        0x02u, 0x01u, 0x02u, 0x01u, 0x01u, 0x80u, 0xd0u, 0x95u, 0xffu, 0xbcu,
        0x31u, 0x08u, 0x06u, 0x75u, 0x73u, 0x65u, 0x72u, 0x2du, 0x31u, 0x08u,
        0x64u, 0x65u, 0x76u, 0x69u, 0x63u, 0x65u, 0x2du, 0x31u, 0x07u, 0x6du,
        0x6fu, 0x64u, 0x65u, 0x6cu, 0x2du, 0x78u, 0x05u, 0x31u, 0x2eu, 0x30u,
        0x2eu, 0x30u, 0x05u, 0x70u, 0x6fu, 0x73u, 0x69u, 0x78u, 0x05u, 0x30u,
        0x2eu, 0x32u, 0x2eu, 0x30u, 0x0au, 0x70u, 0x72u, 0x6fu, 0x64u, 0x75u,
        0x63u, 0x74u, 0x69u, 0x6fu, 0x6eu, 0x04u, 0x62u, 0x6fu, 0x6fu, 0x74u,
        0x07u, 0x01u, 0x07u, 0x00u, 0x02u, 0x07u, 0x01u, 0x03u, 0x07u, 0x02u,
        0x04u, 0x07u, 0x03u, 0x05u, 0x07u, 0x04u, 0x06u, 0x07u, 0x05u, 0x07u,
        0x07u, 0x06u, 0x01u, 0x07u, 0x00u, 0x00u, 0x55u, 0xf2u
    };
    assert(frame_size == sizeof(expected_fixture_frame));
    assert(memcmp(frame, expected_fixture_frame, sizeof(expected_fixture_frame)) == 0);
}

static void test_event_batch_encoder_writes_optional_context_metadata(void)
{
    uint8_t message[160] = {0};
    size_t message_size = 0u;

    honch_wire_v2_batch_context_t context = test_context();
    context.has_device_time_source = true;
    context.device_time_source = 2u;
    context.has_capabilities = true;
    context.capabilities = 5u;
    honch_wire_v2_event_t event = {
        .event_name = "boot",
        .timestamp_ms = 1700000000000ULL,
        .properties = NULL,
        .property_count = 0u
    };

    assert(honch_wire_v2_encode_event_batch(
        &context,
        1700000000000ULL,
        &event,
        1u,
        message,
        sizeof(message),
        &message_size) == HONCH_OK);

    static const uint8_t expected_context_tail[] = {
        0x07u, 0x07u, 0x06u,
        0x09u, 0x03u, 0x02u,
        0x0au, 0x03u, 0x05u,
        0x01u, 0x07u, 0x00u, 0x00u
    };
    assert(bytes_contain(message, message_size, expected_context_tail, sizeof(expected_context_tail)));
}

static void test_event_batch_encoder_writes_context_extension_values(void)
{
    uint8_t message[160] = {0};
    size_t message_size = 0u;

    honch_wire_v2_context_extension_t extensions[] = {
        {
            .key_id = 128u,
            .value = {
                .type = HONCH_WIRE_V2_VALUE_TYPE_STRING,
                .string_value = "edge",
                .string_size = SIZE_MAX
            }
        }
    };
    honch_wire_v2_batch_context_t context = test_context();
    context.extensions = extensions;
    context.extension_count = 1u;
    honch_wire_v2_event_t event = {
        .event_name = "boot",
        .timestamp_ms = 1700000000000ULL,
        .properties = NULL,
        .property_count = 0u
    };

    assert(honch_wire_v2_encode_event_batch(
        &context,
        1700000000000ULL,
        &event,
        1u,
        message,
        sizeof(message),
        &message_size) == HONCH_OK);

    static const uint8_t expected_context_extension[] = {
        0x80u, 0x01u, 0x07u, 0x07u,
        0x01u, 0x08u, 0x00u, 0x00u
    };
    assert(bytes_contain(message, message_size, (const uint8_t *)"edge", 4u));
    assert(bytes_contain(message, message_size, expected_context_extension, sizeof(expected_context_extension)));
}

static void test_event_batch_encoder_rejects_reserved_context_extension_ids(void)
{
    uint8_t message[160] = {0};
    size_t message_size = 0u;

    honch_wire_v2_context_extension_t extensions[] = {
        {
            .key_id = 127u,
            .value = {
                .type = HONCH_WIRE_V2_VALUE_TYPE_UINT,
                .uint_value = 1u
            }
        }
    };
    honch_wire_v2_batch_context_t context = test_context();
    context.extensions = extensions;
    context.extension_count = 1u;
    honch_wire_v2_event_t event = {
        .event_name = "boot",
        .timestamp_ms = 1700000000000ULL,
        .properties = NULL,
        .property_count = 0u
    };

    assert(honch_wire_v2_encode_event_batch(
        &context,
        1700000000000ULL,
        &event,
        1u,
        message,
        sizeof(message),
        &message_size) == HONCH_ERROR_INVALID_ARGUMENT);
}

static void test_event_batch_encoder_rejects_duplicate_context_extension_ids(void)
{
    uint8_t message[160] = {0};
    size_t message_size = 0u;

    honch_wire_v2_context_extension_t extensions[] = {
        {
            .key_id = 128u,
            .value = {
                .type = HONCH_WIRE_V2_VALUE_TYPE_UINT,
                .uint_value = 1u
            }
        },
        {
            .key_id = 128u,
            .value = {
                .type = HONCH_WIRE_V2_VALUE_TYPE_UINT,
                .uint_value = 2u
            }
        }
    };
    honch_wire_v2_batch_context_t context = test_context();
    context.extensions = extensions;
    context.extension_count = 2u;
    honch_wire_v2_event_t event = {
        .event_name = "boot",
        .timestamp_ms = 1700000000000ULL,
        .properties = NULL,
        .property_count = 0u
    };

    assert(honch_wire_v2_encode_event_batch(
        &context,
        1700000000000ULL,
        &event,
        1u,
        message,
        sizeof(message),
        &message_size) == HONCH_ERROR_INVALID_ARGUMENT);
}

static void test_event_batch_encoder_rejects_invalid_device_time_source(void)
{
    uint8_t message[160] = {0};
    size_t message_size = 0u;

    honch_wire_v2_batch_context_t context = test_context();
    context.has_device_time_source = true;
    context.device_time_source = 4u;
    honch_wire_v2_event_t event = {
        .event_name = "boot",
        .timestamp_ms = 1700000000000ULL,
        .properties = NULL,
        .property_count = 0u
    };

    assert(honch_wire_v2_encode_event_batch(
        &context,
        1700000000000ULL,
        &event,
        1u,
        message,
        sizeof(message),
        &message_size) == HONCH_ERROR_INVALID_ARGUMENT);
}

static void test_event_batch_encoder_writes_custom_string_property(void)
{
    uint8_t message[128] = {0};
    size_t message_size = 0u;

    honch_wire_v2_batch_context_t context = test_context();
    honch_wire_v2_property_t properties[] = {
        {
            .key = "mode",
            .value = {
                .type = HONCH_WIRE_V2_VALUE_TYPE_STRING,
                .string_value = "auto",
                .string_size = SIZE_MAX
            }
        }
    };
    honch_wire_v2_event_t event = {
        .event_name = "hdr",
        .timestamp_ms = 1025u,
        .properties = properties,
        .property_count = 1u
    };

    assert(honch_wire_v2_encode_event_batch(
        &context,
        1000u,
        &event,
        1u,
        message,
        sizeof(message),
        &message_size) == HONCH_OK);

    static const uint8_t expected[] = {
        0x02u, 0x01u, 0x01u, 0xe8u, 0x07u, 0x0au, 0x06u, 0x75u, 0x73u, 0x65u,
        0x72u, 0x2du, 0x31u, 0x08u, 0x64u, 0x65u, 0x76u, 0x69u, 0x63u, 0x65u,
        0x2du, 0x31u, 0x07u, 0x6du, 0x6fu, 0x64u, 0x65u, 0x6cu, 0x2du, 0x78u,
        0x05u, 0x31u, 0x2eu, 0x30u, 0x2eu, 0x30u, 0x05u, 0x70u, 0x6fu, 0x73u,
        0x69u, 0x78u, 0x05u, 0x30u, 0x2eu, 0x32u, 0x2eu, 0x30u, 0x0au, 0x70u,
        0x72u, 0x6fu, 0x64u, 0x75u, 0x63u, 0x74u, 0x69u, 0x6fu, 0x6eu, 0x03u,
        0x68u, 0x64u, 0x72u, 0x04u, 0x6du, 0x6fu, 0x64u, 0x65u, 0x04u, 0x61u,
        0x75u, 0x74u, 0x6fu, 0x07u, 0x01u, 0x07u, 0x00u, 0x02u, 0x07u, 0x01u,
        0x03u, 0x07u, 0x02u, 0x04u, 0x07u, 0x03u, 0x05u, 0x07u, 0x04u, 0x06u,
        0x07u, 0x05u, 0x07u, 0x07u, 0x06u, 0x01u, 0x07u, 0x32u, 0x01u, 0x11u,
        0x07u, 0x09u
    };
    assert(message_size == sizeof(expected));
    assert(memcmp(message, expected, sizeof(expected)) == 0);
}

static void test_event_batch_encoder_allows_empty_string_property_values(void)
{
    uint8_t message[128] = {0};
    size_t message_size = 0u;
    honch_wire_v2_batch_context_t context = test_context();
    honch_wire_v2_property_t properties[] = {
        {
            .key = "note",
            .value = {
                .type = HONCH_WIRE_V2_VALUE_TYPE_STRING,
                .string_value = ""
            }
        }
    };
    honch_wire_v2_event_t event = {
        .event_name = "annotated",
        .timestamp_ms = 1000u,
        .properties = properties,
        .property_count = 1u
    };

    assert(honch_wire_v2_encode_event_batch(
        &context,
        1000u,
        &event,
        1u,
        message,
        sizeof(message),
        &message_size) == HONCH_OK);

    static const uint8_t expected_suffix[] = {
        0x07u, 0x00u, 0x01u, 0x11u, 0x07u, 0x09u
    };
    assert(message[5] == 0x0au);
    assert(bytes_end_with(message, message_size, expected_suffix, sizeof(expected_suffix)));
}

static void test_event_batch_encoder_treats_zero_string_size_as_empty(void)
{
    uint8_t message[128] = {0};
    size_t message_size = 0u;
    honch_wire_v2_batch_context_t context = test_context();
    honch_wire_v2_property_t properties[] = {
        {
            .key = "note",
            .value = {
                .type = HONCH_WIRE_V2_VALUE_TYPE_STRING,
                .string_value = "sentinel",
                .string_size = 0u
            }
        }
    };
    honch_wire_v2_event_t event = {
        .event_name = "annotated",
        .timestamp_ms = 1000u,
        .properties = properties,
        .property_count = 1u
    };

    assert(honch_wire_v2_encode_event_batch(
        &context,
        1000u,
        &event,
        1u,
        message,
        sizeof(message),
        &message_size) == HONCH_OK);

    assert(!bytes_contain(message, message_size, (const uint8_t *)"sentinel", 8u));
}

static void test_event_batch_encoder_uses_reserved_property_key_refs(void)
{
    uint8_t message[128] = {0};
    size_t message_size = 0u;
    honch_wire_v2_batch_context_t context = test_context();
    honch_wire_v2_property_t properties[] = {
        {
            .key = "$battery_level",
            .value = {
                .type = HONCH_WIRE_V2_VALUE_TYPE_INT,
                .int_value = 44
            }
        }
    };
    honch_wire_v2_event_t event = {
        .event_name = "battery",
        .timestamp_ms = 1000u,
        .properties = properties,
        .property_count = 1u
    };

    assert(honch_wire_v2_encode_event_batch(
        &context,
        1000u,
        &event,
        1u,
        message,
        sizeof(message),
        &message_size) == HONCH_OK);

    static const uint8_t expected_suffix[] = {
        0x07u, 0x00u, 0x01u, 0x02u, 0x04u, 0x58u
    };
    assert(message[5] == 0x08u);
    assert(bytes_end_with(message, message_size, expected_suffix, sizeof(expected_suffix)));
}

static void test_event_batch_encoder_writes_float64_little_endian(void)
{
    uint8_t message[128] = {0};
    size_t message_size = 0u;
    honch_wire_v2_batch_context_t context = test_context();
    honch_wire_v2_property_t properties[] = {
        {
            .key = "temperature",
            .value = {
                .type = HONCH_WIRE_V2_VALUE_TYPE_FLOAT64,
                .float64_value = 36.5
            }
        }
    };
    honch_wire_v2_event_t event = {
        .event_name = "sample",
        .timestamp_ms = 1000u,
        .properties = properties,
        .property_count = 1u
    };

    assert(honch_wire_v2_encode_event_batch(
        &context,
        1000u,
        &event,
        1u,
        message,
        sizeof(message),
        &message_size) == HONCH_OK);

    static const uint8_t expected_suffix[] = {
        0x07u, 0x00u, 0x01u, 0x11u, 0x06u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x40u, 0x42u, 0x40u
    };
    assert(bytes_end_with(message, message_size, expected_suffix, sizeof(expected_suffix)));
}

static void test_event_batch_encoder_writes_bytes_values(void)
{
    uint8_t message[128] = {0};
    size_t message_size = 0u;
    static const uint8_t blob[] = {0xdeu, 0xadu, 0xbeu, 0xefu};
    honch_wire_v2_batch_context_t context = test_context();
    honch_wire_v2_property_t properties[] = {
        {
            .key = "blob",
            .value = {
                .type = HONCH_WIRE_V2_VALUE_TYPE_BYTES,
                .bytes = {
                    .data = blob,
                    .size = sizeof(blob)
                }
            }
        }
    };
    honch_wire_v2_event_t event = {
        .event_name = "binary",
        .timestamp_ms = 1000u,
        .properties = properties,
        .property_count = 1u
    };

    assert(honch_wire_v2_encode_event_batch(
        &context,
        1000u,
        &event,
        1u,
        message,
        sizeof(message),
        &message_size) == HONCH_OK);

    static const uint8_t expected_suffix[] = {
        0x07u, 0x00u, 0x01u, 0x11u, 0x08u, 0x04u, 0xdeu, 0xadu, 0xbeu, 0xefu
    };
    assert(bytes_end_with(message, message_size, expected_suffix, sizeof(expected_suffix)));
}

static void test_event_batch_encoder_writes_empty_bytes_values(void)
{
    uint8_t message[128] = {0};
    size_t message_size = 0u;
    honch_wire_v2_batch_context_t context = test_context();
    honch_wire_v2_property_t properties[] = {
        {
            .key = "blob",
            .value = {
                .type = HONCH_WIRE_V2_VALUE_TYPE_BYTES,
                .bytes = {
                    .data = NULL,
                    .size = 0u
                }
            }
        }
    };
    honch_wire_v2_event_t event = {
        .event_name = "binary",
        .timestamp_ms = 1000u,
        .properties = properties,
        .property_count = 1u
    };

    assert(honch_wire_v2_encode_event_batch(
        &context,
        1000u,
        &event,
        1u,
        message,
        sizeof(message),
        &message_size) == HONCH_OK);

    static const uint8_t expected_suffix[] = {
        0x07u, 0x00u, 0x01u, 0x11u, 0x08u, 0x00u
    };
    assert(bytes_end_with(message, message_size, expected_suffix, sizeof(expected_suffix)));
}

static void test_byte_append_skips_memcpy_for_empty_values(void)
{
    uint8_t message[128] = {0};
    size_t message_size = 0u;
    honch_wire_v2_batch_context_t context = test_context();
    honch_wire_v2_property_t properties[] = {
        {
            .key = "blob",
            .value = {
                .type = HONCH_WIRE_V2_VALUE_TYPE_BYTES,
                .bytes = {
                    .data = NULL,
                    .size = 0u
                }
            }
        }
    };
    honch_wire_v2_event_t event = {
        .event_name = "binary",
        .timestamp_ms = 1000u,
        .properties = properties,
        .property_count = 1u
    };

    assert(honch_wire_v2_encode_event_batch(
        &context,
        1000u,
        &event,
        1u,
        message,
        sizeof(message),
        &message_size) == HONCH_OK);

    assert(message_size > 0u);
}

static void test_event_batch_encoder_rejects_nan_float64(void)
{
    uint8_t message[128] = {0};
    size_t message_size = 0u;
    honch_wire_v2_batch_context_t context = test_context();
    union {
        uint64_t bits;
        double value;
    } nan_value = {.bits = 0x7ff8000000000000ULL};
    honch_wire_v2_property_t properties[] = {
        {
            .key = "temperature",
            .value = {
                .type = HONCH_WIRE_V2_VALUE_TYPE_FLOAT64,
                .float64_value = nan_value.value
            }
        }
    };
    honch_wire_v2_event_t event = {
        .event_name = "sample",
        .timestamp_ms = 1000u,
        .properties = properties,
        .property_count = 1u
    };

    assert(honch_wire_v2_encode_event_batch(
        &context,
        1000u,
        &event,
        1u,
        message,
        sizeof(message),
        &message_size) == HONCH_ERROR_INVALID_ARGUMENT);
}

static void test_event_batch_encoder_writes_nested_array_and_map_values(void)
{
    uint8_t message[192] = {0};
    size_t message_size = 0u;
    honch_wire_v2_batch_context_t context = test_context();
    honch_wire_v2_value_t modes[] = {
        {
            .type = HONCH_WIRE_V2_VALUE_TYPE_STRING,
            .string_value = "hdr",
            .string_size = SIZE_MAX
        },
        {
            .type = HONCH_WIRE_V2_VALUE_TYPE_STRING,
            .string_value = "auto",
            .string_size = SIZE_MAX
        }
    };
    honch_wire_v2_map_pair_t detail_pairs[] = {
        {
            .key = "modes",
            .value = {
                .type = HONCH_WIRE_V2_VALUE_TYPE_ARRAY,
                .array = {
                    .items = modes,
                    .count = 2u
                }
            }
        }
    };
    honch_wire_v2_property_t properties[] = {
        {
            .key = "details",
            .value = {
                .type = HONCH_WIRE_V2_VALUE_TYPE_MAP,
                .map = {
                    .entries = detail_pairs,
                    .count = 1u
                }
            }
        }
    };
    honch_wire_v2_event_t event = {
        .event_name = "capture",
        .timestamp_ms = 1000u,
        .properties = properties,
        .property_count = 1u
    };

    assert(honch_wire_v2_encode_event_batch(
        &context,
        1000u,
        &event,
        1u,
        message,
        sizeof(message),
        &message_size) == HONCH_OK);

    static const uint8_t expected_suffix[] = {
        0x07u, 0x00u, 0x01u, 0x11u, 0x0au, 0x01u, 0x13u, 0x09u, 0x02u,
        0x07u, 0x0au, 0x07u, 0x0bu
    };
    assert(message[5] == 0x0cu);
    assert(bytes_end_with(message, message_size, expected_suffix, sizeof(expected_suffix)));
}

static void test_event_batch_encoder_allows_eight_nested_arrays(void)
{
    uint8_t message[256] = {0};
    size_t message_size = 0u;
    honch_wire_v2_batch_context_t context = test_context();
    honch_wire_v2_value_t values[9] = {0};

    values[8] = (honch_wire_v2_value_t) {
        .type = HONCH_WIRE_V2_VALUE_TYPE_UINT,
        .uint_value = 1u
    };
    for (int i = 7; i >= 0; i--) {
        values[i] = (honch_wire_v2_value_t) {
            .type = HONCH_WIRE_V2_VALUE_TYPE_ARRAY,
            .array = {
                .items = &values[i + 1],
                .count = 1u
            }
        };
    }

    honch_wire_v2_property_t properties[] = {
        {
            .key = "nested",
            .value = values[0]
        }
    };
    honch_wire_v2_event_t event = {
        .event_name = "depth",
        .timestamp_ms = 1000u,
        .properties = properties,
        .property_count = 1u
    };

    assert(honch_wire_v2_encode_event_batch(
        &context,
        1000u,
        &event,
        1u,
        message,
        sizeof(message),
        &message_size) == HONCH_OK);
}

static void test_event_batch_encoder_rejects_nine_nested_arrays(void)
{
    uint8_t message[256] = {0};
    size_t message_size = 0u;
    honch_wire_v2_batch_context_t context = test_context();
    honch_wire_v2_value_t values[10] = {0};

    values[9] = (honch_wire_v2_value_t) {
        .type = HONCH_WIRE_V2_VALUE_TYPE_UINT,
        .uint_value = 1u
    };
    for (int i = 8; i >= 0; i--) {
        values[i] = (honch_wire_v2_value_t) {
            .type = HONCH_WIRE_V2_VALUE_TYPE_ARRAY,
            .array = {
                .items = &values[i + 1],
                .count = 1u
            }
        };
    }

    honch_wire_v2_property_t properties[] = {
        {
            .key = "nested",
            .value = values[0]
        }
    };
    honch_wire_v2_event_t event = {
        .event_name = "depth",
        .timestamp_ms = 1000u,
        .properties = properties,
        .property_count = 1u
    };

    assert(honch_wire_v2_encode_event_batch(
        &context,
        1000u,
        &event,
        1u,
        message,
        sizeof(message),
        &message_size) == HONCH_ERROR_INVALID_ARGUMENT);
}

static void test_event_batch_encoder_rejects_duplicate_map_keys(void)
{
    uint8_t message[192] = {0};
    size_t message_size = 0u;
    honch_wire_v2_batch_context_t context = test_context();
    honch_wire_v2_map_pair_t detail_pairs[] = {
        {
            .key = "mode",
            .value = {
                .type = HONCH_WIRE_V2_VALUE_TYPE_STRING,
                .string_value = "hdr",
                .string_size = SIZE_MAX
            }
        },
        {
            .key = "mode",
            .value = {
                .type = HONCH_WIRE_V2_VALUE_TYPE_STRING,
                .string_value = "auto",
                .string_size = SIZE_MAX
            }
        }
    };
    honch_wire_v2_property_t properties[] = {
        {
            .key = "details",
            .value = {
                .type = HONCH_WIRE_V2_VALUE_TYPE_MAP,
                .map = {
                    .entries = detail_pairs,
                    .count = 2u
                }
            }
        }
    };
    honch_wire_v2_event_t event = {
        .event_name = "capture",
        .timestamp_ms = 1000u,
        .properties = properties,
        .property_count = 1u
    };

    assert(honch_wire_v2_encode_event_batch(
        &context,
        1000u,
        &event,
        1u,
        message,
        sizeof(message),
        &message_size) == HONCH_ERROR_INVALID_ARGUMENT);
}

static void test_event_batch_encoder_rejects_null_map_keys(void)
{
    uint8_t message[192] = {0};
    size_t message_size = 0u;
    honch_wire_v2_batch_context_t context = test_context();
    honch_wire_v2_map_pair_t detail_pairs[] = {
        {
            .key = "mode",
            .value = {
                .type = HONCH_WIRE_V2_VALUE_TYPE_STRING,
                .string_value = "hdr",
                .string_size = SIZE_MAX
            }
        },
        {
            .key = NULL,
            .value = {
                .type = HONCH_WIRE_V2_VALUE_TYPE_STRING,
                .string_value = "auto",
                .string_size = SIZE_MAX
            }
        }
    };
    honch_wire_v2_property_t properties[] = {
        {
            .key = "details",
            .value = {
                .type = HONCH_WIRE_V2_VALUE_TYPE_MAP,
                .map = {
                    .entries = detail_pairs,
                    .count = 2u
                }
            }
        }
    };
    honch_wire_v2_event_t event = {
        .event_name = "capture",
        .timestamp_ms = 1000u,
        .properties = properties,
        .property_count = 1u
    };

    assert(honch_wire_v2_encode_event_batch(
        &context,
        1000u,
        &event,
        1u,
        message,
        sizeof(message),
        &message_size) == HONCH_ERROR_INVALID_ARGUMENT);
}

static void test_event_batch_encoder_rejects_promoted_context_property_duplicates(void)
{
    uint8_t message[128] = {0};
    size_t message_size = 0u;
    honch_wire_v2_batch_context_t context = test_context();
    honch_wire_v2_property_t properties[] = {
        {
            .key = "$device_id",
            .value = {
                .type = HONCH_WIRE_V2_VALUE_TYPE_STRING,
                .string_value = "spoof",
                .string_size = SIZE_MAX
            }
        }
    };
    honch_wire_v2_event_t event = {
        .event_name = "spoof",
        .timestamp_ms = 1000u,
        .properties = properties,
        .property_count = 1u
    };

    assert(honch_wire_v2_encode_event_batch(
        &context,
        1000u,
        &event,
        1u,
        message,
        sizeof(message),
        &message_size) == HONCH_ERROR_INVALID_ARGUMENT);
}

static void test_event_batch_encoder_rejects_duplicate_property_keys(void)
{
    uint8_t message[160] = {0};
    size_t message_size = 0u;
    honch_wire_v2_batch_context_t context = test_context();
    honch_wire_v2_property_t properties[] = {
        {
            .key = "mode",
            .value = {
                .type = HONCH_WIRE_V2_VALUE_TYPE_STRING,
                .string_value = "auto",
                .string_size = SIZE_MAX
            }
        },
        {
            .key = "mode",
            .value = {
                .type = HONCH_WIRE_V2_VALUE_TYPE_STRING,
                .string_value = "manual",
                .string_size = SIZE_MAX
            }
        }
    };
    honch_wire_v2_event_t event = {
        .event_name = "hdr",
        .timestamp_ms = 1000u,
        .properties = properties,
        .property_count = 2u
    };

    assert(honch_wire_v2_encode_event_batch(
        &context,
        1000u,
        &event,
        1u,
        message,
        sizeof(message),
        &message_size) == HONCH_ERROR_INVALID_ARGUMENT);
}

static void test_event_batch_encoder_rejects_string_table_entry_over_sdk_limit(void)
{
    uint8_t message[128] = {0};
    size_t message_size = 0u;
    honch_wire_v2_batch_context_t context = test_context();
    char long_value[514] = {0};
    memset(long_value, 'a', 513u);
    honch_wire_v2_property_t properties[] = {
        {
            .key = "note",
            .value = {
                .type = HONCH_WIRE_V2_VALUE_TYPE_STRING,
                .string_value = long_value,
                .string_size = SIZE_MAX
            }
        }
    };
    honch_wire_v2_event_t event = {
        .event_name = "annotated",
        .timestamp_ms = 1000u,
        .properties = properties,
        .property_count = 1u
    };

    assert(honch_wire_v2_encode_event_batch(
        &context,
        1000u,
        &event,
        1u,
        message,
        sizeof(message),
        &message_size) == HONCH_ERROR_INVALID_ARGUMENT);
}

static void test_event_batch_encoder_rejects_invalid_utf8_strings(void)
{
    uint8_t message[128] = {0};
    size_t message_size = 0u;
    honch_wire_v2_batch_context_t context = test_context();
    const char invalid_utf8[] = {(char)0xc3, (char)0x28, '\0'};
    honch_wire_v2_property_t properties[] = {
        {
            .key = "note",
            .value = {
                .type = HONCH_WIRE_V2_VALUE_TYPE_STRING,
                .string_value = invalid_utf8,
                .string_size = SIZE_MAX
            }
        }
    };
    honch_wire_v2_event_t event = {
        .event_name = "annotated",
        .timestamp_ms = 1000u,
        .properties = properties,
        .property_count = 1u
    };

    assert(honch_wire_v2_encode_event_batch(
        &context,
        1000u,
        &event,
        1u,
        message,
        sizeof(message),
        &message_size) == HONCH_ERROR_INVALID_ARGUMENT);
}

static void test_event_batch_encoder_rejects_total_string_table_bytes_over_sdk_limit(void)
{
    uint8_t message[4096] = {0};
    size_t message_size = 0u;
    honch_wire_v2_batch_context_t context = test_context();
    char values[5][501] = {{0}};
    honch_wire_v2_property_t properties[5] = {0};

    for (size_t i = 0u; i < 5u; i++) {
        memset(values[i], (int)('a' + i), 500u);
        properties[i] = (honch_wire_v2_property_t) {
            .key = i == 0u ? "note0" : i == 1u ? "note1" : i == 2u ? "note2" : i == 3u ? "note3" : "note4",
            .value = {
                .type = HONCH_WIRE_V2_VALUE_TYPE_STRING,
                .string_value = values[i],
                .string_size = SIZE_MAX
            }
        };
    }

    honch_wire_v2_event_t event = {
        .event_name = "annotated",
        .timestamp_ms = 1000u,
        .properties = properties,
        .property_count = 5u
    };

    assert(honch_wire_v2_encode_event_batch(
        &context,
        1000u,
        &event,
        1u,
        message,
        sizeof(message),
        &message_size) == HONCH_ERROR_INVALID_ARGUMENT);
}

static void test_event_batch_encoder_rejects_compact_messages_over_sdk_limit(void)
{
    uint8_t message[8192] = {0};
    size_t message_size = 0u;
    honch_wire_v2_batch_context_t context = test_context();
    honch_wire_v2_property_t properties[64] = {0};
    honch_wire_v2_event_t events[10] = {0};
    char keys[64][8] = {{0}};

    for (size_t i = 0u; i < 64u; i++) {
        int written = snprintf(keys[i], sizeof(keys[i]), "k%02u", (unsigned)i);
        assert(written > 0 && (size_t)written < sizeof(keys[i]));
        properties[i] = (honch_wire_v2_property_t) {
            .key = keys[i],
            .value = {
                .type = HONCH_WIRE_V2_VALUE_TYPE_UINT,
                .uint_value = 123456789u + (uint64_t)i
            }
        };
    }
    for (size_t i = 0u; i < 10u; i++) {
        events[i] = (honch_wire_v2_event_t) {
            .event_name = "sample",
            .timestamp_ms = 1000u + (uint64_t)i,
            .properties = properties,
            .property_count = 64u
        };
    }

    assert(honch_wire_v2_encode_event_batch(
        &context,
        1000u,
        events,
        10u,
        message,
        sizeof(message),
        &message_size) == HONCH_ERROR_INVALID_ARGUMENT);
}

int main(void)
{
    test_crc16_ccitt_false_matches_check_value();
    test_single_final_frame_uses_v2_header_varint_id_and_message_crc();
    test_init_frame_includes_total_length_and_no_crc_when_more_frames_follow();
    test_final_continuation_uses_offset_and_crc_over_complete_message();
    test_final_frame_rejects_incomplete_payload();
    test_more_frame_rejects_complete_payload();
    test_chunker_splits_message_into_ascending_v2_frames();
    test_event_batch_encoder_writes_required_context_and_single_event();
    test_event_batch_encoder_matches_single_required_context_fixture_frame();
    test_event_batch_encoder_writes_optional_context_metadata();
    test_event_batch_encoder_writes_context_extension_values();
    test_event_batch_encoder_rejects_reserved_context_extension_ids();
    test_event_batch_encoder_rejects_duplicate_context_extension_ids();
    test_event_batch_encoder_rejects_invalid_device_time_source();
    test_event_batch_encoder_writes_custom_string_property();
    test_event_batch_encoder_allows_empty_string_property_values();
    test_event_batch_encoder_treats_zero_string_size_as_empty();
    test_event_batch_encoder_uses_reserved_property_key_refs();
    test_event_batch_encoder_writes_float64_little_endian();
    test_event_batch_encoder_writes_bytes_values();
    test_event_batch_encoder_writes_empty_bytes_values();
    test_byte_append_skips_memcpy_for_empty_values();
    test_event_batch_encoder_rejects_nan_float64();
    test_event_batch_encoder_writes_nested_array_and_map_values();
    test_event_batch_encoder_allows_eight_nested_arrays();
    test_event_batch_encoder_rejects_nine_nested_arrays();
    test_event_batch_encoder_rejects_duplicate_map_keys();
    test_event_batch_encoder_rejects_null_map_keys();
    test_event_batch_encoder_rejects_promoted_context_property_duplicates();
    test_event_batch_encoder_rejects_duplicate_property_keys();
    test_event_batch_encoder_rejects_string_table_entry_over_sdk_limit();
    test_event_batch_encoder_rejects_invalid_utf8_strings();
    test_event_batch_encoder_rejects_total_string_table_bytes_over_sdk_limit();
    test_event_batch_encoder_rejects_compact_messages_over_sdk_limit();
    return 0;
}
