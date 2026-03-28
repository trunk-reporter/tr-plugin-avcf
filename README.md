# tr-plugin-avcf

Trunk Recorder plugin that captures analog voice recordings as self-contained `.avcf` files and/or publishes them over MQTT.

This is the analog companion to [tr-plugin-dvcf](https://github.com/trunk-reporter/tr-plugin-dvcf). Where `.dvcf` captures raw codec frames for digital P25/DMR calls, `.avcf` wraps analog audio recordings with full call metadata in the same SSSP v2 container format.

## Why

Standard trunk-recorder analog recordings are plain `.wav` files with no embedded metadata. To build an ASR training dataset, you need to correlate audio files with talkgroup labels, signal quality, speaker information, and other call-level data from external sources.

`.avcf` files are self-contained: one file per call, audio + metadata included. Upload a batch of `.avcf` files and you have a complete dataset.

## Features

- **File writing** — saves `.avcf` sidecar files alongside audio recordings (same base name, `.avcf` extension)
- **MQTT publishing** — publishes analog call data as `audio_avcf_base64` over MQTT
- **Waveform agnostic** — wraps WAV, FLAC, M4A, or OGG without transcoding
- **Analog filtering** — only processes analog calls by default (configurable)

## File Format

`.avcf` = Analog Voice Capture Format. SSSP v2 binary container — see [AVCF_SPEC.md](AVCF_SPEC.md) for the full specification.

Each file contains:
1. `CALL_START` — call identification and timing
2. `AUDIO_DATA` — opaque audio blob with MIME content-type
3. `CALL_METADATA` — JSON with talkgroup labels, signal/noise, speaker list, etc.
4. `CALL_END` — call summary

## Requirements

- Trunk Recorder v5.0+ with plugin API support
- Paho MQTT C++ (for MQTT publishing)
- Boost (already required by trunk-recorder)

## Building

Builds as a `user_plugins` drop-in — no fork of trunk-recorder required.

```bash
# 1. Clone trunk-recorder
git clone https://github.com/TrunkRecorder/trunk-recorder.git
cd trunk-recorder

# 2. Drop this plugin into user_plugins/
mkdir -p user_plugins
git clone https://github.com/trunk-reporter/tr-plugin-avcf user_plugins/mqtt_avcf

# 3. Build with local plugins enabled
cmake -B build -DUSE_LOCAL_PLUGINS=ON
cmake --build build -j$(nproc)

# 4. Install
sudo cmake --install build
```

### Dependencies

```bash
# Ubuntu/Debian
sudo apt-get install libpaho-mqtt3as-dev libpaho-mqttpp3-dev
```

## Configuration

Add to your trunk-recorder `config.json`:

```json
{
  "plugins": [
    {
      "name": "mqtt_avcf",
      "library": "libmqtt_avcf",
      "write_enabled": true,
      "mqtt_enabled": false,
      "analog_only": true,
      "broker": "tcp://localhost:1883",
      "topic": "trunk-recorder",
      "clientid": "avcf-handler",
      "username": "",
      "password": "",
      "qos": 0
    }
  ]
}
```

| Option | Default | Description |
|---|---|---|
| `write_enabled` | `true` | Write `.avcf` sidecar files to disk |
| `mqtt_enabled` | `false` | Publish analog call data over MQTT |
| `analog_only` | `true` | Only process analog calls (skip digital) |
| `broker` | `tcp://localhost:1883` | MQTT broker URL |
| `topic` | `trunk-recorder` | MQTT topic prefix (publishes to `{topic}/avcf`) |
| `clientid` | `avcf-handler` | MQTT client ID |
| `username` | `""` | MQTT username (optional) |
| `password` | `""` | MQTT password (optional) |
| `qos` | `0` | MQTT QoS level |

## MQTT Message Format

When `mqtt_enabled: true`, the plugin publishes a JSON message on `{topic}/avcf`:

```json
{
  "audio_avcf_base64": "<base64-encoded .avcf content>",
  "metadata": {
    "talkgroup": 9170,
    "talkgroup_tag": "Fire Dispatch",
    "talkgroup_group": "Fire",
    "freq": 855737500,
    "start_time": 1711234567,
    "stop_time": 1711234590,
    "call_length": 23,
    "signal": -42.5,
    "noise": -110.2,
    "emergency": false,
    "analog": true,
    "audio_type": "analog",
    "short_name": "butco",
    "filename": "9170-1711234567_855737500.wav",
    "srcList": [{"src": 1234567, "time": 1711234567, "pos": 0.0, "emergency": 0, "signal_system": "", "tag": ""}]
  }
}
```

## Related Projects

- [tr-plugin-dvcf](https://github.com/trunk-reporter/tr-plugin-dvcf) — sibling plugin for digital P25/DMR codec frames
- [IMBE-ASR](https://github.com/trunk-reporter/imbe-asr) — ASR model for digital calls (reads .dvcf files)
- [tr-engine](https://github.com/trunk-reporter/tr-engine) — backend that ingests MQTT and routes to ASR providers
