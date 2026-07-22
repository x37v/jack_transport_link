# Jack Transport Link

A service that bridges [Ableton's Link](https://github.com/Ableton/link) to and from
[Jack Transport](https://jackaudio.org/api/transport-design.html), allowing applications
that use *Jack Transport* to synchronize their timing with other applications that support
*Link*. It also implements **Link Audio**: real-time, beat-grid-aligned audio streaming
between peers in a Link session.

## Build Requirements

* a compiler that can build c++17
	* clang++, g++
* [cmake](https://cmake.org/) 3.16 or higher

## Building

Make sure you've updated your submodules:

```shell
git submodule update --init --recursive
```

Use cmake to configure, then build:

```shell
mkdir build && cd build && cmake .. && make
```

If everything succeeds, you should have an executable here: `./bin/jack_transport_link`.

### Linux Systemd Service

There is an optional systemd service file that is enabled by default, at this
time it is run as the user `pi` for the raspi. You can disable that through the
cmake `-DINSTALL_SERVICE_FILE=Off`

If you want to target a different user, you'll have to edit the appropriate file in `config/`

## Installing

You can just run from the bin directory if you want, or copy the executable somewhere,
but you can also use the install target:

```shell
sudo make install
```

Or, on debian systems you can use `cpack` and then install the deb, from the build dir.

```shell
cpack && sudo dpkg -i *.deb
```

## Running

There are a few options for running the service, setting the initial tempo
and time signature details, indicating if you want the service to start a
jack server if there isn't already one to connect to, etc.

Run with the `-h` switch to discover the full option list. Options relevant to the
control surfaces below:

| Flag | Description |
|------|-------------|
| `-n, --jack-client-name <name>` | JACK client name (metadata subject). Default `jack-transport-link`. |
| `-N, --link-name <name>` | Link peer name to broadcast; empty uses the hostname. |
| `-o, --osc-port <port>` | Enable the OSC listener on this UDP port. |
| `-c, --config <path>` | Config file path (see [Config File](#config-file)). |
| `-s / -S` | Enable / disable transport start-stop sync with Link peers. |
| `--link / --no-link` | Join / don't join the Link session (default: join). |
| `-A, --no-link-audio` | Disable Link Audio (enabled by default). |
| `--link-audio-in-stereo-channels <n>` | Number of stereo send pairs (default 1). |
| `--link-audio-out-stereo-channels <n>` | Number of stereo receive pairs (default 1). |
| `--latency-ms <ms>` | Link Audio receiver playout buffer, ms (default 100, range 0–2000). |
| `--sync-to-incoming / --no-sync-to-incoming` | Sync to Incoming Audio (default: off). |
| `--capture-latency-trim-ms <ms>` | Trim added to auto-detected capture latency (send). |
| `--playback-latency-trim-ms <ms>` | Trim added to auto-detected playback latency (receive). |

CLI flags take precedence over saved config values.

## Control Interfaces

The service is controlled and observed two ways:

1. **JACK metadata** — the primary interface. All keys hang off the base
   `http://www.x37v.info/jack/metadata/` and target the JACK client (default subject
   `jack-transport-link`). Writable keys are polled via the property-change callback;
   read-only keys are published by the service and updated automatically. See the
   [Metadata Reference](#metadata-reference).
2. **OSC** — an optional subset, enabled with `-o <port>`. See [OSC Control](#osc-control).

Since JACK transport doesn't let clients request tempo, tempo (and everything else)
goes through the metadata API. You must use jack 1.9.13 or newer for metadata support.

Example — set / read the tempo:

```shell
# set 150 bpm
jack_property --client jack-transport-link \
  http://www.x37v.info/jack/metadata/bpm 150.0 \
  https://www.w3.org/2001/XMLSchema#decimal

# read it back
jack_property --client jack-transport-link --list \
  http://www.x37v.info/jack/metadata/bpm
```

## Metadata Reference

All keys are relative to the base `http://www.x37v.info/jack/metadata/` and set on the
JACK client subject. **Access** is `R/W` (client may write; service also publishes the
applied value) or `R` (read-only, published by the service). Deleting a writable property
reverts it to its default where noted.

**Type URIs:** `decimal` = `https://www.w3.org/2001/XMLSchema#decimal`, `integer` =
`https://www.w3.org/2001/XMLSchema#integer`, `boolean` = `https://www.w3.org/2001/XMLSchema#boolean`,
`string` = `text/plain`, `JSON` = `application/json`.

### Transport & Link session

| Key | Type | Access | Description |
|-----|------|--------|-------------|
| `bpm` | decimal | R/W | Transport tempo in beats per minute. Settable even while stopped. |
| `linksync` | boolean | R/W | JACK transport follows the Link tempo/grid. Turning on adopts the current Link tempo. |
| `link/start-stop-sync` | boolean | R/W | Synchronize transport start/stop with start-stop-enabled Link peers. |
| `link/enabled` | boolean | R/W | Master Link on/off. When off, peers don't see this device (tempo sync + Link Audio inactive); JACK transport keeps running locally. Delete → reverts to enabled. |
| `linkpeers` | integer | R | Number of currently connected Link peers. |

### Link Audio — configuration

| Key | Type | Access | Description |
|-----|------|--------|-------------|
| `linkaudio/in-stereo-channels` | integer | R/W | Number of stereo **send** pairs (registers `in_1/in_2`, …). Range 0–max. |
| `linkaudio/out-stereo-channels` | integer | R/W | Number of stereo **receive** pairs (registers `out_1/out_2`, …). |
| `linkaudio/peer-name` | string | R/W | Link peer display name. Publishes the *effective* name (override or hostname); write to override; delete → reverts to hostname. |
| `linkaudio/latency` | decimal (ms) | R/W | Receiver playout buffer in ms (converted to beats at the current tempo). Default 100, range 0–2000, non-finite reverts to default. Delete → 100. |
| `linkaudio/sync-to-incoming` | boolean | R/W | Sync to Incoming Audio: delay the local transport timeline by the buffer so transport-locked generators align with incoming audio. **Default off.** The receive buffer is always applied regardless. Delete → reverts to default (off). |

### Link Audio — source selection (write)

| Key | Type | Access | Description |
|-----|------|--------|-------------|
| `linkaudio/source` | JSON | R/W | Set source filters. A single `{"peer":…,"channel":…}` object sets receiver 0; a JSON array sets each receiver by index. `{}` reverts a receiver to auto-selection. Both fields are case-insensitive substring matches and either may be omitted. |
| `linkaudio/source/<N>` | JSON object | R/W | Set the filter for receiver `N` only. |
| `linkaudio/source/<N>/name` | string | R/W | Display name for receive slot `N` ("Recv N" by default). |
| `linkaudio/sink/<N>/name` | string | R/W | Display name for send slot `N` ("Send N" by default); announced to the Link session. |

### Link Audio — status (read-only)

| Key | Type | Access | Description |
|-----|------|--------|-------------|
| `linkaudio/channels` | JSON | R | Available channels grouped by peer: `[{"peer":…,"channels":[…]}, …]`. |
| `linkaudio/source-status` | JSON | R | **Current connections**, one entry per receiver: `{"peer":…,"channel":…}` when connected, `{}` when not. |
| `linkaudio/source-filters` | JSON | R | The **configured** filters per receiver (what was requested), even when no matching peer is online. |
| `linkaudio/source-health` | JSON | R | Per-receiver receive health: `{"buffered_ms":…,"dropouts":…,"jitter_ms":…,"connected":…}`. Updated a few times per second. |

### Link Audio — latency (read-only)

The service auto-detects device I/O latency via JACK's latency callback and compensates
send/receive timing. These are observation-only; adjust the trim via the
`--capture-latency-trim-ms` / `--playback-latency-trim-ms` CLI flags (or config), **not**
via metadata.

| Key | Type | Access | Description |
|-----|------|--------|-------------|
| `linkaudio/capture-latency` | decimal (ms) | R | Effective send latency (auto-detected + trim). |
| `linkaudio/playback-latency` | decimal (ms) | R | Effective receive latency (auto-detected + trim). |
| `linkaudio/capture-latency-auto` | decimal (ms) | R | JACK auto-detected capture latency. |
| `linkaudio/playback-latency-auto` | decimal (ms) | R | JACK auto-detected playback latency. |

## Link Audio

Link Audio streams real-time audio between peers in a Link session with beat-grid
alignment. It is enabled by default (disable with `--no-link-audio`).

Each stereo send pair (configurable with `--link-audio-in-stereo-channels`, default `1`)
registers JACK input ports `in_1`/`in_2`, `in_3`/`in_4`, etc. and announces a named stereo
channel ("Send 1", "Send 2", …) to the Link session.

Each stereo receive pair (configurable with `--link-audio-out-stereo-channels`, default
`1`) registers JACK output ports `out_1`/`out_2`, `out_3`/`out_4`, etc. and subscribes to
one incoming channel ("Recv 1", "Recv 2", …). Send and receive counts are independent.

The receiver buffers incoming audio by `linkaudio/latency` ms (always applied — network
buffers arrive late). "Sync to Incoming Audio" (`linkaudio/sync-to-incoming`, default off)
additionally delays the *local* transport timeline by that buffer so transport-locked
generators stay phase-aligned with the incoming stream; when off, the local transport runs
live and incoming audio simply lags local generators by the buffer.

### Selecting a source

By default the first available peer is subscribed automatically. To select a specific peer,
write the `linkaudio/source` JACK metadata property (or send the matching OSC message).
Read the resulting connection from `linkaudio/source-status`, and the available channels
from `linkaudio/channels`.

```shell
# Set receiver 0 only
jack_property --client jack-transport-link \
  http://www.x37v.info/jack/metadata/linkaudio/source/0 \
  '{"peer":"Push"}' application/json

# Set receiver 1 only
jack_property --client jack-transport-link \
  http://www.x37v.info/jack/metadata/linkaudio/source/1 \
  '{"peer":"Alex","channel":"Cue"}' application/json

# Set all receivers at once (array form)
jack_property --client jack-transport-link \
  http://www.x37v.info/jack/metadata/linkaudio/source \
  '[{"peer":"Push"},{"peer":"Alex"}]' application/json

# Revert receiver 0 to auto (others unchanged)
jack_property --client jack-transport-link \
  http://www.x37v.info/jack/metadata/linkaudio/source/0 \
  '{}' application/json

# Read the current connections (array, one entry per receiver)
jack_property --client jack-transport-link --list \
  http://www.x37v.info/jack/metadata/linkaudio/source-status

# List available channels
jack_property --client jack-transport-link --list \
  http://www.x37v.info/jack/metadata/linkaudio/channels
```

Change the number of stereo pairs at runtime with the integer `in-stereo-channels` /
`out-stereo-channels` properties:

```shell
jack_property --client jack-transport-link \
  http://www.x37v.info/jack/metadata/linkaudio/in-stereo-channels 2 \
  https://www.w3.org/2001/XMLSchema#integer
```

## OSC Control

Pass `-o <port>` to enable the OSC listener. It covers a subset of the metadata controls;
anything not listed here is metadata-only.

| Address | Argument | Description |
|---------|----------|-------------|
| `/jacklink/bpm` | `float` or `double` | Set the tempo in beats per minute. |
| `/jacklink/beattime` | `float` or `double` | Seek the transport to the given beat position. |
| `/jacklink/sync` | `bool` | Enable (`true`) / disable (`false`) Link sync (`linksync`). |
| `/jacklink/rolling` | `bool` | Start (`true`) / stop (`false`) the transport. |
| `/jacklink/start-stop-sync` | `bool` | Enable / disable transport start/stop sync with Link peers. |
| `/jacklink/enabled` | `bool` | Master Link on/off (join / leave the session). |
| `/jacklink/audio/peer-name` | `string` | Set the Link peer name (empty reverts to hostname). |
| `/jacklink/audio/latency` | `float` or `double` | Receiver playout buffer in ms (clamped to 0–2000). |
| `/jacklink/audio/sync-to-incoming` | `bool` | Sync to Incoming Audio toggle. |
| `/jacklink/audio/source` | `string string [string string …]` | Set source filters as flat `peer channel` pairs: `peer0 channel0 peer1 channel1 …`. |
| `/jacklink/audio/source/<N>` | `string string` | Set the source filter for receiver N as a `peer channel` pair. |
| `/jacklink/audio/source/<N>/name` | `string` | Set the display name for receive slot N. |
| `/jacklink/audio/sink/<N>/name` | `string` | Set the display name for send slot N. |
| `/jacklink/audio/in-stereo-channels` | `int` | Change the number of stereo send pairs at runtime. |
| `/jacklink/audio/out-stereo-channels` | `int` | Change the number of stereo receive pairs at runtime. |

OSC source pairs use flat strings rather than JSON — empty strings match any peer/channel
(auto-selection):

```
# Set receiver 0:    /jacklink/audio/source/0  "Push" ""
# Set receiver 1:    /jacklink/audio/source/1  "Alex" "Cue"
# Set all at once:   /jacklink/audio/source    "Push" "" "Alex" "Cue"
# Revert 0 to auto:  /jacklink/audio/source/0  "" ""
```

## Config File

Settings are persisted to `~/.config/jack-transport-link/config.json` (respects
`$XDG_CONFIG_HOME`). Override with `-c <path>`. CLI flags take precedence over config values.

Saved fields: `bpm`, `quantum`, `time_sig_denom`, `ticks_per_beat`, `start_stop_sync`,
`sync`, `link_enabled`, `link_audio_enabled`, `in_stereo_channels`, `out_stereo_channels`,
`source_filters`, `source_names`, `sink_names`, `link_peer_name`, `link_audio_latency_ms`,
`link_audio_sync_to_incoming`, `link_audio_capture_latency_trim_ms`,
`link_audio_playback_latency_trim_ms`.

## TODO

* Follower mode (just report transport, don't drive it)
* Option to synchronize the rolling start to a start of a bar.
* Windows support

## Acknowledgements

Built using:

* [Ableton's Link](https://github.com/Ableton/link)
* [Jack Audio Connection Toolkit](https://jackaudio.org/)
* [cpp-optparse](https://github.com/weisslj/cpp-optparse)

I learned a lot from [jack_link](https://github.com/rncbc/jack_link) by Rui
Nuno Capela, which is trying to solve the same problem but has a user interface
and I wanted to simply run as a service. *jack_link* also has some issues with
discontinuities that I wasn't happy with.
