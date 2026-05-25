import honch


client = honch.Honch(
    api_key="your-api-key",
    endpoint_url="https://capture.honch.io",
    device_model="dev-board",
    firmware_version="1.0.0",
    queue_directory="/honch",
    disable_background_flush=True,
)

client.track("queued_before_network", {"source": "boot"})

try:
    client.flush()
except honch.TransportError:
    pass
except honch.CompressionUnavailableError:
    pass
