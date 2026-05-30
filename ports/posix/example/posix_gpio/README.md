# Honch POSIX GPIO Adapter Pattern

This example shows how a platform adapter should translate GPIO-style edge
signals into normal C-core analytics calls.

GPIO tracking is intentionally not part of `honch/honch.h`. Pin numbering,
interrupt delivery, debounce, pull configuration, and thread rules are platform
specific. Keep those details in an adapter and call `honch_track` only from a
normal task, worker, or application thread.

## Contract

An adapter that mirrors the ESP-IDF SDK behavior should:

- Support rising, falling, and both-edge modes.
- Debounce repeated samples before tracking. ESP-IDF uses a 50 ms debounce
  window; use that as the default unless the target platform has a better
  product-specific value.
- Track the caller-provided event name when a configured edge is accepted.
- Include at least `{"pin": <number>}` in the event properties.
- Avoid calling Honch from interrupt handlers or signal handlers.
- Move interrupt or signal work into a queue, pipe, eventfd, worker thread, or
  equivalent platform mechanism before calling `honch_track`.
- Keep the adapter bounded: fixed mapping limits, bounded event queues, and
  clear behavior when the adapter is full.
- Treat SDK errors as real failures. Surface or log summaries, but do not log
  secrets or full customer event payloads.

## Shape

The basic flow is:

```text
platform pin interrupt/sample
  -> adapter debounce and edge match
  -> adapter builds small typed properties
  -> honch_track(client, event_name, properties)
```

For example, a button on pin 0 might produce:

```c
const honch_property_t properties[] = {
    honch_prop("pin", honch_u64(0))
};
honch_track(client, "button_pressed", properties, 1);
```

Adapters may add extra non-sensitive platform details such as edge direction or
normalized value, but `pin` should remain present for ESP-IDF parity.

## What This Example Does

`main.c` uses synthetic samples instead of a real GPIO device. It demonstrates
the edge-detection and event-emission shape without depending on Linux GPIO
libraries, root permissions, or board-specific hardware.
