# AVCF: Analog Voice Capture Format

**Date:** 2026-03-28
**Status:** Approved

## Problem

The `.dvcf` format captures pre-vocoder codec frames for digital P25/DMR calls, making them self-contained for ASR dataset building. But trunk-recorder also records analog (conventional FM) calls, and those recordings have no equivalent self-contained format — they're just `.wav` files with no embedded metadata.

Users building ASR training datasets for radio transmissions need both digital and analog calls. Analog calls should be just as easy to batch-upload as digital calls: one file per call, audio + metadata included.

## Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Unified or sibling format? | **Sibling** (`.avcf`) | Digital and analog go through completely different processing pipelines (IMBE-ASR vs standard ASR). Separate formats, separate plugins, separate upload paths. |
| Audio payload | **Opaque blob with MIME type** | Container is waveform-agnostic (WAV, FLAC, M4A, Vorbis). Content-type string identifies the format. No transcoding. |
| Plugin architecture | **Separate plugin** (`mqtt_avcf`) | Different data acquisition pattern — no incremental streaming, everything at call_end. Simpler than mqtt_dvcf. No risk of breaking the digital path. |
| MQTT payload | **Full .avcf as base64** | Matches mqtt_dvcf pattern (`audio_avcf_base64` + `metadata`). Consistent with mqtt_status plugin's `audio_wav_base64` convention. |

## AVCF File Format

Built on SSSP v2 framing (same as `.dvcf`). Reuses existing message types unchanged and adds one new message type.

### File Structure

```
1. CALL_START      (msg_type=0x02, binary — identical to .dvcf)
2. AUDIO_DATA      (msg_type=0x06, content-type + audio blob — NEW)
3. CALL_METADATA   (msg_type=0x05, JSON — identical to .dvcf)
4. CALL_END        (msg_type=0x03, binary — identical to .dvcf)
```

Receivers must dispatch on `msg_type`, not position.

### Shared Message Types

CALL_START (0x02), CALL_METADATA (0x05), and CALL_END (0x03) are defined in the DVCF spec and used identically here. See `DVCF_SPEC.md` §3.3, §3.6, and §3.4.

### New Message Type: AUDIO_DATA (msg_type = 0x06)

Carries an opaque audio file with a MIME content-type identifier.

```
[8 bytes: SSSP v2 header]
  magic[0]    = 0x53 ('S')
  magic[1]    = 0x59 ('Y')
  version     = 0x02
  msg_type    = 0x06
  payload_len = N (uint32, little-endian)

[1 byte: content_type_len (M)]
[M bytes: content_type (UTF-8 MIME string, e.g. "audio/wav")]
[N-1-M bytes: raw audio file bytes]
```

**Content-type values:**

| Extension | MIME Type | Notes |
|-----------|-----------|-------|
| `.wav` | `audio/wav` | Default trunk-recorder output |
| `.m4a` | `audio/mp4` | AAC compressed |
| `.flac` | `audio/flac` | Lossless compressed |
| `.ogg` | `audio/ogg` | Vorbis compressed |

The audio bytes are the raw file contents — no transcoding, no reframing. A consumer extracts them and writes directly to a file with the appropriate extension.

**Rules:**
- `content_type_len` must be > 0 (content type is required).
- Receivers that don't recognize the content type should skip the message via `payload_len`.
- The audio blob is the complete file (including any format-specific headers like WAV RIFF headers).

### File Extension and MIME Type

- Extension: **`.avcf`** (Analog Voice Capture Format)
- MIME type: `application/x-avcf` (unregistered)

### File Naming Convention

Same convention as `.dvcf` — sidecar alongside the audio file:

```
9173-1774473273.232_852687500.0-call_39.wav   (audio)
9173-1774473273.232_852687500.0-call_39.avcf  (analog voice capture)
```

## Plugin: mqtt_avcf

### Architecture

