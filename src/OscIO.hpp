#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#if !defined(_WIN32)
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <ip/UdpSocket.h>
#include <osc/OscPacketListener.h>
#include <osc/OscReceivedElements.h>

#include "JackTransportLink.hpp"

// Owns the OSC receive socket, the listener registry and the send path.
//
// Constructed in main() *outside* the JACK reconnect loop, so it outlives every
// JackTransportLink. That is required, not cosmetic: the port we bind is published as JACK
// metadata and cached by clients, so a socket rebuilt per JACK session could come back on a
// different port after a conflict — and registrations need to survive a JACK blip that a client
// has no reason to notice.
//
// oscpack bakes the PacketListener in at construction (UdpListeningReceiveSocket's ctor attaches
// it), so this object is the listener: it handles the /jacklink/listeners/* routes itself and
// forwards everything else to the current JackTransportLink.
class OscIO : public oscpack::OscPacketListener {
public:
  // Try to bind `port`, on loopback only unless `anyAddress`. Returns false when the port is
  // taken, leaving this object unbound so the caller can decide whether to iterate or fail.
  //
  // Loopback is the default because the command surface is unauthenticated and partly
  // destructive: an argument-less /jacklink/audio/sinks/set is a legal "remove every sink", and
  // the reconcile persists that to the config file within a second. Binding every interface is
  // opt-in (--osc-bind-any).
  //
  // Deliberately no SetAllowReuse(true): a failed bind *is* our conflict detector, and the posix
  // implementation sets both SO_REUSEADDR and SO_REUSEPORT, which would let two jack_transport_link
  // instances silently split one message stream. Don't "fix" this.
  bool bind(int port, bool anyAddress) {
    try {
      const unsigned long address =
          anyAddress ? oscpack::IpEndpointName::ANY_ADDRESS : loopback_address;
      mSocket = std::make_unique<oscpack::UdpListeningReceiveSocket>(
          oscpack::IpEndpointName(address, port), this);
    } catch (const std::runtime_error &e) {
      mSocket.reset();
      return false;
    }
    // Track the bound port ourselves: UdpSocket::LocalPort() is only assigned in Connect(), not
    // Bind(), so it reads 0 for a listening socket.
    mPort = port;
    mAnyAddress = anyAddress;
    return true;
  }

  bool bound() const { return mSocket != nullptr; }
  int port() const { return mPort; }
  bool anyAddress() const { return mAnyAddress; }

  // The receive loop, and the only place that decides the OSC thread is finished.
  //
  // Restartable on purpose. oscpack's Run() returns not only when we ask it to: the vendored
  // posix multiplexer treats any 8-byte datagram equal to "__stop_" *on the listening socket*
  // as an asynchronous break (ip/posix/UdpSocket.h), so any host that can reach us could
  // otherwise retire the OSC thread — and with it listener registration, transport commands and
  // the whole Link Audio bridge — while the daemon kept running, with nothing to restart it and
  // no log line. Run() clears its own break flag on entry, so re-entering it is clean and costs
  // nothing but a log line. (The real fix belongs upstream in the oscpack fork: a break command
  // has no business arriving over the network when AsynchronousBreak already has a private pipe.)
  void run() {
    if (!mSocket)
      return;
    while (!mStopping.load(std::memory_order_acquire)) {
      bool threw = false;
      try {
        mSocket->Run();
      } catch (const std::exception &e) {
        threw = true;
        std::cerr << "error in osc receive loop: " << e.what() << "\n";
      } catch (...) {
        threw = true;
        std::cerr << "unknown error in osc receive loop\n";
      }
      if (mStopping.load(std::memory_order_acquire))
        break;
      if (threw) {
        // Don't spin hot on a socket that keeps failing immediately.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      } else {
        std::cerr << "warning: osc receive loop broke unexpectedly (a remote \"__stop_\" "
                     "datagram, most likely); restarting\n";
      }
    }
    mStopped.store(true, std::memory_order_release);
  }

