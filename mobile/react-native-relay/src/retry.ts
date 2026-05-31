const INITIAL_BACKOFF_MS = 1000;
const MAX_BACKOFF_MS = 300000;
const JITTER_RATIO = 0.25;

export function nextBackoffDelayMs(
  attempt: number,
  random: () => number = Math.random
): number {
  const safeAttempt = Math.max(0, attempt);
  const base = Math.min(INITIAL_BACKOFF_MS * 2 ** safeAttempt, MAX_BACKOFF_MS);
  const jitter = 1 - JITTER_RATIO + random() * JITTER_RATIO * 2;
  return Math.min(Math.round(base * jitter), MAX_BACKOFF_MS);
}
