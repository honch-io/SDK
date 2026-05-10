from .errors import TransportError


class Scheduler:
    def __init__(self, client):
        self.client = client
        self.enabled = not client.config.disable_background_flush
        self.current_retry_delay_ms = client.config.flush_retry_initial_ms
        self.next_retry_ms = 0
        self._periodic = None
        if self.enabled and client.config.flush_interval_seconds > 0:
            starter = getattr(client.platform, "start_periodic", None)
            if starter is not None:
                self._periodic = starter(client.config.flush_interval_seconds * 1000, self.flush_due)

    def after_enqueue(self):
        if not self.enabled:
            return
        if self.client.config.flush_event_threshold <= 0:
            return
        if self.client.queue.count() < self.client.config.flush_event_threshold:
            return
        now = self.client.platform.now_ms()
        if now < self.next_retry_ms:
            return
        try:
            self.client.flush()
            self.current_retry_delay_ms = self.client.config.flush_retry_initial_ms
            self.next_retry_ms = 0
        except TransportError:
            wait = self._next_wait_ms()
            self.next_retry_ms = now + wait
            self.current_retry_delay_ms = min(
                self.current_retry_delay_ms * 2,
                self.client.config.flush_retry_max_ms,
            )

    def flush_due(self):
        if not self.enabled or self.client.queue.count() == 0:
            return
        now = self.client.platform.now_ms()
        if now < self.next_retry_ms:
            return
        try:
            self.client.flush()
            self.current_retry_delay_ms = self.client.config.flush_retry_initial_ms
            self.next_retry_ms = 0
        except TransportError:
            wait = self._next_wait_ms()
            self.next_retry_ms = now + wait
            self.current_retry_delay_ms = min(
                self.current_retry_delay_ms * 2,
                self.client.config.flush_retry_max_ms,
            )

    def _next_wait_ms(self):
        delay = self.current_retry_delay_ms
        quarter = delay // 4
        if quarter <= 0:
            return delay
        jitter_span = quarter * 2 + 1
        jitter = self.client.platform.now_ms() % jitter_span
        return delay - quarter + jitter

    def stop(self):
        self.enabled = False
        stopper = getattr(self._periodic, "stop", None)
        if stopper is not None:
            stopper()
