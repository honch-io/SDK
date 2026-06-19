/**
 * Honch JSON ingestion — TypeScript reference client (companion-app relay).
 *
 * This is a *reference implementation* meant to be READ and PORTED, not a
 * published package. It models the common hardware topology:
 *
 *     device (firmware)  ──BLE/serial──▶  your mobile/companion app  ──HTTPS──▶  Honch
 *                                          (this client runs HERE)
 *
 * The device generates events; your app collects them and forwards them to
 * Honch. Two things follow from that and shape this client:
 *
 *   1. The `context` describes the DEVICE, not the phone — `$device_id`,
 *      `$device_model`, `$firmware_version` are the paired hardware's. (One
 *      client instance relays one device. For several devices, use one client
 *      each — `context` is per-request, so they can't share a batch anyway.)
 *   2. The phone is the unreliable hop: it goes offline, gets backgrounded, and
 *      gets killed. So this client (a) preserves each event's real device time,
 *      and (b) supports durable persistence of the pending queue so nothing is
 *      lost across an app restart (see `initialQueue` / `onQueueChange`).
 *
 * It is intentionally zero-runtime-dependency (uses the global `fetch`),
 * heavily commented, and correct-over-clever.
 *
 * ---------------------------------------------------------------------------
 * THE API CONTRACT (authoritative)
 * ---------------------------------------------------------------------------
 * Endpoint:   POST {host}/capture        Content-Type: application/json
 * Auth:       header  X-Honch-Project-Key: <project_api_key>
 * Body:       { "context": { distinct_id, $device_id, $device_model,
 *                            $firmware_version, $sdk_platform, $sdk_version,
 *                            $environment?, $session_id? },
 *               "events": [ { event, timestamp?, properties? } ] }
 *
 * `context` is sent ONCE per request; the SERVER promotes every key (except
 * `distinct_id`, the top-level identity) into each event's properties. This
 * client sends context + events as-is and never pre-merges or injects defaults
 * (e.g. `$environment` defaults to "production" server-side) — which is exactly
 * why the shared conformance fixtures round-trip through buildBatch unchanged.
 *
 * `timestamp` is epoch MILLISECONDS or RFC3339; omit it and the server stamps
 * receive time. A relay should pass the DEVICE's event time so an event keeps
 * its real time even after sitting on the phone offline (see track()).
 *
 * Success:  200 -> { "status": "ok", "accepted": N, "rejected": M, "errors": [...] }
 *   Partial acceptance: a 200 stores the valid events; any per-event errors in
 *   `errors` are permanent (bad data) — log them, never retry.
 * Failure:  4xx (nothing stored) -> { "errors": [ { code, message, field? } ] }
 *
 * Retry policy:
 *   - retry on 429, 5xx, and network/transport errors with exponential backoff
 *     (initial 1s, max 5min, +/-25% jitter);
 *   - DROP on other 4xx (400/401/415/422) — permanent, fix the request.
 *
 * Identity: `distinct_id` starts as the device id (anonymous). When the app's
 * user signs in, call identify() — it emits a `$identify` event carrying
 * `$anon_distinct_id`, which is what merges the anonymous device person into the
 * user. See identify() and the "Build Your Own Integration" docs.
 *
 * Validation: POST {host}/capture/validate is a dry run — same body, stores
 * nothing, returns { ok, accepted, rejected, expanded_events, errors }. See validate().
 */

// Reported as $sdk_version by default; override via the constructor when you fork.
// Tracks the SDK behavior version (matches core's HONCH_SDK_VERSION).
export const SDK_VERSION = "0.2.2";
// Default $sdk_platform. A real relay should set this to its platform, e.g.
// "react-native", "ios", or "android".
export const SDK_PLATFORM = "honch-ts-ref";

// Server-side limit: at most 500 events per /capture request.
export const MAX_EVENTS_PER_REQUEST = 500;

// Backoff bounds for the retry loop (milliseconds).
const BACKOFF_INITIAL_MS = 1_000;
const BACKOFF_MAX_MS = 300_000; // 5 minutes
const BACKOFF_JITTER = 0.25; // +/-25%

// ---------------------------------------------------------------------------
// Wire types. These mirror the documented body shape exactly.
// ---------------------------------------------------------------------------

/** A JSON-serializable property value. */
export type PropertyValue =
  | string
  | number
  | boolean
  | null
  | PropertyValue[]
  | { [key: string]: PropertyValue };

/** The once-per-request context. `distinct_id` is identity; `$`-keys are the
 *  paired device's dimensions. The contract forbids unknown context keys, so we
 *  do NOT allow arbitrary extras — that mirrors `unknown_context_key`. */
