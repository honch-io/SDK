from . import platform as fs


class IdentityStore:
    def __init__(self, state_dir, platform, configured_device_id=None):
        self.state_dir = state_dir
        self.platform = platform
        fs.ensure_dir(state_dir)
        self.device_id = self._load_or_create_device_id(configured_device_id)
        self.distinct_id = self._load_or_create_distinct_id()

    def _path(self, name):
        return fs.join_path(self.state_dir, name)

    def _read_optional(self, name):
        try:
            value = fs.read_text(self._path(name))
            return value or None
        except OSError:
            return None

    def _write(self, name, value):
        fs.write_text(self.state_dir, name, value)

    def _load_or_create_device_id(self, configured):
        if configured is not None and str(configured).strip() != "":
            value = str(configured)
            self._write("device_id", value)
            return value
        stored = self._read_optional("device_id")
        if stored:
            return stored
        value = self.platform.random_hex(16)
        self._write("device_id", value)
        return value

    def _load_or_create_distinct_id(self):
        stored = self._read_optional("distinct_id")
        if stored:
            return stored
        self._write("distinct_id", self.device_id)
        return self.device_id

    def set_distinct_id(self, distinct_id):
        previous = self.distinct_id
        self._write("distinct_id", distinct_id)
        self.distinct_id = distinct_id
        return previous

    def restore_distinct_id(self, distinct_id):
        self._write("distinct_id", distinct_id)
        self.distinct_id = distinct_id

    def check_firmware_version(self, current):
        previous = self._read_optional("firmware_version")
        changed = previous is not None and previous != current
        self._write("firmware_version", current)
        return changed, previous

    def reset(self):
        next_id = self.platform.random_hex(16)
        self._write("device_id", next_id)
        self._write("distinct_id", next_id)
        self.device_id = next_id
        self.distinct_id = next_id
