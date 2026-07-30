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
| `linkaudio/peer-name` | string | R/W | Link peer display name. Publishes the *effective* name (override or hostname); write to override; delete → reverts to hostname. |
| `linkaudio/latency` | decimal (ms) | R/W | Receiver playout buffer in ms (converted to beats at the current tempo). Default 100, range 0–2000, non-finite reverts to default. Delete → 100. |
| `linkaudio/sync-to-incoming` | boolean | R/W | Sync to Incoming Audio: delay the local transport timeline by the buffer so transport-locked generators align with incoming audio. **Default off.** The receive buffer is always applied regardless. Delete → reverts to default (off). |

### Link Audio — sinks and sources (the lists)

Both keys are **declarative**: write the list you want and the service reconciles it (add,
remove, rename, reorder in one idempotent pass), then publishes the canonical applied value —
key-tagged, in display order — back to the same key. Diff your desired list against that
read-back. Deleting either key reverts it to the empty list (all slots removed).

| Key | Type | Access | Value |
|-----|------|--------|-------|
| `linkaudio/sinks` | JSON | R/W | `[{"key":"a3f2c19b4e0d","name":"Drums"}, …]` in display order. On write, `key` is optional: an entry with only a `name` is self-keying (the key is derived from the name), and an entry with an existing slot's `key` plus a different `name` is a **rename**. |
| `linkaudio/sources` | JSON | R/W | `[{"key":"7b10c9de2245","peer":"Alex's Move","channel":"Cue"}, …]` in display order. On write, an entry with a `channel` is matched by exact `peer`+`channel`; an entry with only a `key` refers to an existing slot (this is what an order-only write looks like). |

Rejected entries (empty or duplicate sink name, duplicate source, slot-key collision,
unparseable JSON) are skipped, the rest of the list is applied, and the canonical value is
republished so a client's read-back self-corrects. A rejected *rename* keeps its slot under the
previous name — removal is only ever expressed by omitting an entry.

### Link Audio — commands (write-only)

| Key | Type | Access | Description |
|-----|------|--------|-------------|
| `linkaudio/reset-dropouts` | string | W | Zero a source's cumulative dropout count so it reads as "dropouts since now" — useful for A/B'ing a latency or buffer change. Value is a slot `key`, or `*` for every source. The service **removes the property** once it has acted, which is also what lets the same value be sent twice in a row. |

### Link Audio — status (read-only)