export interface Context {
  distinct_id?: string;
  $device_id: string;
  $device_model: string;
  $firmware_version: string;
  $sdk_platform: string;
  $sdk_version: string;
  $environment?: string;
  $session_id?: string;
}

/** A single event. `timestamp` is epoch-ms or RFC3339; omit to let server stamp. */
export interface HonchEvent {
  event: string;
  timestamp?: number | string;
  properties?: Record<string, PropertyValue>;
}

/** The full POST /capture request body. */
export interface Batch {
  context: Context;
  events: HonchEvent[];
}

/** Shape returned by POST /capture/validate (and used to read error bodies). */
export interface ValidateResponse {
  ok?: boolean;
  content_type?: string;
  accepted?: number;
  rejected?: number;
  expanded_events?: unknown[];
  errors?: Array<{ code: string; message: string; field?: string }>;
}

// ---------------------------------------------------------------------------
// Pure helpers (no I/O, no state) — easy to unit test and to port.
// ---------------------------------------------------------------------------

/**
 * Assemble the request body for POST /capture.
 *
 * PURE function: given a context and a list of events it returns the exact JSON
 * object the server expects, with NO mutation of the inputs and NO injected
 * defaults. Defaults like $environment="production" are applied by the *server*,
 * which is why the conformance fixtures round-trip through buildBatch unchanged.
 */
export function buildBatch(context: Context, events: HonchEvent[]): Batch {
  // Shallow copies so adding/removing entries on the caller's own context or
  // events list won't disturb an in-flight batch. (Nested objects — e.g. an
  // event's `properties` — are shared by reference, so don't mutate those in
  // place after enqueueing; track() already snapshots properties on its own.)
  return { context: { ...context }, events: [...events] };
}

/**
 * Should an HTTP status be retried (vs. permanently dropped)?
 * Retry: status 0 (network/transport failure — see post()), 429 (rate limited),
 *        and any 5xx (server error).
 * Drop:  all other 4xx (400 bad request, 401 unauthorized, 415, 422 validation).
 */
function isRetryableStatus(status: number): boolean {
  return status === 0 || status === 429 || (status >= 500 && status <= 599);
}

/**
 * Exponential backoff with full +/-25% jitter, capped at BACKOFF_MAX_MS.
 * `attempt` is 0-based: 0 -> ~1s, 1 -> ~2s, 2 -> ~4s, ...
 */
function backoffDelayMs(attempt: number): number {
  const base = Math.min(BACKOFF_INITIAL_MS * 2 ** attempt, BACKOFF_MAX_MS);
  const jitter = base * BACKOFF_JITTER;
  // Uniformly pick within [base - jitter, base + jitter].
  return Math.max(0, base + (Math.random() * 2 - 1) * jitter);
}

function nowMs(): number {
  return Date.now();
}

function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

/** Reference-client logging hook. Replace with your logging framework. */
function log(msg: string): void {
  // eslint-disable-next-line no-console
  console.log(`[honch] ${msg}`);
}

// ---------------------------------------------------------------------------
// Client.
// ---------------------------------------------------------------------------

export interface HonchClientOptions {
  apiKey: string;
  /** The paired device's identity/dimensions (NOT the phone's). */
  deviceId: string;
  deviceModel: string;
  firmwareVersion: string;
  /** Identity. Defaults to deviceId (an anonymous person keyed on the hardware);
   * call identify() once the user is known to merge it into the user's person. */
  distinctId?: string;
  /** The relay's platform, reported as $sdk_platform (e.g. "react-native"). */
  sdkPlatform?: string;
  sdkVersion?: string;
  host?: string;
  environment?: string;
  sessionId?: string;
  /** Batching config. */
  flushEventThreshold?: number;
  flushIntervalSeconds?: number;
  maxQueue?: number;
  /** Per-request timeout (ms) and max retry attempts before drop. */
  timeoutMs?: number;
  maxRetries?: number;
  /**
   * Durable persistence (recommended on mobile, where the app can be killed).
   * `initialQueue` rehydrates events saved before the last shutdown.
   * `onQueueChange` is called with the current PENDING queue whenever it changes
   * (event queued, or events sent/dropped) — persist it (e.g. to MMKV or
   * AsyncStorage) so nothing is lost across a restart. Both are optional; the
   * default is an in-memory queue.
   */
  initialQueue?: HonchEvent[];
  onQueueChange?: (pending: readonly HonchEvent[]) => void;
}

