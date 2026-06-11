import honch


client = honch.Honch(
    api_key="your-api-key",
    endpoint_url="https://i.honch.io",
    device_id="dev-board-001",
    device_model="dev-board",
    firmware_version="1.0.0",
    event_buffer=bytearray(8192),
)

client.track("queued_before_network", {"source": "boot"})

try:
    client.flush()
except honch.TransportError:
    pass
except honch.CompressionUnavailableError:
    pass
