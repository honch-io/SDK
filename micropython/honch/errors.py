class HonchError(Exception):
    pass


class InvalidArgumentError(HonchError):
    pass


class StorageError(HonchError):
    pass


class TransportError(HonchError):
    pass


class RateLimitedError(TransportError):
    pass


class ServerError(TransportError):
    pass


class RejectedError(HonchError):
    pass


class CompressionUnavailableError(HonchError):
    pass


class NotInitializedError(HonchError):
    pass
