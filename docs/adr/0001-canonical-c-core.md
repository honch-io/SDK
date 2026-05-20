# ADR 0001: Canonical C Core And Platform Ports

## Status

Accepted

## Context

The SDK repository currently contains separate ESP-IDF, C/POSIX, and MicroPython implementations. They duplicate queue, retry, identity, lifecycle, and CBOR behavior. The SDK must support direct HTTP devices and BLE-only devices relayed through companion apps.

## Decision

Honch SDK behavior will be owned by a canonical portable C core. Platform SDKs will be ports or conformance-compatible wrappers. The core owns event semantics, identity, sessions, queue policy, CBOR, retry classification, and packetization. Ports own storage, time, random, locking, scheduling, transport, and platform metadata.

## Consequences

ESP-IDF and POSIX remain public SDKs, but their internals move behind port adapters. MicroPython remains a Python implementation initially, validated by conformance fixtures. React Native relay consumes the packetizer/relay contract.