A trunk-recorder `user_plugins` shared library. Much simpler than `mqtt_dvcf` — no incremental streaming, no per-call state, no mutex. Everything happens at `call_end`.

### Callbacks

| Callback | Purpose |
|----------|---------|
| `parse_config()` | Read config (same schema as mqtt_dvcf) |
| `start()` | Create MQTT connection if enabled |
| `stop()` | Disconnect MQTT, cleanup |
| `call_end(Call_Data_t)` | All work: read audio, build .avcf, write/publish |

`call_start` and `voice_codec_data` are not used.

### call_end Flow

1. **Filter:** Skip if not analog (check `call_info.audio_type` or analog flag). Skip if `call_info.filename` is empty.
2. **Read audio file:** Read the completed audio file from `call_info.filename` into memory.
3. **Detect content type:** Map file extension to MIME type (`".wav"` → `"audio/wav"`, etc.).
4. **Build .avcf in memory:**
   - Emit CALL_START from call_info fields
   - Emit AUDIO_DATA (content-type + audio bytes)
   - Emit CALL_METADATA (JSON from Call_Data_t, identical to mqtt_dvcf)
   - Emit CALL_END from call_info fields
5. **Write file** (if `write_enabled`): Write `.avcf` sidecar (same base name, `.avcf` extension).
6. **Publish MQTT** (if `mqtt_enabled`): Base64-encode and publish to `{topic}/avcf`.

### Configuration

```json
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
```

| Option | Default | Description |
|--------|---------|-------------|
| `write_enabled` | `true` | Write `.avcf` sidecar files to disk |
| `mqtt_enabled` | `false` | Publish to MQTT broker |
| `analog_only` | `true` | Only process analog calls (skip digital) |
| `broker` | `tcp://localhost:1883` | MQTT broker URL |
| `topic` | `trunk-recorder` | MQTT topic prefix (publishes to `{topic}/avcf`) |
| `clientid` | `avcf-handler` | MQTT client ID |
| `username` | `""` | MQTT username (optional) |
| `password` | `""` | MQTT password (optional) |
| `qos` | `0` | MQTT QoS level |

### MQTT Payload

Published to `{topic}/avcf`:

```json
{
  "audio_avcf_base64": "<base64-encoded .avcf file>",
  "metadata": {
    "talkgroup": 9170,
    "talkgroup_tag": "Fire Dispatch",
    "talkgroup_alpha_tag": "BUTCO FD",
    "talkgroup_group": "Fire",
    "freq": 855737500,
    "start_time": 1711234567,
    "stop_time": 1711234590,
    "call_length": 23,
    "signal": -42.5,
    "noise": -110.2,
    "freq_error": 12,
    "spike_count": 0,
    "emergency": false,
    "priority": 3,
    "phase2_tdma": false,
    "tdma_slot": 0,
    "analog": true,
    "audio_type": "analog",
    "short_name": "butco",
    "filename": "9170-1711234567_855737500.wav",
    "srcList": [
      {"src": 1234567, "time": 1711234567, "pos": 0.0, "emergency": 0, "signal_system": "", "tag": ""}
    ]
  }
}
```

## Cross-Project Changes

### tr-plugin-dvcf (this repo)

Register AUDIO_DATA (0x06) in `DVCF_SPEC.md` §5 message type table:

```
| 0x06         | `audio_data`   | Opaque audio blob (see AVCF spec) |
```

Update reserved range to `0x07–0xFF`.

### New repo: tr-plugin-avcf

- `mqtt_avcf.cc` — plugin source
- `CMakeLists.txt` — same dependencies as mqtt_dvcf
- `AVCF_SPEC.md` — this spec
- `README.md` — build instructions, config, usage

### Downstream: tr-engine

Needs a new route: `audio_avcf_base64` on `{topic}/avcf` → standard ASR provider (Whisper, etc.), not IMBE-ASR.

### No changes: IMBE-ASR

IMBE-ASR only consumes `.dvcf` files. Analog calls are not relevant.