| Key | Type | Access | Description |
|-----|------|--------|-------------|
| `linkaudio/channels` | JSON | R | Available channels grouped by peer: `[{"peer":…,"channels":[…]}, …]`. |
| `linkaudio/source-status` | JSON | R | Per-source live receive telemetry, key-tagged and in display order: `[{"key":…,"connected":…,"receiving":…,"buffered_ms":…,"dropouts":…,"unmappable":…,"jitter_ms":…}, …]`. Updated a few times per second, and immediately on a connect/disconnect. The configured identity is in `linkaudio/sources`; because it is matched exactly, the resolved channel is either identical to it or absent, which is what `connected` reports. `receiving` is true while blocks are actually being filled with audio — see [Reading the telemetry](#reading-the-telemetry). |

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

The control surface is two **explicit, ordered lists**: sinks (outgoing) and sources
(incoming). A device starts with **zero of each**, and nothing ever connects on its own —
you add a sink by naming it, and a source by picking a specific advertised peer/channel.

* A **sink** announces a named stereo channel to the Link session and is fed by the JACK
  input port pair `in_<key>_l` / `in_<key>_r`.
* A **source** subscribes to one exact advertised `(peer, channel)` and writes it to the JACK
  output port pair `out_<key>_l` / `out_<key>_r`.

The receiver buffers incoming audio by `linkaudio/latency` ms (always applied — network
buffers arrive late). "Sync to Incoming Audio" (`linkaudio/sync-to-incoming`, default off)
additionally delays the *local* transport timeline by that buffer so transport-locked
generators stay phase-aligned with the incoming stream; when off, the local transport runs
live and incoming audio simply lags local generators by the buffer.

### Slot keys and port names

Every slot has a **key**: 12 lowercase hex digits, the low 48 bits of an FNV-1a-64 hash of the
slot's identity — a sink's announced name, or a source's exact `peer` + `channel`. It is
*derived*, never allocated and never persisted; the same identity always yields the same key,
so the JACK port names come back identical across a restart.

That matters because a patchbay (and a saved RNBO set) records connections **by port name**.
With sequential slot numbers, deleting a slot and adding an unrelated one would hand the new
slot the old one's ports — and therefore its cables. Deriving the name from the identity makes
that impossible: a different identity is always a different port.

Two consequences worth knowing:

* **Re-adding the same identity picks its old cables back up.** Delete the sink "Drums", add
  "Drums" again, and a saved set's connections to `in_<hash("Drums")>_*` re-apply. That is the
  intended reading of identity-derived naming, and the only case where anything reconnects on
  its own. Delete "Drums" / add "Bass" gets you a clean, unconnected slot.
* **Renaming a sink re-registers its ports** (the key is a function of the name). The service
  migrates *live* connections across the rename, so the running graph is preserved; connections
  already written into a saved set won't match the new name until the set is re-saved. Safe-fail
  — a lost connection, never a wrong one. A Link peer renaming *itself* is the same story from
  the source side: the old identity is simply gone, and that source goes disconnected.

Hashed port names are not a UI problem: each port carries JACK `pretty-name` metadata (a sink's
name, a source's `"<peer>: <channel>"`, suffixed " L"/" R") plus an `order` following the
display order, so patchbays show and sort them sensibly.

### Adding and removing sinks

```shell
# Two sinks, in display order
jack_property --client jack-transport-link \
  http://www.x37v.info/jack/metadata/linkaudio/sinks \
  '[{"name":"Drums"},{"name":"Bass"}]' application/json

# Read back the canonical value (now key-tagged)
jack_property --client jack-transport-link --list \
  http://www.x37v.info/jack/metadata/linkaudio/sinks

# ... and the ports it registered, with their pretty names
jack_lsp -A jack-transport-link

# Rename "Drums" to "Kit": same key, new name (live connections follow the new ports)
jack_property --client jack-transport-link \
  http://www.x37v.info/jack/metadata/linkaudio/sinks \
  '[{"key":"b42f444b799a","name":"Kit"},{"name":"Bass"}]' application/json

# Reorder (order metadata only — no ports re-registered, no connections moved)
jack_property --client jack-transport-link \
  http://www.x37v.info/jack/metadata/linkaudio/sinks \
  '[{"name":"Bass"},{"name":"Kit"}]' application/json

# Remove everything
jack_property --client jack-transport-link \
  http://www.x37v.info/jack/metadata/linkaudio/sinks '[]' application/json
```

### Adding and removing sources

A source names an exact peer and channel — matching is exact and case-sensitive, and there is
no auto-selection. List what's advertised with `linkaudio/channels`, then add what you want:

```shell
# List available channels
jack_property --client jack-transport-link --list \
  http://www.x37v.info/jack/metadata/linkaudio/channels

# Subscribe to two of them
jack_property --client jack-transport-link \
  http://www.x37v.info/jack/metadata/linkaudio/sources \
  '[{"peer":"Push","channel":"Master"},{"peer":"Alex","channel":"Cue"}]' application/json

# Live receive telemetry, joined to the list above by key
jack_property --client jack-transport-link --list \
  http://www.x37v.info/jack/metadata/linkaudio/source-status
```

If a source's peer goes offline the entry **stays in the list**, marked disconnected, keeps its
ports, and reconnects by itself when a channel with the same peer and channel name reappears.
A peer appearing that no source names connects to nothing.

### Reading the telemetry

`source-status` reports two separate booleans, and the difference matters:

| | meaning |
|---|---|
| `connected: false` | nothing on the network is advertising this exact peer + channel |
| `connected: true, receiving: false`, `unmappable: 0` | subscribed, but nothing usable is arriving |
| `connected: true, receiving: false`, `unmappable` rising | audio **is** arriving, but stamped for a different Link session, so it can't be beat-aligned and is discarded |
| `connected: true, receiving: true` | audio is flowing |

`unmappable` counts buffers whose `beginBeats`/`endBeats` map to nothing, which Link reports when
the buffer came from a *different Link session*. No `latency` value can fix that — the two devices
have to be in the same session (Link enabled on both, same network). Beware when reading this code:
`endBeats()` returns `std::optional<double>`, and comparing it directly against a `double` is a
trap, because `nullopt < v` is defined as **true** for every `v` — so a foreign-session buffer
looks "too old" and is silently discarded at any buffer size.

A `receiving: false` with `unmappable: 0` is almost always a playout buffer too small for the
network. A sender stamps each
buffer with the beat it captured and then transmits it, so a buffer inevitably *arrives* after the
beat it carries. `linkaudio/latency` is what lets the receiver aim its playout cursor far enough
behind the live beat for the audio to have shown up. Set it to `0` and the cursor sits on the live
beat, every queued buffer is already too old, and the source outputs silence forever.

**`dropouts` cannot tell you this.** A dropout is only counted once playback has established a
read position — silence before that is indistinguishable from normal pre-roll. A source that never
starts therefore reports `0` dropouts while producing nothing, which is exactly why `receiving`
exists. (Related: because each starve resets the read position, a sustained outage counts as one
dropout rather than one per block.)

There's no universal minimum for `latency`; it has to cover the sender's buffer size plus network
jitter. Raise it until `receiving` goes true and `dropouts` stops advancing.

### Resetting dropout counts

`dropouts` is cumulative for the life of the connection, which makes it hard to read after you've
been changing settings. Zero it to measure from now:

```shell
# every source
jack_property --client jack-transport-link \
  http://www.x37v.info/jack/metadata/linkaudio/reset-dropouts '*' text/plain

# one source, by slot key
oscsend localhost 4001 /jacklink/audio/source/reset-dropouts s 7b10c9de2245
# ... or by identity
oscsend localhost 4001 /jacklink/audio/source/reset-dropouts ss "Alex's Move" Cue
```

Resetting does not disturb the stream, and `source-status` is republished immediately rather than
at the next telemetry tick, so the zero shows up at once.

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
| `/jacklink/audio/sink/add` | `string name` | Append a sink. Rejected if the name is empty or already used. |
| `/jacklink/audio/sink/remove` | `string name\|key` | Remove that sink (tried as a name first, then as a key). |
| `/jacklink/audio/sink/rename` | `string old, string new` | Rename a sink (identified by name or key). Rejected on a name collision. |
| `/jacklink/audio/sinks/order` | `string …` | Set the display order by name-or-key. Omitted slots keep their relative order at the end. |
| `/jacklink/audio/source/add` | `string peer, string channel` | Append a source. Rejected if that pair is already present. |
| `/jacklink/audio/source/remove` | `string peer, string channel` *or* `string key` | Remove that source. |
| `/jacklink/audio/source/reset-dropouts` | *(none)*, `string key`, *or* `string peer, string channel` | Zero the dropout count — of every source with no arguments, otherwise of the one named. |
| `/jacklink/audio/sources/order` | `string …` | Set the display order by key. Omitted slots keep their relative order at the end. |

These are imperative sugar over the two declarative list properties — whole-array JSON is
painful to send over OSC. Each command mutates the desired list and lets the same reconcile pass
apply it, so the rejection rules are identical.

Note that the identifier is an *argument*, not part of the address: a sink name can contain a
`/`, which an OSC address can't encode.

```
# /jacklink/audio/sink/add       "Drums"
# /jacklink/audio/sink/rename    "Drums" "Kit"
# /jacklink/audio/sinks/order    "Bass" "Kit"
# /jacklink/audio/source/add     "Alex's Move" "Cue"
# /jacklink/audio/source/remove  "Alex's Move" "Cue"
```

## Config File

Settings are persisted to `~/.config/jack-transport-link/config.json` (respects
`$XDG_CONFIG_HOME`). Override with `-c <path>`. CLI flags take precedence over config values.

Saved fields: `bpm`, `quantum`, `time_sig_denom`, `ticks_per_beat`, `start_stop_sync`,
`sync`, `link_enabled`, `link_audio_enabled`, `sinks`, `sources`, `link_peer_name`,
`link_audio_latency_ms`, `link_audio_sync_to_incoming`,
`link_audio_capture_latency_trim_ms`, `link_audio_playback_latency_trim_ms`.

`sinks` and `sources` hold the two lists in display order, identities only:

```json
{
  "sinks":   [{ "name": "Drums" }, { "name": "Bass" }],
  "sources": [{ "peer": "Alex's Move", "channel": "Cue" }]
}
```

**Slot keys are not persisted** — they're re-derived from these identities on load, which is
what makes the JACK port names stable across a restart.

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
