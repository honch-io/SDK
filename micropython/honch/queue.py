from . import platform as fs
from .errors import InvalidArgumentError, StorageError


class PersistentQueue:
    def __init__(self, root_dir, platform, max_queued_events, max_event_bytes):
        self.root_dir = root_dir
        self.platform = platform
        self.max_queued_events = max_queued_events
        self.max_event_bytes = max_event_bytes
        self.pending_dir = fs.join_path(root_dir, "pending")
        self.dead_dir = fs.join_path(root_dir, "dead")
        self.state_dir = fs.join_path(root_dir, "state")
        self._sequence = 0
        for directory in (self.pending_dir, self.dead_dir, self.state_dir):
            fs.ensure_dir(directory)
            self._remove_tmp_files(directory)

    def _remove_tmp_files(self, directory):
        for name in fs.list_files(directory, ".tmp"):
            fs.remove_if_exists(fs.join_path(directory, name))

    def enqueue(self, event_json):
        if len(event_json) > self.max_event_bytes:
            raise InvalidArgumentError("event exceeds max_event_bytes")
        while self.count() >= self.max_queued_events and self.count() > 0:
            oldest = self.pending_files()[0]
            fs.remove_if_exists(fs.join_path(self.pending_dir, oldest))

        filename = "%020d-%06d-%s.json" % (
            self.platform.now_ms(),
            self._sequence,
            self.platform.random_hex(16),
        )
        self._sequence += 1
        fs.write_text_atomic(self.pending_dir, filename, event_json)

    def pending_files(self):
        return fs.list_files(self.pending_dir, ".json")

    def count(self):
        return len(self.pending_files())

    def read_batch(self, max_count):
        batch = []
        names = self.pending_files()[:max_count]
        for name in names:
            path = fs.join_path(self.pending_dir, name)
            try:
                text = fs.read_text(path)
            except OSError as exc:
                raise StorageError(str(exc))
            if len(text) > self.max_event_bytes:
                self.move_to_dead([name])
                raise InvalidArgumentError("persisted event exceeds max_event_bytes")
            batch.append((name, text))
        return batch

    def delete_pending(self, names):
        for name in names:
            fs.remove_if_exists(fs.join_path(self.pending_dir, name))

    def move_to_dead(self, names):
        for name in names:
            src = fs.join_path(self.pending_dir, name)
            dst = fs.join_path(self.dead_dir, name)
            try:
                fs.os.rename(src, dst)
            except OSError:
                try:
                    text = fs.read_text(src)
                    fs.write_text_atomic(self.dead_dir, name, text)
                finally:
                    fs.remove_if_exists(src)

    def clear(self):
        for directory in (self.pending_dir, self.dead_dir):
            for name in fs.list_files(directory, ".json"):
                fs.remove_if_exists(fs.join_path(directory, name))