/**
 * Buffered relay client for the Honch JSON ingestion API.
 *
 * Events are appended to a queue and flushed when it reaches
 * `flushEventThreshold`, every `flushIntervalSeconds` (via a timer), or on an
 * explicit flush()/close(). With `onQueueChange`/`initialQueue` wired to durable
 * storage, the pending queue survives app restarts.
 *
 * JavaScript is single-threaded, so no locking is needed — but flush() can be
 * triggered concurrently (timer + size trigger). A simple in-flight flag
 * coalesces those so events are never sent twice.
 */
export class HonchClient {
  private readonly apiKey: string;
  private readonly host: string;
  private readonly timeoutMs: number;
  private readonly maxRetries: number;
  private readonly flushEventThreshold: number;
  private readonly maxQueue: number;
  private readonly onQueueChange:
    | ((pending: readonly HonchEvent[]) => void)
    | undefined;

  private context: Context;
  private queue: HonchEvent[];

  private readonly timer: ReturnType<typeof setInterval> | undefined;
  private flushPromise: Promise<void> | null = null;
  private closed = false;

  constructor(opts: HonchClientOptions) {
    if (!opts.apiKey) throw new Error("apiKey is required");

    this.apiKey = opts.apiKey;
    this.host = (opts.host ?? "https://i.honch.io").replace(/\/+$/, "");
    this.timeoutMs = opts.timeoutMs ?? 10_000;
    this.maxRetries = opts.maxRetries ?? 8;
    this.flushEventThreshold = opts.flushEventThreshold ?? 30;
    this.maxQueue = opts.maxQueue ?? 500;
    this.onQueueChange = opts.onQueueChange;

    // Rehydrate any events persisted before the last shutdown.
    this.queue = opts.initialQueue ? [...opts.initialQueue] : [];

    // The context mirrors the wire shape. distinct_id is identity; the $-keys
    // are the paired device's dimensions the server promotes into each event.
    this.context = {
      $device_id: opts.deviceId,
      $device_model: opts.deviceModel,
      $firmware_version: opts.firmwareVersion,
      $sdk_platform: opts.sdkPlatform ?? SDK_PLATFORM,
      $sdk_version: opts.sdkVersion ?? SDK_VERSION,
    };
    // Anonymous identity defaults to the device id (the canonical model:
    // distinct_id == device_id until the user is identified).
    this.context.distinct_id = opts.distinctId ?? opts.deviceId;
    if (opts.environment !== undefined) this.context.$environment = opts.environment;
    if (opts.sessionId !== undefined) this.context.$session_id = opts.sessionId;

    // Background flush on an interval. Cleared in close().
    const intervalMs = (opts.flushIntervalSeconds ?? 60) * 1000;
    this.timer = setInterval(() => {
      // Fire-and-forget; flush() handles its own errors.
      void this.flush();
    }, intervalMs);
    // Do not keep a Node process alive solely for this timer.
    (this.timer as { unref?: () => void }).unref?.();
  }

  // -- public identity / context -----------------------------------------

  /**
   * Identify the current (anonymous) person as `distinctId`, e.g. a user id.
   *
   * This is the ONLY thing that stitches the anonymous device history to the
   * user. It emits a `$identify` event carrying `$anon_distinct_id` = the
   * previous distinct_id, which tells Honch to MERGE the anonymous person into
   * the identified one. Changing the distinct_id WITHOUT this event would create
   * a second, unconnected person. `setProps`/`setOnce` set person properties
   * (`$set` overwrites, `$set_once` only fills gaps). Queued events are flushed
   * first so they keep the previous identity.
   */
  async identify(
    distinctId: string,
    setProps?: Record<string, PropertyValue>,
    setOnce?: Record<string, PropertyValue>,
  ): Promise<void> {
    if (!distinctId) throw new Error("distinctId must be a non-empty string");

    const previous = this.context.distinct_id;
    if (previous === distinctId) {
      this.setPersonProperties(setProps, setOnce);
      return;
    }

    // Send anything queued under the previous identity before switching.
    await this.flush();

    const props: Record<string, PropertyValue> = {};
    if (previous) props.$anon_distinct_id = previous;
    if (setProps) props.$set = setProps;
    if (setOnce) props.$set_once = setOnce;

    this.context.distinct_id = distinctId;
    this.track("$identify", Object.keys(props).length ? props : undefined);
  }

