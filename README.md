# Roformer Source Separation (DeaDBeeF Plugin)

`roformer.so` is an out-of-tree DeaDBeeF decoder plugin that plays source-separated output from the Mel-Band-Roformer-Vocal-Model project.

It can switch playback between:
- original source (`🔊`)
- instrumental (`🎸`)
- vocal (`🎤`)

## Features

- External plugin only (no DeaDBeeF core/source modifications required).
- Statusbar toggle button with emoji mode indicator.
- Action: `Playback/Toggle Roformer Mode`.
- Direct inference execution via `inference.py` streaming float32 PCM.
- Cache in `/tmp/deadbeef-$UID-roformer-cache`.
- Persistent `index.txt` cache map across restarts.
- Cache size limit in MB (`roformer.cache_limit_mb`, default 1024).
- During precache overflow, oldest cache is evicted unless it belongs to the current playing entry or any entry after it in the current playlist; in that case precaching pauses.
- Optional verbose logging to DeaDBeeF internal log (`roformer.trace=1`).

## Build

From `roformer-plugin/`:

```bash
make
```

## Install

```bash
make install
```

This installs `roformer.so` to:

`~/.local/lib/deadbeef/`

Restart DeaDBeeF after install/update.

## Runtime Requirements

- Python 3 (default executable: `python3`)
- Mel-Band-Roformer model directory containing:
  - `inference.py`
  - `configs/config_vocals_mel_band_roformer.yaml`
  - `MelBandRoformer.ckpt`

### Model Setup (required)

1. Clone:

```bash
git clone https://github.com/nonoo/Mel-Band-Roformer-Vocal-Model
```

2. Download checkpoint:

`https://huggingface.co/KimberleyJSN/melbandroformer/blob/main/MelBandRoformer.ckpt`

3. Place `MelBandRoformer.ckpt` into the cloned `Mel-Band-Roformer-Vocal-Model` directory.

4. In DeaDBeeF plugin settings, set **Mel-Band-Roformer-Vocal-Model directory** to that cloned directory path.

## Plugin Configuration

In DeaDBeeF plugin settings:

- **Mel-Band-Roformer-Vocal-Model directory** (`roformer.model_dir`)
- **Enable verbose logging** (`roformer.trace`, default off)
- **Python executable** (`roformer.python`, default `python3`)
- **Cache size limit (MB)** (`roformer.cache_limit_mb`, default `1024`)

## Modes and Behavior

- Default mode on startup is `🔊` (original).
- When mode is `🎸` or `🎤`, playback uses roformer decode path.
- New tracks are auto-routed through roformer when non-original mode is active.
- Seeking is disabled while inference is in progress (except seek to 0).
- Full seeking is enabled after inference is finished, and immediately for cache hits.

## Cache Format

Cache files are WAV (`.wav`) with float32 PCM (`WAVE`, format code 3).

Each cache file starts with a 44-byte WAV header and data chunk sizes are patched when inference completes successfully.

`index.txt` schema:

```text
# mode	mtime	size	samplerate	channels	complete	cache_file	uri
```

Field notes:
- `mode`: `instr` for instrumental, `vocal` for vocal
- `mtime`,`size`: source file stat used for cache key validity
- `samplerate`,`channels`: playback format
- `complete`: `1` means reusable complete cache, `0` incomplete
- `cache_file`: absolute path to cache `.wav`
- `uri`: source track URI/path

Only `complete=1` entries are used as cache hits.

On startup, orphan `.wav` files not referenced by `index.txt` are removed.

If playback stops before inference completes, incomplete cache is removed.

## Troubleshooting

- Enable verbose logging (`roformer.trace=1`) and inspect DeaDBeeF log.
- Verify `roformer.model_dir` points to the correct model repo directory.
- Verify `python3` (or configured executable) can run `inference.py`.
- If needed, clear cache directory:
  - `/tmp/deadbeef-$UID-roformer-cache`
