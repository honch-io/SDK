# Connected Camera Example

This example simulates a connected action camera using the C/POSIX Honch SDK.

It demonstrates:

- generated persistent `device_id`
- user identification
- `$set_property` event emission with `honch_set_property`
- realistic hardware events
- explicit flush to the configured capture endpoint

## Run

From the repository root, build examples:

```sh
cmake -S . -B build -DHONCH_BUILD_TESTS=ON -DHONCH_BUILD_EXAMPLES=ON
cmake --build build
```

Set the capture endpoint and API key:

```sh
export HONCH_CAPTURE_ENDPOINT="http://127.0.0.1:8001"
export HONCH_API_KEY="honch_..."
```

Run the camera simulation:

```sh
./build/example/connected_camera/honch_connected_camera_example
```

The example posts to `<HONCH_CAPTURE_ENDPOINT>/batch` and defaults to the local
capture service at `http://127.0.0.1:8001`.

The example stores local SDK state and queued events under:

```text
.honch-connected-camera-queue/
```
