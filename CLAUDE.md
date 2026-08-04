# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

An ESPHome external component (`kasa_plug`) that lets an ESP32/ESP8266 board control TP-Link Kasa smart plugs (HS/KP/EP model prefixes) over their local TCP protocol, exposing each plug as an ESPHome/Home Assistant `switch` entity. It is a port of the KasaSmartPlug Arduino library. Plugs whose firmware only speaks the newer KLAP-over-HTTP protocol are not supported.

## Commands

There is no standalone build or test suite; validation goes through the ESPHome CLI against `example.yaml`:

```bash
esphome config example.yaml     # validate schema + run Python codegen (fast check)
esphome compile example.yaml    # full C++ build for the target board
esphome run example.yaml        # compile + flash
```

To build against the local checkout instead of GitHub, `example.yaml` contains a commented-out local `external_components` source (`type: local, path: components`) — switch to that while iterating, since the default source pulls from `github://tonymacdonald1995/esphome-kasa`.

## Architecture

Everything lives in `components/kasa_plug/`, split along ESPHome's standard config-time (Python) / runtime (C++) boundary:

- `__init__.py` — declares the `kasa_plug` C++ namespace and CODEOWNERS.
- `switch.py` — config schema (`host` required, `port` default 9999, polling default 30s) and `to_code()` codegen that instantiates `KasaPlugSwitch` and applies the config. Declares `DEPENDENCIES = ["network"]` and `AUTO_LOAD = ["async_tcp", "json"]`.
- `kasa_plug.h` / `kasa_plug.cpp` — `KasaPlugSwitch`, which inherits both `switch_::Switch` (for `write_state()`) and `PollingComponent` (for `update()`).

Runtime behavior, all in `kasa_plug.cpp`:

- **Protocol:** Kasa devices speak plain JSON on TCP 9999, obfuscated with an XOR-autokey cipher (initial key `0xAB` / 171) and framed by a 4-byte big-endian length header. `encrypt_()`/`decrypt_()` implement this; commands are the string constants `CMD_GET_SYSINFO` / `CMD_RELAY_ON` / `CMD_RELAY_OFF`.
- **Polling:** `update()` starts a `GET_STATE` transaction (`get_sysinfo`), and on completion `process_response_()` parses `system.get_sysinfo.relay_state` and publishes it — so changes made via the plug's button or the Kasa app get reflected.
- **Writes:** `write_state()` records the desired state in `pending_write_` and starts a `SET_STATE` transaction if idle; a toggle that arrives mid-transaction is sent by `finish_transaction_()` right after. State is published only after the plug's reply confirms `err_code == 0`.
- **Transport:** ESPHome's core `async_tcp` component (`AsyncClient`), the officially supported cross-platform TCP client — AsyncTCP libraries on ESP32/ESP8266/RP2040/LibreTiny, a socket-based impl elsewhere. Do *not* use Arduino `WiFiClient` (breaks ESP32-IDF builds) or `esphome::socket` directly for outbound connections (the raw-lwIP impl on ESP8266/RP2040 has no `connect()`).
- **Concurrency model:** one transaction (connect → send one framed command → receive one framed reply) at a time. `AsyncClient` callbacks may run outside the main loop (dedicated task on ESP32/LibreTiny), so they only append to `rx_buf_` / set flags under `lock_` (an `esphome::Mutex`, no-op on single-threaded platforms); all decisions happen in `loop()`, which is `disable_loop()`d while idle. A 2 s transaction deadline replaces per-step timeouts; failures set the component warning status. Keep any new I/O inside this state machine — never block the main loop.
