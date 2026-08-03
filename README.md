# ESPHome Kasa Plug

An [ESPHome](https://esphome.io/) external component for controlling **TP-Link
Kasa smart plugs** (HS100/HS103/HS105/HS110, KP105/KP115, EP10, and similar)
over your local network — no TP-Link cloud account and no internet required.

It is a port of Kris Jearakul's excellent
[KasaSmartPlug](https://github.com/kj831ca/KasaSmartPlug) Arduino library to the
ESPHome component API, so an ESP board on your network can expose Kasa plugs as
native ESPHome/Home Assistant `switch` entities.

## How it works

Kasa devices speak a simple JSON protocol on **TCP port 9999**, lightly
obfuscated with an XOR-autokey cipher (initial key `0xAB`) and framed with a
4-byte big-endian length header. This component:

- polls each plug on a configurable interval for its relay state (so external
  changes — the physical button, the Kasa app — are reflected back), and
- sends `set_relay_state` commands when the switch is toggled from
  ESPHome/Home Assistant.

All network I/O is non-blocking with short timeouts, so an unreachable plug logs
a warning and retries on the next poll rather than stalling the device.

## Usage

Add the component and one `switch` entry per plug:

```yaml
external_components:
  - source: github://tonymacdonald1995/esphome-kasa
    components: [kasa_plug]

switch:
  - platform: kasa_plug
    name: "Living Room Plug"
    host: 192.168.1.42
```

See [`example.yaml`](example.yaml) for a complete, flashable configuration.

### Configuration variables

| Option            | Required | Default | Description                                                                 |
| ----------------- | -------- | ------- | --------------------------------------------------------------------------- |
| `host`            | **yes**  | —       | IP address (recommended) or hostname of the plug on your LAN.               |
| `port`            | no       | `9999`  | TCP port of the Kasa protocol. Only change for unusual setups.              |
| `update_interval` | no       | `30s`   | How often to poll the plug for its current relay state.                     |
| `name`, `id`, `icon`, `restore_mode`, … | — | — | All the standard ESPHome [Switch](https://esphome.io/components/switch/) options are supported. |

> **Tip:** give each plug a static DHCP lease so its `host` doesn't change.

## Compatibility

- **ESP boards:** ESP32 and ESP8266 (uses ESPHome's cross-platform socket layer).
- **Plugs:** the classic local-protocol Kasa devices (model prefixes `HS`, `KP`,
  `EP`). Newer firmware that only speaks the encrypted **KLAP** protocol over
  HTTP is **not** supported.

## Development

To iterate locally, point `external_components` at the checked-out repo and
validate the config:

```yaml
external_components:
  - source:
      type: local
      path: components
    components: [kasa_plug]
```

```bash
esphome config example.yaml     # validate schema + codegen
esphome compile example.yaml    # full build
```

## Credits

- [KasaSmartPlug](https://github.com/kj831ca/KasaSmartPlug) by Kris Jearakul —
  the original Arduino library this component is ported from.
- The TP-Link HS110 protocol
  [reverse-engineering write-up](https://www.softscheck.com/en/reverse-engineering-tp-link-hs110/)
  by softScheck.
