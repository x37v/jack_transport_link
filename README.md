# Jack Transport Link

A service that bridges [Ableton's Link](https://github.com/Ableton/link) to and from
[Jack Transport](https://jackaudio.org/api/transport-design.html), allowing applications
that use *Jack Transport* to synchronize their timing with other applications that support
*Link*.

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

Run with the `-h` switch to discover more details.

## Notes

Since jack transport doesn't allow clients to request tempo, we use the
metadata API to do tempo requests.  You must use jack 1.9.13 or newer for
metadata support. You can request the tempo even if the transport isn't running.

The key for bpm is `http://www.x37v.info/jack/metadata/bpm` and the type is `https://www.w3.org/2001/XMLSchema#decimal`.

Here is an example of how to set the tempo to 150 beats per minute.

```shell
jack_property --client jack-transport-link http://www.x37v.info/jack/metadata/bpm 150.0 https://www.w3.org/2001/XMLSchema#decimal
```

To get the current bpm property.
```shell
jack_property --client jack-transport-link --list http://www.x37v.info/jack/metadata/bpm
```

## OSC Control

Pass `-o <port>` to enable the OSC listener. All messages are sent to that UDP port.

| Address | Argument | Description |
|---------|----------|-------------|
| `/jacklink/bpm` | `float` or `double` | Set the tempo in beats per minute. |
| `/jacklink/beattime` | `float` or `double` | Seek the transport to the given beat position. |
| `/jacklink/sync` | `bool` | Enable (`true`) or disable (`false`) Link sync. |
| `/jacklink/rolling` | `bool` | Start (`true`) or stop (`false`) the transport. |
| `/jacklink/linkaudio/source` | `string` (JSON) | Set source filter for all receivers (object or array). See [Link Audio](#link-audio). |
| `/jacklink/linkaudio/source/<N>` | `string` (JSON object) | Set source filter for receiver N (zero-based). |

## Link Audio

Link Audio streams real-time audio between peers in a Link session with beat-grid alignment. It is enabled by default (disable with `--no-link-audio`).

Each stereo send pair (configurable with `--link-audio-in-stereo-channels`, default `1`) registers JACK input ports `in_1`/`in_2`, `in_3`/`in_4`, etc. and announces a named stereo channel ("Send 1", "Send 2", …) to the Link session.

Each stereo receive pair (configurable with `--link-audio-out-stereo-channels`, default `1`) registers JACK output ports `out_1`/`out_2`, `out_3`/`out_4`, etc. and subscribes to one incoming channel ("Recv 1", "Recv 2", …). Send and receive counts are independent.

### Selecting a source

By default the first available peer is subscribed automatically. To select a specific peer, set the `linkaudio/source` Jack metadata property or send an OSC message to `/jacklink/linkaudio/source`.

The value is either a **single JSON object** (sets receiver 0 only) or a **JSON array** (sets each receiver by index). Each element is a `{"peer":"…","channel":"…"}` filter — both fields are case-insensitive substring matches and either can be omitted. `{}` reverts that receiver to auto-selection.

The property always reflects the current connection state as a JSON array, one entry per receive pair — `{}` for any receiver with no active source.

```shell
# Set receiver 0 only (indexed property — single object)
jack_property --client jack-transport-link \
  http://www.x37v.info/jack/metadata/linkaudio/source/0 \
  '{"peer":"Push"}' application/json

# Set receiver 1 only
jack_property --client jack-transport-link \
  http://www.x37v.info/jack/metadata/linkaudio/source/1 \
  '{"peer":"Alex","channel":"Cue"}' application/json

# Set all receivers at once (array form on the base property)
jack_property --client jack-transport-link \
  http://www.x37v.info/jack/metadata/linkaudio/source \
  '[{"peer":"Push"},{"peer":"Alex"}]' application/json

# Revert receiver 0 to auto (others unchanged)
jack_property --client jack-transport-link \
  http://www.x37v.info/jack/metadata/linkaudio/source/0 \
  '{}' application/json

# Read all current connections (array, one entry per receiver)
jack_property --client jack-transport-link --list \
  http://www.x37v.info/jack/metadata/linkaudio/source

# Read the current connection for receiver 0
jack_property --client jack-transport-link --list \
  http://www.x37v.info/jack/metadata/linkaudio/source/0

# List available channels
jack_property --client jack-transport-link --list \
  http://www.x37v.info/jack/metadata/linkaudio/channels
```

The `linkaudio/channels` property is read-only and updated automatically. Its value is a JSON array grouped by peer:

```json
[{"peer": "Alex's MacBook", "channels": ["Link Audio"]}, {"peer": "Push 3", "channels": ["Main", "Cue"]}]
```

## TODO

* Latency Compensation computation
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
