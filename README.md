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
| `-o, --osc-port <port>` | UDP port for the OSC interface. Default `3234`, searched upwards over 16 ports; an explicit port is used as given. See [Binding the OSC port](#binding-the-osc-port). |
| `--no-osc` | Disable OSC entirely (enabled by default). **Also disables the Link Audio bridge**, which is OSC-only. |
| `--osc-bind-any` | Bind the OSC socket on every interface instead of loopback only. Off by default — see [Binding the OSC port](#binding-the-osc-port). |
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

The service is controlled and observed two ways, split by *how often the value changes*:

1. **JACK metadata** — the transport and Link session settings: tempo, sync toggles, peer
   count. All keys hang off the base `http://www.x37v.info/jack/metadata/` and target the JACK
   client (default subject `jack-transport-link`). Writable keys are picked up via the
   property-change callback; read-only keys are published by the service. See the
   [Metadata Reference](#metadata-reference). You must use jack 1.9.13 or newer for metadata
   support — JACK transport doesn't let clients request a tempo, which is why tempo lives here.
2. **OSC** — everything about **Link Audio**, in both directions, plus imperative equivalents of
   the transport settings. On by default. See [OSC Control](#osc-control).

Link Audio is deliberately *not* on metadata. JACK metadata is a disk-backed Berkeley DB and
every write notifies every connected client whether it cares or not, which is fine for "set the
tempo occasionally" and wrong for a few-times-a-second stream of receive telemetry. Link Audio
state is pushed over UDP to clients that have asked for it, and nobody else pays for it.

The one metadata key Link Audio does need is discovery: `osc-port` tells a client which UDP port
to talk to. Everything from there is OSC.

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

This is the complete list of *client* keys: everything else moved to [OSC](#osc-control). The
service also sets [per-port metadata](#per-port-metadata) on its Link Audio ports, whose subject
is the port rather than the client.

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
| `osc-port` | integer | R | The UDP port the OSC interface actually bound. Absent when started with `--no-osc`. This is how a client finds the [OSC interface](#osc-control) — and because JACK removes a client's properties when it disconnects, this key disappearing and reappearing is also how a client learns the service restarted and its listener registration needs renewing. Deleting it by hand doesn't turn anything off — the service re-publishes it, as it does every other key it owns. |

### Per-port metadata

The service also decorates its own Link Audio ports, subject = the *port* UUID rather than the
client. Three of the four keys are JACK's standard presentation keys, which live here rather than
on OSC precisely because a generic patchbay finds them by looking up the port it is already
drawing; the fourth is the slot key, which is what a *client* joins a port back to its OSC slot
with.

| Key | Value on a sink port | Value on a source port |
|-----|----------------------|------------------------|
| `port-group` (`http://jackaudio.org/metadata/port-group`) | `Link: Sends` — every sink in one group | `Link: <peer>` — one group per source peer |
| `pretty-name` (`http://jackaudio.org/metadata/pretty-name`) | `<name> L` / `<name> R` | `<channel> L` / `<channel> R` (the peer is in the group name) |
| `order` (`http://jackaudio.org/metadata/order`) | `1`, `2`, `3`… following the display order | same, numbered independently of the sinks |
| `link/audio/slot` (`http://www.x37v.info/jack/metadata/link/audio/slot`) | the sink's 12-hex-digit [slot key](#slot-keys-and-port-names) | the source's slot key |

The slot key is the machine-readable join: `port-group` and `pretty-name` are presentation
strings that change when a sink is renamed or a peer renames itself, whereas this key is exactly
the `key` field of the `sinks` / `sources` / `source-status` state payloads. Match on it rather
than parsing a group or pretty name.

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

All of this is driven over [OSC](#osc-control) — commands in, state pushed back out.

The receiver buffers incoming audio by `latency` ms (always applied — network buffers arrive
late). "Sync to Incoming Audio" (default off) additionally delays the *local* transport timeline
by that buffer so transport-locked generators stay phase-aligned with the incoming stream; when
off, the local transport runs live and incoming audio simply lags local generators by the buffer.

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
name, or a source's channel name, suffixed `" L"` / `" R"`), a `port-group` (`Link: Sends` for
sinks, `Link: <peer>` for sources) and an `order` following the display order, so patchbays show,
group and sort them sensibly. Each port also carries its slot key — see
[per-port metadata](#per-port-metadata).

### Adding and removing sinks

```shell
# Two sinks, in display order
oscsend localhost 3234 /jacklink/audio/sink/add s Drums
oscsend localhost 3234 /jacklink/audio/sink/add s Bass

# The ports it registered, with their pretty names
jack_lsp -A jack-transport-link

# Rename "Drums" to "Kit" (live connections follow the new ports)
oscsend localhost 3234 /jacklink/audio/sink/rename ss Drums Kit

# Reorder (order metadata only — no ports re-registered, no connections moved)
oscsend localhost 3234 /jacklink/audio/sinks/order ss Bass Kit

# Remove one
oscsend localhost 3234 /jacklink/audio/sink/remove s Kit
```

The applied list comes back on `/jacklink/state/audio/sinks`, key-tagged and in display order —
register a listener (see [OSC Control](#osc-control)) and diff against that.

### Adding and removing sources

A source names an exact peer and channel — matching is exact and case-sensitive, and there is
no auto-selection. `/jacklink/state/audio/channels` lists what's advertised; add what you want:

```shell
oscsend localhost 3234 /jacklink/audio/source/add ss Push Master
oscsend localhost 3234 /jacklink/audio/source/add ss Alex Cue
```

Live receive telemetry arrives on `/jacklink/state/audio/source-status`, joined to
`/jacklink/state/audio/sources` by key.

If a source's peer goes offline the entry **stays in the list**, marked disconnected, keeps its
ports, and reconnects by itself when a channel with the same peer and channel name reappears.
A peer appearing that no source names connects to nothing.

### Replacing the whole list

`sinks/set` and `sources/set` state the desired list outright instead of naming one change:
whatever isn't listed is removed, and the argument order is the display order.

```shell
# Exactly these two sinks, in this order — anything else we hold goes away
oscsend localhost 3234 /jacklink/audio/sinks/set ss Drums Bass

# Exactly this one source
oscsend localhost 3234 /jacklink/audio/sources/set ss Push Master

# No arguments = remove all
oscsend localhost 3234 /jacklink/audio/sinks/set
```

This exists for restoring a saved arrangement. Doing it with `add`/`remove` costs one reconcile
pass per command, and each structural pass cycles `jack_deactivate`/`jack_activate`; one `set`
applies the whole arrangement in a single pass, so ports appear once and connections settle once.

Two consequences of it being a *list of identities* rather than a list of edits:

- A rename isn't expressible. A sink's key — and therefore its JACK port names — derives from its
  name, so replacing `Drums` with `Kit` reads as a remove plus an add, and connections to the old
  ports don't follow. Use `sink/rename` when you mean rename.
- The message is all-or-nothing. A malformed list (a non-string, an unpaired peer, an empty name
  or channel) is rejected whole and logged, because applying the readable part of it would delete
  the slots that came after the bad entry. This is stricter than the imperative commands, where a
  rejected entry is skipped and the rest applied.

The applied lists come back on `/jacklink/state/audio/{sinks,sources}` as usual — a client that
just wrote a `set` can diff against those to see what landed.

### Reading the telemetry

`/jacklink/state/audio/source-status` reports two separate booleans, and the difference matters:

| | meaning |
|---|---|
| `connected: false` | nothing on the network is advertising this exact peer + channel |
| `connected: true, receiving: false` | subscribed, but rendering **pure silence** |
| `connected: true, receiving: true` | audio is flowing |

The middle row is almost always a playout buffer too small to cover the arrival offset — see
[How much `latency` a source needs](#how-much-latency-a-source-needs). The other possibility is
that the sender is in a *different Link session*: `beginBeats`/`endBeats` then map to nothing and
the buffer has to be discarded, which looks the same from the outside. Beware when reading that
code — `endBeats()` returns `std::optional<double>`, and comparing it directly against a `double`
is a trap, because `nullopt < v` is defined as **true** for every `v`, so a foreign-session buffer
looks "too old" and gets discarded at any buffer size. A sender stamps each
buffer with the beat it captured and only transmits it once it's full, so a buffer normally *arrives*
after the beat it carries (not always — see
[When `arrival_offset_ms` is negative](#when-arrival_offset_ms-is-negative)). `latency` is what lets
the receiver aim its playout cursor far enough behind the live beat for the audio to have shown up.
Set it to `0` and the cursor sits on the live beat, every queued buffer is already too old, and the
source outputs silence forever.

**`dropouts` cannot tell you this.** A dropout is only counted once playback has established a
read position — silence before that is indistinguishable from normal pre-roll. A source that never
starts therefore reports `0` dropouts while producing nothing, which is exactly why `receiving`
exists. (Related: because each starve resets the read position, a sustained outage counts as one
dropout rather than one per block.)

#### How much `latency` a source needs

`arrival_offset_ms` measures it directly: the gap between the beat this block will be *heard* at and
the beat the newest buffer we hold *begins* at. "Heard at" rather than "now" because the receive path
works from `mTimeNext + playback-latency` — which matters for the negative case below.

A sender stamps a buffer with the beat it captured and can only transmit it once it's full, so the
freshest audio you hold is normally in the past and the reading is **positive**. `latency` has to
exceed it for anything to play at all, and exceed it by the jitter margin for playback to stay clean.

The two readings move together: in steady state

    buffered_ms  ~=  latency  -  arrival_offset_ms

approximately — the offset is sampled at the top of a render pass and `buffered_ms` at the bottom, so
expect a few ms of disagreement, on the order of the jitter.

So a source that only plays at a large `latency` while reporting a small `buffered_ms` is not
evidence of a miscalculated buffer — it means the arrival offset really is that large, and
`arrival_offset_ms` is where to look for why. Compare it against:

  - the sender's own buffer/period size (a sender emitting long chunks cannot be received with a
    short playout buffer);
  - the pushed `playback-latency` and `playback-latency-auto`. The receive target is
    `beat(now + playback-latency) - latency`, so an over-reported playback latency pushes the
    playout cursor into the future and has to be paid for with extra `latency`. These are
    read-only; correct them with `--playback-latency-trim-ms` (a negative trim is allowed).

#### When `arrival_offset_ms` is negative

This is normal, and against a DAW it is the *common* case. A negative reading means the newest audio
you hold is stamped for a beat **later** than your playout moment: the sender is running ahead of you
on the shared beat timeline, so audio is arriving before it is needed rather than late.

What pushes it negative is the sender, not you: **a DAW renders ahead of its own output.** At
wall-clock now it has already produced the audio its driver will play out one output-latency from now,
and it stamps buffers with the beat that audio belongs to. Ableton Live is routinely some milliseconds
ahead for this reason alone.

Note which way your own `playback-latency` moves the reading — it is easy to get backwards. The offset
is measured against `beat(mTimeNext + playback-latency)`, so a larger playback latency puts the
reference point *later* and pushes the reading **positive**. A device with real output latency
therefore reports a *less* negative offset than one whose driver reports none, and a still-negative
reading on real hardware means the sender is ahead by more than your own output latency accounts for.

The consequence catches people out: **`buffered_ms` comes out larger than the `latency` you set**,
because `latency - arrival_offset_ms` exceeds `latency` once the offset goes negative. Nothing is
wrong and it is not a buffer overrun — it is cushion the sender handed you for free.

It also means you are probably buffering more than you need, since what `latency` has to cover is the
*positive* arrival delay plus a jitter margin. Two cautions before you cut it:

  - size it against the **worst** offset you observe, not the instantaneous one — it moves with
    network conditions and with the sender's own buffer settings;
  - step down while watching `dropouts` and `receiving`. The failure mode is the opposite sign: a
    large positive offset with `latency` below it gives `receiving: false` and silence.

Worth a glance that `buffered_ms` is *stable* rather than climbing. Steady near
`latency - arrival_offset_ms` is correct; growing without bound would mean the queue isn't draining.

### Resetting dropout counts

`dropouts` is cumulative for the life of the connection, which makes it hard to read after you've
been changing settings. Zero it to measure from now:

```shell
# every source
oscsend localhost 3234 /jacklink/audio/source/reset-dropouts

# one source, by slot key
oscsend localhost 3234 /jacklink/audio/source/reset-dropouts s 7b10c9de2245
# ... or by identity
oscsend localhost 3234 /jacklink/audio/source/reset-dropouts ss "Alex's Move" Cue
```

Resetting does not disturb the stream, and `source-status` is re-sent immediately rather than at
the next telemetry tick, so the zero shows up at once.

## OSC Control

OSC is on by default. It is the whole Link Audio interface, in both directions, and also offers
imperative equivalents of the transport/Link settings that live in metadata.

### Binding the OSC port

With no `-o`, the service binds the first free port in `3234`–`3249` and publishes the one it got
as the `osc-port` metadata key. With an explicit `-o <port>` it makes exactly one attempt.

**Loopback only, by default.** The socket binds `127.0.0.1`, so only clients on this machine can
reach it. The command surface is unauthenticated and partly destructive — an argument-less
`/jacklink/audio/sinks/set` legitimately means "remove every sink", the reconcile persists that
within a second, and `/jacklink/listeners/clear` silences every client — so one datagram from any
host that can reach the port is enough to wipe a device's Link Audio arrangement across restarts.
`--osc-bind-any` binds every interface (`0.0.0.0`) for the cases that need it; treat it as opting
the device's audio routing into whatever the network can send.

While bound to loopback, listener registrations for non-loopback addresses are rejected with a log
line rather than accepted: a `sendto()` to a routable address from a loopback-bound socket doesn't
arrive, so registering one would be a silent no-op.

Every failure is **fatal** — a jack_transport_link with no OSC socket has no Link Audio bridge at
all, and on a headless device a unit that failed to start is far easier to diagnose than one that
came up quietly half-working. That covers an explicit port already in use, the whole default range
being busy, and a port outside `1`–`65535`. In particular `-o 0` is an *error*, not "off": port 0
conventionally means "let the OS pick an ephemeral port", and that meaning is worth keeping free.

To actually turn OSC off, use `--no-osc`. Nothing binds, no `osc-port` key is published, and a
client that finds no key correctly reports Link Audio unavailable. The service still runs as JACK
transport master and Link peer, and the metadata keys still work.

The port is deliberately **not** persisted to the config file: it's discovery-published, and a
config-supplied value would raise an unanswerable question about whether it counts as explicit
(fail on conflict) or default (search the range).

### Registering for state

State is *pushed*, only to clients that ask, so an idle device with nobody watching sends nothing
at all. Register with the port you want it sent to:

| Address | Args | Behaviour |
|---------|------|-----------|
| `/jacklink/listeners/add` | `s "[ip:]port"` *or* `i port` | Register. A bare port implies `127.0.0.1`. Always answered with a **full snapshot of every state address**, even if that endpoint was already registered. |
| `/jacklink/listeners/del` | `s "[ip:]port"` *or* `i port` | Unregister. Silent if not present. |
| `/jacklink/listeners/clear` | *(none)* | Unregister everything. |

The snapshot goes to the endpoint named in the payload, **not** to the datagram's source — a
client generally sends from an ephemeral port and listens on a declared one. Rejected (with a log
line): a port outside `1`–`65535`, our own receive port, a non-loopback address while the socket is
bound to loopback, and any address that isn't a numeric IPv4 literal — host *names* are never
resolved, because `getaddrinfo` would block the single OSC thread for the resolver timeout and
stall every transport command behind it.

Snapshotting even a duplicate registration is the point, not sloppiness: if the *client* restarts
while the service doesn't, its registration is still here, so ignoring the duplicate would send
nothing and leave the client blank until something happened to change.

Registrations are **ephemeral** — in memory only, never written to the config file or any
database, and gone when the service exits. Re-register on startup, and re-register when the
`osc-port` key reappears. Sending the same `add` periodically is a reasonable heartbeat; it's
idempotent.

### State push

Sent to every registered listener, one message per datagram (the JSON payloads are easily
multi-KB, which makes a bundle a bad idea). Booleans are OSC `T`/`F`.

| Address | Args | Description |
|---------|------|-------------|
| `/jacklink/state/audio/available` | `bool` | Whether Link Audio is enabled at all (false with `-A`). Explicit, because with no metadata key to be missing there's nothing else to infer it from. |
| `/jacklink/state/audio/channels` | `s` JSON | Available channels grouped by peer: `[{"peer":…,"channels":[…]}, …]`. |
| `/jacklink/state/audio/peer-name` | `s` | The *effective* Link peer name being broadcast (override or hostname). |
| `/jacklink/state/audio/latency` | `f` ms | Applied receiver playout buffer. |
| `/jacklink/state/audio/sync-to-incoming` | `bool` | Applied Sync to Incoming Audio toggle. |
| `/jacklink/state/audio/sinks` | `s` JSON | `[{"key":"a3f2c19b4e0d","name":"Drums"}, …]` in display order. |
| `/jacklink/state/audio/sources` | `s` JSON | `[{"key":"7b10c9de2245","peer":"Alex's Move","channel":"Cue"}, …]` in display order. |
| `/jacklink/state/audio/source-status` | `s` JSON | Per-source live receive telemetry, key-tagged and in display order: `[{"key":…,"connected":…,"receiving":…,"buffered_ms":…,"dropouts":…,"arrival_offset_ms":…,"jitter_ms":…}, …]`. A few times per second, and immediately on a connect/disconnect. See [Reading the telemetry](#reading-the-telemetry). |
| `/jacklink/state/audio/capture-latency` | `f` ms | Effective send latency (auto-detected + trim). |
| `/jacklink/state/audio/playback-latency` | `f` ms | Effective receive latency (auto-detected + trim). |
| `/jacklink/state/audio/capture-latency-auto` | `f` ms | JACK auto-detected capture latency. |
| `/jacklink/state/audio/playback-latency-auto` | `f` ms | JACK auto-detected playback latency. |

The four JSON blobs stay whole strings on purpose. Each is a self-healing snapshot of its own
topic, and `sinks`/`sources` are the authority for **both** the slot set and the display order,
with telemetry joined to them by `key` — flattening them into per-slot addresses would throw that
atomicity away.

`/jacklink/state/...` is deliberately a different namespace from the `/jacklink/...` command
addresses the service accepts, so state can never be mistaken for a command in either direction.

Traffic is kept down three ways: nothing is built at all with no listeners registered; a payload
identical to the last one sent is skipped; and the noisy measured floats are quantized to 0.1 ms
before that comparison, so wobble can't defeat it. With every source disconnected the telemetry
payload is a constant all-zero array and the stream stops entirely. Against that, the whole state
is re-sent every ~2 s, so a dropped datagram can't leave a listener stale indefinitely.

The latency values are observation-only; adjust them with the `--capture-latency-trim-ms` /
`--playback-latency-trim-ms` CLI flags (or config), not over the wire.

### Commands

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
| `/jacklink/audio/sinks/set` | `string …` | Replace the whole sink list with these names, in display order. Sinks not named are removed; no arguments removes all. |
| `/jacklink/audio/source/add` | `string peer, string channel` | Append a source. Rejected if that pair is already present. |
| `/jacklink/audio/source/remove` | `string peer, string channel` *or* `string key` | Remove that source. |
| `/jacklink/audio/source/reset-dropouts` | *(none)*, `string key`, *or* `string peer, string channel` | Zero the dropout count — of every source with no arguments, otherwise of the one named. |
| `/jacklink/audio/sources/order` | `string …` | Set the display order by key. Omitted slots keep their relative order at the end. |
| `/jacklink/audio/sources/set` | `string peer, string channel, …` | Replace the whole source list with these pairs, in display order. Sources not named are removed; no arguments removes all. |

Most commands are *imperative and identity-based*: they name what to change rather than restating
the whole list, so a client never has to read-modify-write, and two clients issuing different
commands can't clobber each other. Each one stages a change to the desired list and lets a single
idempotent reconcile pass apply it, which is where all the validation lives.

The two `set` commands are the declarative exception, for restoring a whole saved arrangement at
once — see [Replacing the whole list](#replacing-the-whole-list).

Rejected entries (empty or duplicate sink name, duplicate source, slot-key collision) are skipped
and the rest is applied; the resulting canonical lists are pushed on
`/jacklink/state/audio/{sinks,sources}`, so a client's view self-corrects. A rejected *rename*
keeps its slot under the previous name — removal is only ever expressed by omitting an entry.

Note that the identifier is an *argument*, not part of the address: a sink name can contain a
`/`, which an OSC address can't encode.

```
# /jacklink/audio/sink/add       "Drums"
# /jacklink/audio/sink/rename    "Drums" "Kit"
# /jacklink/audio/sinks/order    "Bass" "Kit"
# /jacklink/audio/source/add     "Alex's Move" "Cue"
# /jacklink/audio/source/remove  "Alex's Move" "Cue"
```

**No delete-reverts-to-default.** The old metadata keys had one: removing `linkaudio/latency`
snapped it back to 100 ms, removing `linkaudio/peer-name` reverted to the hostname, removing
either list emptied it. OSC has no analogue of "unset a value" and that behaviour is simply gone —
set the default explicitly instead (an empty `peer-name` string still means "use the hostname").

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