  // Ask run() to return. Called from main() right before joining the OSC thread.
  //
  // The retry loop closes a race that only exists because run() restarts: Run() clears the break
  // flag on entry, so an AsynchronousBreak() that lands between one Run() returning and the next
  // one starting is simply forgotten, and the join would hang forever. Re-asking until the thread
  // confirms it is out costs a few milliseconds at shutdown.
  void stop() {
    mStopping.store(true, std::memory_order_release);
    if (!mSocket)
      return;
    while (!mStopped.load(std::memory_order_acquire)) {
      mSocket->AsynchronousBreak();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  // Point the forwarding path at a JackTransportLink (nullptr while there is no JACK session).
  // Taking mTargetMutex is what makes clearing it safe: it blocks until any in-flight forward on
  // the OSC thread has returned, so main() can then destroy the target.
  void setTarget(JackTransportLink *target) {
    std::lock_guard<std::mutex> lock(mTargetMutex);
    mTarget = target;
  }

  // Cheap enough to call at 100 Hz — this is the "zero listeners => zero work" gate, so it must
  // not take a lock or the gate would cost more than the work it skips.
  bool hasListeners() const { return mHasListeners.load(std::memory_order_acquire); }

  // Send to every registered listener.
  //
  // MAIN THREAD ONLY. oscpack's SendTo writes the socket implementation's sendToAddr_ member
  // (ip/posix/UdpSocket.h), so exactly one thread may ever call it. Concurrent ReceiveFrom on the
  // OSC thread is fine — that one uses a stack-local fromAddr.
  void sendToListeners(const std::string &packet) {
    if (!mSocket || packet.empty())
      return;
    std::vector<oscpack::IpEndpointName> listeners;
    {
      std::lock_guard<std::mutex> lock(mListenersMutex);
      listeners = mListeners;
    }
    for (const auto &l : listeners)
      mSocket->SendTo(l, packet.data(), packet.size());
  }

  // Send to one endpoint (the snapshot path). MAIN THREAD ONLY, see sendToListeners.
  void sendTo(const oscpack::IpEndpointName &endpoint, const std::string &packet) {
    if (!mSocket || packet.empty())
      return;
    mSocket->SendTo(endpoint, packet.data(), packet.size());
  }

public:
  // Every inbound datagram lands here first, and nothing thrown by parsing one may escape.
  //
  // oscpack's OscPacketListener::ProcessPacket constructs ReceivedPacket/ReceivedMessage, which
  // throw MalformedPacketException on any non-OSC payload (a size that isn't a multiple of 4 is
  // enough), and the vendored Run() loop rethrows. Without this catch a single stray or fuzzed
  // datagram — `echo -n x | nc -u host 3234` — reached std::terminate and took JACK transport,
  // MIDI clock and Link Audio down with it.
  void ProcessPacket(const char *data, int size,
                     const oscpack::IpEndpointName &remoteEndpoint) override {
    try {
      oscpack::OscPacketListener::ProcessPacket(data, size, remoteEndpoint);
    } catch (const std::exception &e) {
      logDroppedPacket(remoteEndpoint, e.what());
    } catch (...) {
      logDroppedPacket(remoteEndpoint, "unknown error");
    }
  }

protected:
  void ProcessMessage(const oscpack::ReceivedMessage &m,
                      const oscpack::IpEndpointName &remoteEndpoint) override {
    const char *addr = m.AddressPattern();
    try {
      if (std::strcmp("/jacklink/listeners/add", addr) == 0) {
        oscpack::IpEndpointName endpoint;
        if (parseEndpoint(m, endpoint))
          addListener(endpoint);
        return;
      }
      if (std::strcmp("/jacklink/listeners/del", addr) == 0) {
        oscpack::IpEndpointName endpoint;
        if (parseEndpoint(m, endpoint))
          removeListener(endpoint);
        return;
      }
      if (std::strcmp("/jacklink/listeners/clear", addr) == 0) {
        clearListeners();
        return;
      }
    } catch (oscpack::Exception &e) {
      std::cerr << "error while parsing message: " << addr << ": " << e.what() << "\n";
      return;
    }

    std::lock_guard<std::mutex> lock(mTargetMutex);
    if (mTarget)
      mTarget->processOscMessage(m, remoteEndpoint);
  }

private:
  // Report a datagram we threw away, at most one line a second. A malformed-packet flood is
  // exactly what an attacker can produce cheapest, and a line each would turn it into a disk
  // filler. OSC thread only, so the counters need no synchronization.
  void logDroppedPacket(const oscpack::IpEndpointName &remoteEndpoint, const char *what) {
    ++mDroppedPackets;
    const auto now = std::chrono::steady_clock::now();
    if (mHasDroppedLog && now - mLastDroppedLog < std::chrono::seconds(1))
      return;
    char from[oscpack::IpEndpointName::ADDRESS_AND_PORT_STRING_LENGTH];
    remoteEndpoint.AddressAndPortAsString(from);
    std::cerr << "warning: dropped malformed osc packet from " << from << ": " << what;
    if (mDroppedPackets > 1)
      std::cerr << " (" << mDroppedPackets << " dropped so far)";
    std::cerr << "\n";
    mLastDroppedLog = now;
    mHasDroppedLog = true;
  }

  // "[ip:]port" as a string (a bare port implies 127.0.0.1, mirroring the RNBO runner's own
  // listener scheme), or a plain int port.
  //
  // The address must be numeric IPv4. Passing a name to IpEndpointName would resolve it with a
  // blocking getaddrinfo on this, the single OSC thread: one `/jacklink/listeners/add
  // "unresolvable.example:9000"` stalled every transport command for the resolver timeout.
  // Legitimate clients only ever send numeric addresses, so this costs nothing real.
  bool parseEndpoint(const oscpack::ReceivedMessage &m,
                     oscpack::IpEndpointName &out) const {
    auto arg = m.ArgumentsBegin();
    if (arg == m.ArgumentsEnd())
      return false;

    std::string ip("127.0.0.1");
    long port = 0;
    if (arg->IsString()) {
      const std::string s = arg->AsStringUnchecked();
      std::string portStr = s;
      const auto colon = s.rfind(':');
      if (colon != std::string::npos) {
        ip = s.substr(0, colon);
        portStr = s.substr(colon + 1);
        if (ip.empty())
          ip = "127.0.0.1";
      }
      try {
        size_t pos = 0;
        port = std::stol(portStr, &pos);
        if (pos != portStr.size()) {
          std::cerr << "warning: ignoring osc listener with non-numeric port \"" << portStr
                    << "\"\n";
          return false;
        }
      } catch (...) {
        std::cerr << "warning: ignoring osc listener with non-numeric port \"" << portStr
                  << "\"\n";
        return false;
      }
    } else if (arg->IsInt32()) {
      port = arg->AsInt32Unchecked();
    } else if (arg->IsInt64()) {
      port = static_cast<long>(arg->AsInt64Unchecked());
    } else {
      return false;
    }

    if (port <= 0 || port > 65535) {
      std::cerr << "warning: ignoring osc listener with out-of-range port " << port << "\n";
      return false;
    }

    struct in_addr parsed;
    if (inet_pton(AF_INET, ip.c_str(), &parsed) != 1) {
      std::cerr << "warning: ignoring osc listener with non-numeric address \"" << ip
                << "\" (a numeric IPv4 address is required; names are not resolved)\n";
      return false;
    }
    const auto address = static_cast<unsigned long>(ntohl(parsed.s_addr));
    // 0.0.0.0 parses fine but isn't a destination.
    if (address == 0 || address == oscpack::IpEndpointName::ANY_ADDRESS) {
      std::cerr << "warning: ignoring osc listener with unusable address \"" << ip << "\"\n";
      return false;
    }
    // Never feed ourselves. Our own state pushes never match a command address, so an echo
    // parses and drops — this is about wasted traffic and lock contention, not a loop.
    if (static_cast<int>(port) == mPort &&
        (isLoopback(address) || (mAnyAddress && isLocalInterfaceAddress(address)))) {
      std::cerr << "warning: ignoring osc listener that is our own receive port\n";
      return false;
    }
    // Bound to loopback, we can only *send* from loopback: a sendto() to a routable address
    // from a 127.0.0.1-bound socket fails, and oscpack's SendTo discards the error. Registering
    // such a listener would be a silent no-op, so say so instead.
    if (!mAnyAddress && !isLoopback(address)) {
      std::cerr << "warning: ignoring non-local osc listener \"" << ip
                << "\": the osc socket is bound to loopback only (see --osc-bind-any)\n";
      return false;
    }
    out = oscpack::IpEndpointName(address, static_cast<int>(port));
    return true;
  }

  void addListener(const oscpack::IpEndpointName &endpoint) {
    {
      std::lock_guard<std::mutex> lock(mListenersMutex);
      if (std::find(mListeners.begin(), mListeners.end(), endpoint) == mListeners.end())
        mListeners.push_back(endpoint);
      mHasListeners.store(!mListeners.empty(), std::memory_order_release);
    }
    // Snapshot even when the entry was already there. That's load-bearing: if the *client*
    // restarted but we did not, its registration is still here, so an ignore-duplicates path
    // would send nothing and leave the client blank until something happened to change.
    //
    // The snapshot has to go out from the main thread (see sendToListeners), so hand the endpoint
    // to the target and let processEvents drain it.
    std::lock_guard<std::mutex> lock(mTargetMutex);
    if (mTarget)
      mTarget->requestOscSnapshot(endpoint);
  }

  void removeListener(const oscpack::IpEndpointName &endpoint) {
    std::lock_guard<std::mutex> lock(mListenersMutex);
    mListeners.erase(std::remove(mListeners.begin(), mListeners.end(), endpoint),
                     mListeners.end());
    mHasListeners.store(!mListeners.empty(), std::memory_order_release);
  }

  void clearListeners() {
    std::lock_guard<std::mutex> lock(mListenersMutex);
    mListeners.clear();
    mHasListeners.store(false, std::memory_order_release);
  }

  // The whole 127.0.0.0/8, not just 127.0.0.1: every one of them reaches us.
  static bool isLoopback(unsigned long address) {
    return ((address >> 24) & 0xFFUL) == 127UL;
  }

#if !defined(_WIN32)
  // Does this address belong to one of our own interfaces? Only consulted when the port already
  // matches ours, so the getifaddrs walk happens at most once per listener registration.
  static bool isLocalInterfaceAddress(unsigned long address) {
    struct ifaddrs *addrs = nullptr;
    if (getifaddrs(&addrs) != 0)
      return false;
    bool found = false;
    for (auto *a = addrs; a != nullptr && !found; a = a->ifa_next) {
      if (a->ifa_addr == nullptr || a->ifa_addr->sa_family != AF_INET)
        continue;
      const auto *in = reinterpret_cast<const struct sockaddr_in *>(a->ifa_addr);
      found = static_cast<unsigned long>(ntohl(in->sin_addr.s_addr)) == address;
    }
    freeifaddrs(addrs);
    return found;
  }
#else
  static bool isLocalInterfaceAddress(unsigned long) { return false; }
#endif

  static const unsigned long loopback_address = 0x7f000001UL;

  std::unique_ptr<oscpack::UdpListeningReceiveSocket> mSocket;
  int mPort = 0;
  bool mAnyAddress = false;

  // Shutdown handshake between stop() (main thread) and run() (OSC thread); see both.
  std::atomic<bool> mStopping{false};
  std::atomic<bool> mStopped{false};

  // Dropped-packet log throttle. OSC thread only.
  std::chrono::steady_clock::time_point mLastDroppedLog{};
  bool mHasDroppedLog = false;
  unsigned long mDroppedPackets = 0;

  // Registered state-push destinations. Ephemeral and in memory only: never written to the config
  // file or any database. Persisting them would mean a stale entry survives a reboot and we blast
  // telemetry at a dead port forever, with nothing to tell us to stop — a client re-registers on
  // startup (and the osc-port key disappearing and reappearing is its cue to), which is cheap.
  //
  // Its own mutex, deliberately not the target's mControlMutex: the registry has nothing to do
  // with control state, and sharing that lock would make an `add` block behind a reconcile.
  std::vector<oscpack::IpEndpointName> mListeners;
  mutable std::mutex mListenersMutex;
  std::atomic<bool> mHasListeners{false};

  JackTransportLink *mTarget = nullptr;
  mutable std::mutex mTargetMutex;
};
