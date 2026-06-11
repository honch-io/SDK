import honch


client = honch.Honch(
    api_key="your-api-key",
    endpoint_url="https://i.honch.io",
    device_id="dev-board-001",
    device_model="dev-board",
    firmware_version="1.0.0",
    event_buffer=bytearray(8192),
)

client.identify("user-123", {"plan": "beta"})
client.session_start("demo")
client.track("button_pressed", {"button": "boot"})
client.session_end()
client.flush()
client.shutdown()