  /**
   * Update the current person's properties without changing identity, via a
   * `$set` event (`$set` overwrites, `$set_once` only fills gaps).
   */
  setPersonProperties(
    setProps?: Record<string, PropertyValue>,
    setOnce?: Record<string, PropertyValue>,
  ): void {
    const props: Record<string, PropertyValue> = {};
    if (setProps) props.$set = setProps;
    if (setOnce) props.$set_once = setOnce;
    if (Object.keys(props).length) this.track("$set", props);
  }

  /** Set or clear the $session_id carried in context for future events. */
  setSession(sessionId: string | undefined): void {
    if (sessionId === undefined) {
      delete this.context.$session_id;
    } else {
      this.context.$session_id = sessionId;
    }
  }

  // -- core tracking ------------------------------------------------------

  /**
   * Queue an event.
   *
   * `event` must be a non-empty string. `properties` are per-event keys; do NOT
   * reuse promoted context keys ($device_id, $sdk_version, ...) here — the
   * server rejects per-event overrides of promoted context.
   *
   * `timestampMs` is epoch milliseconds. As a relay, pass the DEVICE's event
   * time (when the device generated the event), not the time the phone received
   * it — so funnels and timelines stay correct even when the phone was offline.
   * If omitted we stamp NOW (call time).
   */
  track(
    event: string,
    properties?: Record<string, PropertyValue>,
    timestampMs?: number,
  ): void {
    if (!event) throw new Error("event must be a non-empty string");
    if (this.closed) throw new Error("client is closed");

    const record: HonchEvent = {
      event,
      timestamp: timestampMs ?? nowMs(),
    };
    // Only attach properties when there are some, keeping the wire body minimal
    // (matching fixtures where lifecycle events omit properties).
    if (properties && Object.keys(properties).length > 0) {
      record.properties = { ...properties };
    }

    // Bounded queue: drop the OLDEST event if over capacity. On a phone that
    // cannot reach the network for a long time, dropping old data beats
    // unbounded growth.
    if (this.queue.length >= this.maxQueue) this.queue.shift();
    this.queue.push(record);
    this.persist();

    if (this.queue.length >= this.flushEventThreshold) {
      void this.flush();
    }
  }

  // -- lifecycle helpers --------------------------------------------------
  // Thin wrappers over track() for the recommended lifecycle events.

  /** $device_boot — emit once at startup. prop: reset_reason. */
  deviceBoot(resetReason?: string): void {
    this.track("$device_boot", resetReason !== undefined ? { reset_reason: resetReason } : undefined);
  }

  /** $session_start — prop: session_name. */
  sessionStart(name?: string): void {
    this.track("$session_start", name !== undefined ? { session_name: name } : undefined);
  }

  /** $session_end. */
  sessionEnd(): void {
    this.track("$session_end");
  }

  /** $firmware_update — props: previous_version, new_version. */
  firmwareUpdate(previousVersion: string, newVersion: string): void {
    this.track("$firmware_update", {
      previous_version: previousVersion,
      new_version: newVersion,
    });
  }

  /** $battery_low — emit when battery drops below your threshold. prop: level (int). */
  batteryLow(level: number): void {
    this.track("$battery_low", { level });
  }

  // -- flushing / draining ------------------------------------------------

  /**
   * Send all queued events now. Sends in chunks of MAX_EVENTS_PER_REQUEST; each
   * chunk is retried per the backoff policy and a permanently-rejected chunk is
   * dropped (and logged). Resolves once the queue is drained.
   *
   * Durability: a chunk stays in the (persisted) queue for the WHOLE send +
   * retry sequence, and is removed only once the server has accepted it or it is
   * permanently dropped. So if the app is killed mid-send — even during a long
   * backoff — the events are still persisted and resume on the next start.
   * (Trade-off: a crash in the narrow window after the server accepts but before
   * removal can re-send a chunk = at-least-once delivery.)
   *
   * Concurrent flushes are coalesced onto a single in-flight promise, so every
   * caller — including the interval timer and `close()` — awaits the SAME run
   * and never gets a no-op promise while a send is still in progress.
   */
  flush(): Promise<void> {
    this.flushPromise ??= this.drain().finally(() => {
      this.flushPromise = null;
    });
    return this.flushPromise;
  }

  private async drain(): Promise<void> {
    while (this.queue.length > 0) {
      // Snapshot context + peek one request worth of events (do NOT remove them
      // yet — keep them persisted until the server confirms).
      const context = { ...this.context };
      const chunk = this.queue.slice(0, MAX_EVENTS_PER_REQUEST);
      await this.sendWithRetry(context, chunk);
      // Chunk is now done (accepted or permanently dropped). Remove exactly
      // those event objects by identity — robust against a concurrent track()
      // that shifted/pushed the queue while we were sending.
      const sent = new Set(chunk);
      this.queue = this.queue.filter((e) => !sent.has(e));
      this.persist();
    }
  }

