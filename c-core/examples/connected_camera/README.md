# Connected Camera Example

This example simulates a connected action camera using the C/POSIX Honch SDK.

It demonstrates:

- generated persistent `device_id`
- user identification
- persistent event context with `honch_set_property`
- realistic hardware events
- explicit flush to the local mock collector

## Run

From the repository root, build examples:

```sh
cmake -S . -B build -DHONCH_BUILD_TESTS=ON -DHONCH_BUILD_EXAMPLES=ON
cmake --build build
```

Start the mock collector:

```sh
python3 tools/mock_collector.py --port 8765
```

Run the camera simulation:

```sh
./build/examples/connected_camera/honch_connected_camera_example
```

The mock collector prints only a summary, such as:

```json
{"accepted": 5}
```

The example stores local SDK state and queued events under:

```text
.honch-connected-camera-queue/
```
