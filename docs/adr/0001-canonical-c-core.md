# ADR 0001: Canonical C Core And Platform Ports

## Status

Accepted

## Context

The SDK repository previously contained separate ESP-IDF, C/POSIX, and MicroPython implementations that duplicated queue, retry, identity, lifecycle, and CBOR behavior. That made behavior drift likely and made each SDK harder to validate.

The repository now uses a canonical portable C core under `core/`, with platform SDKs under `ports/`. The SDK must continue to support direct HTTP devices and BLE-only devices relayed through companion apps.

## Decision

Honch SDK behavior is owned by the canonical portable C core. Platform SDKs are ports or conformance-compatible wrappers. The core owns event semantics, identity, sessions, queue policy, CBOR, retry classification, and packetization. Ports own storage, time, random, locking, scheduling, transport, and platform metadata.

## Consequences

ESP-IDF and POSIX remain public SDKs, but their internals sit behind port adapters. MicroPython remains a Python implementation initially, validated by conformance fixtures. React Native relay consumes the packetizer/relay contract.
