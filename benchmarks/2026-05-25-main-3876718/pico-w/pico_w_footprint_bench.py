try:
    import gc
    import os
    import sys
    import time
except ImportError:
    import gc
    import os
    import sys
    import utime as time


ENDPOINT_URL = "http://192.168.1.122:8001"
API_KEY = "honch_e2e_test_key"
QUEUE_DIRECTORY = "/honch-bench"
EVENT_COUNT = 20
PAYLOAD_BYTES = 64


def marker(name, **values):
    parts = ["PICO_BENCH", name]
    for key in sorted(values):
        parts.append("%s=%s" % (key, values[key]))
    print(" ".join(parts))


def mem(label):
    gc.collect()
    marker(label, mem_free=gc.mem_free(), mem_alloc=gc.mem_alloc())


def rm_tree(path):
    try:
        entries = os.listdir(path)
    except OSError:
        return
    for entry in entries:
        child = path + "/" + entry
        try:
            rm_tree(child)
            os.rmdir(child)
        except OSError:
            try:
                os.remove(child)
            except OSError:
                pass
    try:
        os.rmdir(path)
    except OSError:
        pass


def now_us():
    return time.ticks_us()


def elapsed_us(start):
    return time.ticks_diff(time.ticks_us(), start)


def main():
    marker("START", endpoint=ENDPOINT_URL, events=EVENT_COUNT, payload_bytes=PAYLOAD_BYTES)
    mem("MEM_BOOT")

    try:
        import network

        sta = network.WLAN(network.STA_IF)
        marker(
            "WIFI",
            active=sta.active(),
            connected=sta.isconnected(),
            ip=sta.ifconfig()[0] if sta.active() else "none",
            status=sta.status() if sta.active() else "none",
        )
    except Exception as exc:
        marker("WIFI_ERROR", error=repr(exc))

    import _honch_core
    import honch

    honch.__path__ = "/lib/honch"
    from honch.client import Honch

    marker("IMPORT_OK", core=str(_honch_core), honch_path=honch.__path__)
    mem("MEM_AFTER_IMPORT")

    rm_tree(QUEUE_DIRECTORY)
    mem("MEM_AFTER_QUEUE_CLEANUP")

    start = now_us()
    client = Honch(
        api_key=API_KEY,
        endpoint_url=ENDPOINT_URL,
        device_id=None,
        device_model="Raspberry Pi Pico W",
        firmware_version="pico-bench-main-3876718",
        environment="benchmark",
        queue_directory=QUEUE_DIRECTORY,
        batch_size=10,
        max_queued_events=100,
        max_event_bytes=4096,
        transport_timeout_ms=15000,
        disable_background_flush=True,
        battery_low_threshold=20,
    )
    marker("INIT", us=elapsed_us(start), device_id=client.get_device_id())
    mem("MEM_AFTER_INIT")

    payload = "x" * PAYLOAD_BYTES
    failures = 0
    total_us = 0
    min_us = 999999999
    max_us = 0
    for seq in range(EVENT_COUNT):
        start = now_us()
        try:
            client.track("pico_w_benchmark", {"seq": seq, "payload": payload})
        except Exception as exc:
            failures += 1
            marker("TRACK_ERROR", seq=seq, error=repr(exc))
            continue
        took = elapsed_us(start)
        total_us += took
        if took < min_us:
            min_us = took
        if took > max_us:
            max_us = took

    avg_us = total_us // (EVENT_COUNT - failures) if EVENT_COUNT != failures else 0
    marker(
        "TRACK_SUMMARY",
        count=EVENT_COUNT,
        failures=failures,
        avg_us=avg_us,
        min_us=min_us if failures != EVENT_COUNT else 0,
        max_us=max_us,
    )
    mem("MEM_AFTER_TRACK")

    start = now_us()
    flush_error = ""
    try:
        client.flush()
    except Exception as exc:
        flush_error = repr(exc)
    marker("FLUSH", us=elapsed_us(start), error=flush_error or "none")
    mem("MEM_AFTER_FLUSH")

    start = now_us()
    shutdown_error = ""
    try:
        client.shutdown()
    except Exception as exc:
        shutdown_error = repr(exc)
    marker("SHUTDOWN", us=elapsed_us(start), error=shutdown_error or "none")
    mem("MEM_AFTER_SHUTDOWN")
    marker("DONE")


main()