  /** Flush remaining events and stop the background timer. */
  async close(): Promise<void> {
    if (this.closed) return;
    this.closed = true;
    if (this.timer !== undefined) clearInterval(this.timer);
    await this.flush();
  }

  // -- validation (debugging aid) ----------------------------------------

  /**
   * Dry-run POST /capture/validate and return the parsed JSON response. Stores
   * nothing. If `events` is omitted, validates whatever is currently queued
   * (without consuming it). Response: { ok, accepted, rejected, expanded_events, errors }.
   */
  async validate(events?: HonchEvent[]): Promise<ValidateResponse> {
    const context = { ...this.context };
    const batchEvents = events !== undefined ? [...events] : [...this.queue];
    const body = buildBatch(context, batchEvents);
    const { status, payload } = await this.post("/capture/validate", body);
    if (payload === null) {
      return { ok: false, errors: [{ code: "non_json_response", message: `status ${status}` }] };
    }
    return payload as ValidateResponse;
  }

  // -- internals ----------------------------------------------------------

  /** Notify the durable-storage hook of the current pending queue. */
  private persist(): void {
    // Pass a copy so an async saver can serialize it without racing later edits.
    this.onQueueChange?.([...this.queue]);
  }

  /**
   * POST one chunk, retrying retryable failures with backoff. Returns once the
   * chunk is done — accepted, permanently dropped (4xx), or retries exhausted.
   * Never throws; the caller removes the chunk from the queue afterward.
   */
  private async sendWithRetry(context: Context, events: HonchEvent[]): Promise<void> {
    if (events.length === 0) return;
    const body = buildBatch(context, events);

    for (let attempt = 0; attempt <= this.maxRetries; attempt++) {
      const { status, payload } = await this.post("/capture", body);

      if (status === 200) {
        // Partial acceptance: a 200 stores the valid events but may report
        // per-event rejections. Those events are permanently bad — log them,
        // never retry them.
        const p = payload as
          | { accepted?: number; rejected?: number; errors?: unknown }
          | null;
        const accepted = p?.accepted;
        const rejected = p?.rejected ?? 0;
        if (rejected) {
          log(
            `flushed ${events.length} event(s), accepted=${accepted}, ` +
              `rejected=${rejected} (permanent): ${JSON.stringify(p?.errors)}`,
          );
        } else {
          log(`flushed ${events.length} event(s), accepted=${accepted}`);
        }
        return;
      }

      if (!isRetryableStatus(status)) {
        // Permanent failure (400/401/415/422). Surface errors and drop.
        const errors = (payload as ValidateResponse | null)?.errors;
        log(
          `dropping ${events.length} event(s): permanent failure status=${status} ` +
            `errors=${JSON.stringify(errors)}`,
        );
        return;
      }

      // Retryable (429/5xx/network=status 0). Back off unless this was the last attempt.
      if (attempt < this.maxRetries) {
        const delay = backoffDelayMs(attempt);
        log(
          `retrying in ${(delay / 1000).toFixed(1)}s (attempt ${attempt + 1}/` +
            `${this.maxRetries}, status=${status})`,
        );
        await sleep(delay);
      }
    }
    log(`dropping ${events.length} event(s): retries exhausted`);
  }

  /**
   * POST JSON to {host}{path}. Returns { status, payload }. Network/transport
   * failures and timeouts map to status 0 (treated as retryable), mirroring the
   * documented response policy where status 0 == retry.
   */
  private async post(
    path: string,
    body: Batch,
  ): Promise<{ status: number; payload: unknown | null }> {
    // AbortController gives us a request timeout without extra deps.
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), this.timeoutMs);
    try {
      const resp = await fetch(this.host + path, {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
          "X-Honch-Project-Key": this.apiKey,
        },
        body: JSON.stringify(body),
        signal: controller.signal,
      });
      const payload = await readJson(resp);
      return { status: resp.status, payload };
    } catch (err) {
      // Network error, DNS failure, abort/timeout, etc. Status 0 == retry.
      log(`network error POST ${path}: ${String(err)}`);
      return { status: 0, payload: null };
    } finally {
      clearTimeout(timer);
    }
  }
}

/** Read and JSON-parse an HTTP response body; null if empty/unparseable. */
async function readJson(resp: Response): Promise<unknown | null> {
  try {
    const text = await resp.text();
    if (!text) return null;
    return JSON.parse(text);
  } catch {
    return null;
  }
}
