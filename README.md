# Source Separation DeaDBeeF Plugin

`sourcesep.so` is an out-of-tree DeaDBeeF decoder plugin that plays source-separated output.

It can switch playback between:
- original source (`🔊`)
- instrumental (`🎸`)
- vocal (`🎤`)

## Features

- External plugin only (no DeaDBeeF core/source modifications required).
- Statusbar toggle button with emoji mode indicator.
- Action: `Playback/Toggle Source Separation Mode`.
- Direct inference execution via a script (e.g. `inference.py`) streaming float32 PCM.
- Cache in `/tmp/deadbeef-$UID-sourcesep-cache`.
- Live raw temp caches (`*.f32tmp*`) are stored separately in `/tmp/deadbeef-$UID-sourcesep-cache-f32tmp` (lazy-created, cleaned on startup and exit).
- Cache filename uses the original source filename stem (plus mode suffix): `<name>.instr.mp3` / `<name>.vocal.mp3`.
- Cache directory is created lazily on first write (not on plugin load).
- Persistent `index.txt` cache map across restarts.
- Cache size limit in MB (`sourcesep.cache_limit_mb`, default 1024).
- During cache evaluation, sourcesep first removes cache files not related to current playback context (not in current playlist and not currently playing/loaded).
- During precache overflow, oldest cache is evicted unless it belongs to the current playing entry or any entry after it in the current playlist; in that case precaching pauses.
- Cache directory is removed when DeaDBeeF exits (plugin stop).
- Optional verbose logging to DeaDBeeF internal log (`sourcesep.trace=1`).

## Build

From `sourcesep-plugin/`:

```bash
make
```

## Install

```bash
make install
```

This installs `sourcesep.so` to:

`~/.local/lib/deadbeef/`

Restart DeaDBeeF after install/update.

## Runtime Requirements

- Python 3 (required for the default `inference.py` script)

### Model Setup (required)

#### Vocal Separation Model (Mel-Band Roformer)

1. Clone:

```bash
git clone https://github.com/nonoo/Mel-Band-Roformer-Vocal-Model
```

2. Download checkpoint:

`https://huggingface.co/KimberleyJSN/melbandroformer/blob/main/MelBandRoformer.ckpt`

3. Place `MelBandRoformer.ckpt` into the cloned `Mel-Band-Roformer-Vocal-Model` directory.

#### Instrumental Separation Model (SCNet)

1. Clone:

```bash
git clone https://github.com/nonoo/Music-Source-Separation-Training
```

2. Download checkpoint:

`https://github.com/ZFTurbo/Music-Source-Separation-Training/releases/download/v1.0.15/model_scnet_ep_36_sdr_10.0891.ckpt`

3. Place it in `Music-Source-Separation-Training/results/model_scnet_ep_36_sdr_10.0891.ckpt`.

#### Plugin configuration

In DeaDBeeF plugin settings, set **Vocal Separation Script** and **Instrumental Separation Script** to the path of `run.sh` (or your specific separation scripts) in that cloned directory.

## Plugin Configuration

In DeaDBeeF plugin settings:

- **Vocal Separation Script** (`sourcesep.vocal_separation_script`): called with `--input <path> --stream-f32le-vocal` arguments.
- **Instrumental Separation Script** (`sourcesep.instrumental_separation_script`): called with `--input <path> --stream-f32le-instrumental` arguments.
- **Enable verbose logging** (`sourcesep.trace`, default off)
- **Cache size limit (MB)** (`sourcesep.cache_limit_mb`, default `1024`)


## Modes and Behavior

- Default mode on startup is `🔊` (original).
- When mode is `🎸` or `🎤`, playback uses sourcesep decode path.
- New tracks are auto-routed through sourcesep when non-original mode is active.
- Seeking is disabled while inference is in progress (except seek to 0).
- Full seeking is enabled after inference is finished, and immediately for cache hits.

## Cache Format

Cache files are MP3 (`.mp3`) generated with model-side `--mp3`.

During active streaming, sourcesep writes a temporary raw float32 cache file (`*.f32tmp`) for instant playback; after inference completes successfully it finalizes to MP3 and updates `index.txt`.

`index.txt` schema:

```text
# mode	mtime	size	samplerate	channels	complete	cache_file	uri
```

Field notes:
- `mode`: `instr` for instrumental, `vocal` for vocal
- `mtime`,`size`: source file stat used for cache key validity
- `samplerate`,`channels`: playback format
- `complete`: `1` means reusable complete cache, `0` incomplete
- `cache_file`: absolute path to cache `.mp3`
- `uri`: source track URI/path

Only `complete=1` entries are used as cache hits.

On startup, orphan `.mp3` files not referenced by `index.txt` are removed.

If playback stops before inference completes, incomplete cache is removed.

## Troubleshooting

- Enable verbose logging (`sourcesep.trace=1`) and inspect DeaDBeeF log.
- Verify `sourcesep.vocal_separation_script` points to the correct script path.
- Verify the script is executable and can run (it's called directly).
- If needed, clear cache directory:
  - `/tmp/deadbeef-$UID-sourcesep-cache`
