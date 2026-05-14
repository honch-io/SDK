import struct


def encode(value):
    out = bytearray()
    _encode_value(out, value)
    return bytes(out)


def decode(data):
    if isinstance(data, str):
        data = data.encode("latin1")
    reader = _Reader(data)
    value = reader.read_value()
    if reader.offset != len(reader.data):
        raise ValueError("trailing CBOR data")
    return value


def _append_type(out, major, value):
    if value < 24:
        out.append((major << 5) | value)
    elif value < 256:
        out.extend(((major << 5) | 24, value))
    elif value < 65536:
        out.append((major << 5) | 25)
        _append_uint(out, value, 2)
    elif value < 4294967296:
        out.append((major << 5) | 26)
        _append_uint(out, value, 4)
    elif value < 18446744073709551616:
        out.append((major << 5) | 27)
        _append_uint(out, value, 8)
    else:
        raise OverflowError("CBOR integer is too large")


def _append_uint(out, value, length):
    for shift in range((length - 1) * 8, -1, -8):
        out.append((value >> shift) & 0xFF)


def _encode_value(out, value):
    if value is None:
        out.append(0xF6)
    elif value is True:
        out.append(0xF5)
    elif value is False:
        out.append(0xF4)
    elif isinstance(value, int):
        if value >= 0:
            _append_type(out, 0, value)
        else:
            _append_type(out, 1, -1 - value)
    elif isinstance(value, float):
        out.append(0xFB)
        out.extend(struct.pack(">d", value))
    elif isinstance(value, str):
        data = value.encode("utf-8")
        _append_type(out, 3, len(data))
        out.extend(data)
    elif isinstance(value, (list, tuple)):
        _append_type(out, 4, len(value))
        for item in value:
            _encode_value(out, item)
    elif isinstance(value, dict):
        _append_type(out, 5, len(value))
        for key, item in value.items():
            if not isinstance(key, str):
                raise TypeError("CBOR map keys must be strings")
            _encode_value(out, key)
            _encode_value(out, item)
    else:
        raise TypeError("unsupported CBOR value")


class _Reader:
    def __init__(self, data):
        self.data = data
        self.offset = 0

    def read_value(self):
        initial = self._read_u8()
        major = initial >> 5
        additional = initial & 0x1F
        if major == 0:
            return self._read_uint(additional)
        if major == 1:
            return -1 - self._read_uint(additional)
        if major == 3:
            length = self._read_uint(additional)
            return self._read(length).decode("utf-8")
        if major == 4:
            length = self._read_uint(additional)
            return [self.read_value() for _ in range(length)]
        if major == 5:
            length = self._read_uint(additional)
            result = {}
            for _ in range(length):
                key = self.read_value()
                if not isinstance(key, str):
                    raise ValueError("CBOR map key is not text")
                result[key] = self.read_value()
            return result
        if major == 7:
            if additional == 20:
                return False
            if additional == 21:
                return True
            if additional == 22:
                return None
            if additional == 27:
                return struct.unpack(">d", self._read(8))[0]
        raise ValueError("unsupported CBOR value")

    def _read_uint(self, additional):
        if additional < 24:
            return additional
        if additional == 24:
            return self._read_u8()
        if additional == 25:
            return self._read_uint_bytes(2)
        if additional == 26:
            return self._read_uint_bytes(4)
        if additional == 27:
            return self._read_uint_bytes(8)
        raise ValueError("unsupported CBOR integer")

    def _read_uint_bytes(self, length):
        value = 0
        for item in self._read(length):
            value = (value << 8) | item
        return value

    def _read_u8(self):
        return self._read(1)[0]

    def _read(self, length):
        end = self.offset + length
        if end > len(self.data):
            raise ValueError("truncated CBOR data")
        chunk = self.data[self.offset:end]
        self.offset = end
        return chunk
