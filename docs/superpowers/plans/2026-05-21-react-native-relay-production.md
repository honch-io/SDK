# React Native Relay Production Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the React Native relay from a TypeScript skeleton into a production relay path that receives firmware packetizer chunks, durably stores complete payloads, retries uploads, and can be proven through capture and ClickHouse.

**Architecture:** Keep protocol parsing, queue assembly, durable storage, retry policy, and upload draining as small TypeScript units with injectable platform adapters. Native Android/iOS modules own BLE, background scheduling, and production storage bindings; the TypeScript package owns protocol correctness and orchestration. The first implementation slice establishes the contract and tested core before adding native BLE and example app code.

**Tech Stack:** TypeScript, Vitest, React Native package exports, Android WorkManager/BLE GATT, iOS CoreBluetooth/BGTaskScheduler, capture `POST /batch` CBOR upload.

---

### Task 1: Relay Frame Contract And CRC

**Files:**
- Modify: `ports/react-native-relay/src/frame.ts`
- Modify: `ports/react-native-relay/test/frame.test.ts`
- Create: `spec/conformance/relay/single_chunk.json`
- Create: `spec/conformance/relay/multi_chunk.json`

- [x] **Step 1: Write failing CRC and fixture tests**
- [x] **Step 2: Run the focused frame tests**
- [x] **Step 3: Implement CRC validation**
- [x] **Step 4: Re-run frame tests**

### Task 2: Durable Queue Interface

**Files:**
- Modify: `ports/react-native-relay/src/relayQueue.ts`
- Create: `ports/react-native-relay/src/durableStore.ts`
- Modify: `ports/react-native-relay/test/relayQueue.test.ts`

- [x] **Step 1: Write failing durable-store-backed queue tests**
- [x] **Step 2: Run relay queue tests**
- [x] **Step 3: Implement `RelayDurableStore` and `createDurableRelayQueue`**
- [x] **Step 4: Re-run relay queue tests**

### Task 3: Upload Retry Classification And Draining

**Files:**
- Modify: `ports/react-native-relay/src/uploader.ts`
- Create: `ports/react-native-relay/src/retry.ts`
- Create: `ports/react-native-relay/src/drain.ts`
- Modify: `ports/react-native-relay/test/uploader.test.ts`
- Create: `ports/react-native-relay/test/drain.test.ts`

- [x] **Step 1: Write failing response classification tests**
- [x] **Step 2: Run upload tests**
- [x] **Step 3: Implement retry policy and drain orchestration**
- [x] **Step 4: Re-run upload/drain tests**

### Task 4: Production Contract Documentation

**Files:**
- Modify: `spec/relay-envelope.md`
- Modify: `spec/relay-chunks.md`
- Modify: `ports/react-native-relay/README.md`

- [x] **Step 1: Document the current production contract**
- [x] **Step 2: Document ACK semantics**
- [x] **Step 3: Review docs for contradictions**

### Task 5: Native Package And Example App Scaffold

**Files:**
- Modify: `ports/react-native-relay/package.json`
- Create: `ports/react-native-relay/react-native.config.js`
- Create: `ports/react-native-relay/android/build.gradle`
- Create: `ports/react-native-relay/android/src/main/AndroidManifest.xml`
- Create: `ports/react-native-relay/ios/HonchReactNativeRelay.podspec`
- Create: `ports/react-native-relay/example/README.md`

- [x] **Step 1: Add package metadata tests or shape checks**
- [x] **Step 2: Add native package scaffold**
- [x] **Step 3: Add example app README**

### Task 6: BLE And Background Implementations

**Files:**
- Create: `ports/react-native-relay/src/ble.ts`
- Create: `ports/react-native-relay/src/scheduler.ts`
- Create: Android and iOS native module files under `ports/react-native-relay/android/` and `ports/react-native-relay/ios/`

- [ ] **Step 1: Define TypeScript native interfaces**
- [ ] **Step 2: Implement Android first**
- [ ] **Step 3: Implement iOS second**

### Task 7: Firmware And E2E Proof

**Files:**
- Create: ESP-IDF relay example or benchtest mode under `ports/esp-idf/`
- Create: E2E scripts/tests under `ports/react-native-relay/test/` or `tools/`

- [ ] **Step 1: Add firmware relay mode**
- [ ] **Step 2: Prove capture path**
