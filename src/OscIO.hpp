#pragma once

#include <algorithm>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

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
  // Try to bind `port`. Returns false when the port is taken, leaving this object unbound so the
  // caller can decide whether to iterate or fail.
  //
  // Deliberately no SetAllowReuse(true): a failed bind *is* our conflict detector, and the posix
  // implementation sets both SO_REUSEADDR and SO_REUSEPORT, which would let two jack_transport_link
  // instances silently split one message stream. Don't "fix" this.
  bool bind(int port) {
    try {
      mSocket = std::make_unique<oscpack::UdpListeningReceiveSocket>(
          oscpack::IpEndpointName(oscpack::IpEndpointName::ANY_ADDRESS, port), this);
    } catch (const std::runtime_error &e) {
      mSocket.reset();
      return false;
    }
    // Track the bound port ourselves: UdpSocket::LocalPort() is only assigned in Connect(), not
    // Bind(), so it reads 0 for a listening socket.
    mPort = port;
    return true;
  }

  bool bound() const { return mSocket != nullptr; }
  int port() const { return mPort; }

  void run() {
    if (mSocket)
      mSocket->Run();
  }
  void stop() {
    if (mSocket)
      mSocket->AsynchronousBreak();
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
  // "[ip:]port" as a string (a bare port implies 127.0.0.1, mirroring the RNBO runner's own
  // listener scheme), or a plain int port.
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
        if (pos != portStr.size())
          return false;
      } catch (...) {
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

    oscpack::IpEndpointName endpoint(ip.c_str(), static_cast<int>(port));
    // GetHostByName yields 0 for an unresolvable name; ANY_ADDRESS isn't a destination.
    if (endpoint.address == 0 ||
        endpoint.address == oscpack::IpEndpointName::ANY_ADDRESS) {
      std::cerr << "warning: ignoring osc listener with unusable address \"" << ip << "\"\n";
      return false;
    }
    // Never feed ourselves.
    if (static_cast<int>(port) == mPort && endpoint.address == loopback_address) {
      std::cerr << "warning: ignoring osc listener that is our own receive port\n";
      return false;
    }
    out = endpoint;
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

  static const unsigned long loopback_address = 0x7f000001UL;

  std::unique_ptr<oscpack::UdpListeningReceiveSocket> mSocket;
  int mPort = 0;

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
