# Jack Transport Link

A service that bridges [Ableton's Link](https://github.com/Ableton/link) to and from
[Jack Transport](https://jackaudio.org/api/transport-design.html), allowing applications
that use *Jack Transport* to synchronize their timing with other applications that support
*Link*.

## Build Requirements

* a compiler that can build c++17
	* clang++, g++
* [cmake](https://cmake.org/) 3.16 or higher
* [conan](https://conan.io/)
	* `pip3 install conan`
	* make sure the *conan* executable is in your path

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
