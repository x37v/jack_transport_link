#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <jack/jack.h>
#include <jack/metadata.h>
#include <jack/types.h>

#include <ableton/LinkAudio.hpp>
#include <ableton/link/HostTimeFilter.hpp>
#include <ableton/platforms/Config.hpp>
#include "LinkAudioRenderer.hpp"

#include <ip/IpEndpointName.h>
#include <osc/OscReceivedElements.h>

// Every atomic type touched by a JACK realtime callback must be implemented
// without a library fallback lock on each supported target.
static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(std::atomic<float>::is_always_lock_free);
static_assert(std::atomic<jack_nframes_t>::is_always_lock_free);
static_assert(std::atomic<size_t>::is_always_lock_free);

class OscIO;

class JackTransportLink {
public:
  using SinkRenderer = ableton::linkaudio::LinkAudioSinkRenderer<ableton::LinkAudio>;
  using SourceRenderer = ableton::linkaudio::LinkAudioSourceRenderer<ableton::LinkAudio>;

  enum class MIDIClockRunState { Running, Stopped, NeedsSync };

  // A requested (desired) sink. `key` binds the entry to an existing slot — that's how a
  // rename is expressed (same key, different name). An entry with only a name is
  // self-keying, since the key is derived from the name.
  struct DesiredSink {
    std::string key;
    std::string name;
  };
  // A requested (desired) source. An entry carrying a channel name is identity-bearing
  // (matched by exact peer + channel); an entry with only a key refers to an existing slot
  // and adopts its identity (used by order-only writes).
  struct DesiredSource {
    std::string key;
    std::string peer;
    std::string channel;
  };

  JackTransportLink(jack_client_t *client,
                    bool enableStartStopSync = true,
                    double initialBPM = 100.,
                    double initialQuantum = 4.,
                    float initialTimeSigDenom = 4.,
                    double initialTicksPerBeat = 1920.,
                    bool enableLinkAudio = true,
                    bool syncLink = true,
                    std::string configPath = "",
                    std::vector<std::string> sinkNames = {},
                    std::vector<std::pair<std::string, std::string>> sources = {},
                    std::string linkPeerName = "",
                    double captureLatencyTrimMs = 0.0,
                    double playbackLatencyTrimMs = 0.0,
                    double latencyMs = 100.0,
                    bool syncToIncomingAudio = true,
                    bool linkEnabled = true,
                    OscIO *osc = nullptr);
  ~JackTransportLink();

  void processEvents();

  // Handle one inbound OSC message. Called on the OSC thread by OscIO, which owns the socket and
  // is itself the oscpack PacketListener — we're no longer *a* packet listener, we just handle
  // messages.
  void processOscMessage(const oscpack::ReceivedMessage &m,
                         const oscpack::IpEndpointName &remoteEndpoint);
  // A listener registered (or re-registered) and is owed a full state snapshot. Called on the OSC
  // thread; the send itself has to happen on the main thread, so this only stages the endpoint and
  // processEvents drains it.
  void requestOscSnapshot(const oscpack::IpEndpointName &endpoint);

  static int processCallback(jack_nframes_t nframes, void *arg);
  static void timeBaseCallback(jack_transport_state_t state,
                               jack_nframes_t nframes, jack_position_t *pos,
                               int new_pos, void *arg);
  static int syncCallback(jack_transport_state_t state, jack_position_t *pos,
                          void *arg);
  static void propertyChangeCallback(jack_uuid_t subject, const char *key,
                                     jack_property_change_t change, void *arg);
  static void latencyCallback(jack_latency_callback_mode_t mode, void *arg);

private:
  // The body of processEvents, run with mControlMutex held. Split out so the lock is released
  // before flushOsc() issues its syscalls.
  void processEventsLocked();
  int processCallback(jack_nframes_t nframes);
  void timeBaseCallback(jack_transport_state_t state, jack_nframes_t nframes,
                        jack_position_t *pos, bool posIsNew);
  int syncCallback(jack_transport_state_t state, jack_position_t *pos);
  void propertyChangeCallback(jack_uuid_t subject, const char *key,
                              jack_property_change_t change);
  // Re-read JACK's measured capture/playback latency from our slot ports, then recompute
  // effective values. Called from processEvents (main thread) — NOT from the latency callback —
  // so the slot-vector reads are serialized with reconcileSinks/Sources on the same thread.
  void updateLatencyRanges();
  // Fold auto-detected latency + user trim (ms) into the effective frame offsets (atomics
  // read by the RT process callback) and re-send the read-only latency values.
  void recomputeEffectiveLatency();

  // The Link Audio state push. Each of these builds its payload, compares it against what was last
  // sent and queues a datagram only on a real change — and each returns before building anything
  // when no listener is registered, which is what makes the 4 Hz telemetry timer free on an
  // idle device.
  void sendLinkAudioAvailable();
  void sendLinkAudioChannels(const std::vector<ableton::LinkAudio::Channel>& channels);
  void sendLinkAudioPeerName();
  void sendLinkAudioLatencyMs();
  void sendLinkAudioSyncToIncoming();
  void sendLinkAudioSinks();
  void sendLinkAudioSources();
  void sendLinkAudioSourceStatus();
  void sendLinkAudioLatencies();
  // Payload builders, shared by the guarded senders and the unconditional snapshot path.
  std::string buildChannelsJson(
      const std::vector<ableton::LinkAudio::Channel>& channels) const;
  std::string buildSinksJson() const;
  std::string buildSourcesJson() const;
  std::string buildSourceStatusJson() const;
  // Publish the OSC port we actually bound, so a client can find us. This is the whole discovery
  // protocol: JACK wipes a client's properties when it disconnects, so this key disappearing and
  // reappearing *is* the "jack_transport_link restarted, re-register with me" signal.
  void setOscPortProperty();
  void setBPMProperty(double bpm);
  void setEnableStartStopProperty(bool enable);
  void setSyncProperty(bool sync);
  void setNumPeersProperty(size_t peers);
  void setLinkEnabledProperty();
  // Effective Link peer name = override (mLinkPeerName, if non-empty) else the hostname.
  std::string effectiveLinkPeerName() const;
  // Recompute the effective peer name; if it changed, rename the Link peer and re-send it.
  void applyLinkPeerName();
  bool updateLinkAudioSource();
  void updateAudioPortMetadata();
  // Zero the cumulative dropout count of one source (by slot key) or of every source
  // (target empty or "*"), and republish the status so the client sees the zeroes at once.
  void resetSourceDropouts(const std::string& target);
  // Reconcile the live slots against a desired list: add / remove / rename / reorder in one
  // idempotent pass. A reorder-only change touches nothing the RT thread reads.
  void reconcileSinks(const std::vector<DesiredSink>& desired);
  void reconcileSources(const std::vector<DesiredSource>& desired);
  // jack_deactivate() drops *all* of our ports' connections, not just the affected slot's, so
  // reconcile snapshots every connection (including the MIDI clock out) and restores it by name.
  struct PortConn { std::string mine; std::string other; bool output; };
  std::vector<PortConn> snapshotConnections() const;
  // `rename` maps an old port name to its replacement, so a renamed slot's live connections
  // follow it onto its new (re-hashed) ports.
  void restoreConnections(const std::vector<PortConn>& conns,
                          const std::map<std::string, std::string>& rename);
  // Register the stereo port pair for a slot: in_<key>_l/_r for sinks, out_<key>_l/_r for
  // sources. Identity-derived, so the same identity always gets the same port names.
  void registerSlotPorts(const std::string& key, bool input,
                         jack_port_t*& portL, jack_port_t*& portR);
  // The desired list staged for the next reconcile. Seeded from the live slots unless a
  // reconcile is already pending (so back-to-back commands compose). Caller holds mControlMutex
  // and must set the matching mNeedsReconcile* flag.
  std::vector<DesiredSink>& pendingSinks();
  std::vector<DesiredSource>& pendingSources();
  // Slot lookup; returns npos when absent. findSink() accepts a name or a key (for the OSC
  // commands, which take a human-typed identifier); findSinkByKey() is the internal, key-only
  // form used wherever the caller already has a key.
  size_t findSinkByKey(const std::string& key) const;
  size_t findSink(const std::string& nameOrKey) const;
  size_t findSource(const std::string& key) const;
  void saveConfig();

  void invalidateClockSyncBBT();

  // Queue one already-encoded datagram for sending. Encoding here (under mControlMutex) is
  // correct — it reads guarded state — but the syscall must not happen under the lock, so
  // processEvents flushes the outbox after releasing it.
  void queueOsc(std::string packet);
  void queueOscTo(const oscpack::IpEndpointName &endpoint, std::string packet);
  // Drain the outbox onto the socket. Only caller is processEvents: main thread, no locks held.
  void flushOsc();
  // Send every state address to one endpoint, unconditionally (no change guard) — this is what a
  // freshly-registered listener gets so it starts from a complete picture.
  void sendOscSnapshot(const oscpack::IpEndpointName &endpoint);
  // Re-send the low-rate state to every listener, bypassing the change guards. Counterweight to
  // those guards: a lost datagram can't leave a client stale indefinitely.
  void republishOscState();

  jack_client_t *mJackClient;
  // Not owned; outlives us (constructed in main outside the JACK reconnect loop). With --no-osc it
  // is present but unbound, so it can never gain a listener and every send gates itself off.
  OscIO *mOsc = nullptr;

  // mSampleRate must precede the slot vectors (renderers hold a reference to it)
  double mSampleRate;
  // Serializes non-real-time control operations arriving from the main loop,
  // JACK metadata callback, and OSC thread. Recursive because these operations
  // call helpers that also take a snapshot under the same lock.
  mutable std::recursive_mutex mControlMutex;
  // Link peer-name override (empty = auto/hostname). Declared before mLink because the
  // constructor seeds mLink's peer name from effectiveLinkPeerName(), which reads it.
  std::string mLinkPeerName;
  ableton::LinkAudio mLink;

  // An outgoing (send) slot: a named stereo channel announced to the Link session, fed by the
  // JACK input port pair in_<key>_l / in_<key>_r. Identity = the announced name.
  struct SinkSlot {
    std::string name;   // announced Link channel name; non-empty and unique
    std::string key;    // slotKey(name) — recomputed on rename
    std::unique_ptr<SinkRenderer> renderer;
    jack_port_t* portL = nullptr;
    jack_port_t* portR = nullptr;
  };
  // An incoming (receive) slot: subscribes to one exact advertised peer/channel and writes it
  // to the JACK output port pair out_<key>_l / out_<key>_r. Identity = peer + channel.
  struct SourceSlot {
    std::string peer;
    std::string channel;
    std::string key;    // slotKey(peer + '\0' + channel)
    std::unique_ptr<SourceRenderer> renderer;
    jack_port_t* portL = nullptr;
    jack_port_t* portR = nullptr;
    std::optional<ableton::ChannelId> currentId; // live connection; nullopt = disconnected
  };
  // Creation order, read by the RT process callback. Only ever mutated inside a
  // jack_deactivate() window (see reconcileSinks/reconcileSources), so the RT thread never
  // observes a partially-updated vector and no extra atomic is needed.
  std::vector<SinkSlot> mSinks;
  std::vector<SourceSlot> mSources;
  // Display order (slot keys), main thread only. Kept separate from the slot vectors so a
  // reorder only permutes these + republishes ORDER port metadata — no deactivate, so a
  // purely cosmetic change never drops a connection.
  std::vector<std::string> mSinkOrder;
  std::vector<std::string> mSourceOrder;

  jack_port_t *mMIDIClockOut = nullptr;
  MIDIClockRunState mMIDIClockRunState = MIDIClockRunState::Stopped;
  int mMIDIClockCount = 0;
  // first clock tick gets a delay, track it across process calls
  double mClockFrameDelay = 0;

  jack_port_t *mClickPort = nullptr;
  double mInternalBeat = 0.0;
  std::atomic<bool> mSyncLink;
  bool mWasSyncLink;

  int32_t mBeatLast = -1;
  int32_t mBarLast = -1;
  double mTickLast = -1.0;

  std::chrono::microseconds mTime;
  std::chrono::microseconds mTimeNext;

  jack_transport_state_t mTransportStateReportedLast =
      jack_transport_state_t::JackTransportStopped;

  std::atomic<float> mBPM;
  std::atomic<double> mLinkBPM;
  double mBPMLast;
  double mQuantum;
  double mInitialQuantum; // time sig num, called quantum in link
  float mInitialTimeSigDenom;
  double mInitialTicksPerBeat;

  jack_uuid_t mJackClientUUID;

  std::atomic<bool> mReportBPM{false};
  std::atomic<bool> mReportLinkSync{false};
  std::atomic<bool> mReportStartStopEnable{false};
  // Peer count recorded by Link's callback thread, published from processEvents.
  std::atomic<size_t> mNumPeers{0};
  std::atomic<bool> mReportNumPeers{false};

  std::atomic<bool> mChannelsChanged{false};
  bool mLinkAudioEnabled = false;
  // true once jack_activate() has run, so reconcile knows whether it needs to bracket its
  // port/renderer mutations with deactivate/activate (the constructor builds the initial
  // slots before the client is ever activated).
  bool mJackActivated = false;

  // I/O latency compensation. The send path stamps audio to the beat it was actually
  // captured at (mTimeNext - capture latency); the receive path targets the beat the audio
  // will actually be heard at (mTimeNext + playback latency). Effective = auto-detected from
  // JACK + a per-direction user trim (covers converter/SPI latency JACK can't see and lets us
  // correct the driver's guessed capture/playback split). Frame values are read in the RT
  // process callback; recomputed non-RT whenever auto, trim, or sample rate changes.
  std::atomic<jack_nframes_t> mAutoCaptureLatencyFrames{0};
  std::atomic<jack_nframes_t> mAutoPlaybackLatencyFrames{0};
  std::atomic<double> mCaptureLatencyTrimMs{0.0};
  std::atomic<double> mPlaybackLatencyTrimMs{0.0};
  // Receiver playout buffer in milliseconds (converted to beats at the current tempo in the
  // renderer). Configurable; default 100ms, clamped to [0, 2000].
  std::atomic<float> mLatencyMs{100.0f};
  // "Sync to Incoming Audio" (formerly Ableton's "Monitoring Mode"): when true, delay the local
  // transport timeline (the reported JACK BBT) by the receive buffer so transport-locked local
  // generators align with the incoming audio; when false, the transport runs live. The receive
  // buffer (mLatencyMs) is ALWAYS applied to incoming playout regardless of this toggle (network
  // buffers arrive late and need it) — this only gates the transport-BBT shift (timeBaseCallback).
  // Default false.
  std::atomic<bool> mSyncToIncomingAudio{false};
  std::atomic<jack_nframes_t> mEffCaptureLatencyFrames{0};
  std::atomic<jack_nframes_t> mEffPlaybackLatencyFrames{0};
  // Set by the latency callback (JACK notification thread) on graph/buffer-size changes; drained
  // in processEvents (main thread), which reads the port latency ranges and republishes. Deferring
  // off the notification thread avoids both racing rebuildAudioPorts() on the port vectors and the
  // illegal jack_set_property-from-notification-thread call.
  std::atomic<bool> mNeedsRecomputeLatency{false};
  // Set when the playout-buffer (mLatencyMs) value changes; drained in processEvents to
  // republish the (possibly clamped/reverted) value off the notification thread.
  std::atomic<bool> mNeedsPublishLatencyMs{false};
  // Same, for the sync-to-incoming-audio toggle.
  std::atomic<bool> mNeedsPublishSyncToIncoming{false};

  // Master Link on/off. When false we call mLink.enable(false), leaving the Link session so
  // peers don't see us at all (also stops tempo sync + Link Audio); jtl keeps running as the
  // local JACK transport master. mLink.enable() is not RT-safe, so the desired state is applied
  // from processEvents. Default true.
  std::atomic<bool> mLinkEnabledDesired{true};
  std::atomic<bool> mNeedsApplyLinkEnabled{false};
  std::atomic<bool> mNeedsPublishLinkEnabled{false};

  // Desired sink/source lists staged by the OSC handlers, applied (reconciled) in processEvents.
  // Guarded by mControlMutex.
  std::vector<DesiredSink> mDesiredSinks;
  std::vector<DesiredSource> mDesiredSources;
  std::atomic<bool> mNeedsReconcileSinks{false};
  std::atomic<bool> mNeedsReconcileSources{false};
  std::atomic<bool> mNeedsSourceUpdate{false};
  std::atomic<bool> mReportLinkAudioChannels{false};
  std::atomic<bool> mReportLinkAudioSource{false};
  // Pending dropout-count reset requested over OSC; drained in processEvents, since the reset also
  // re-sends the status and sends have to happen on the main thread.
  std::string mResetDropoutsTarget;
  std::atomic<bool> mNeedsResetDropouts{false};

  std::string mConfigPath;
  std::atomic<bool> mNeedsSaveConfig{false};
  std::chrono::steady_clock::time_point mLastConfigSave{};
  // Throttle for periodic source-status metadata publishing (see processEvents).
  std::chrono::steady_clock::time_point mLastHealthPublish{};

  // Encoded datagrams waiting to go out, in order. `broadcast` = every listener; otherwise the
  // single endpoint in `dest` (the snapshot path). Guarded by mControlMutex.
  struct OscOut {
    bool broadcast = true;
    oscpack::IpEndpointName dest;
    std::string packet;
  };
  std::vector<OscOut> mOscOutbox;
  // What the listeners were last sent, per state address; a send whose payload matches is skipped.
  // nullopt = never sent, which always sends — that's also how the periodic re-publish forces one.
  std::optional<bool> mSentAvailable;
  std::optional<std::string> mSentChannelsJson;
  std::optional<std::string> mSentPeerName;
  std::optional<float> mSentLatencyMs;
  std::optional<bool> mSentSyncToIncoming;
  std::optional<std::string> mSentSinksJson;
  std::optional<std::string> mSentSourcesJson;
  std::optional<std::string> mSentSourceStatusJson;
  std::optional<float> mSentCaptureLatencyMs;
  std::optional<float> mSentPlaybackLatencyMs;
  std::optional<float> mSentCaptureLatencyAutoMs;
  std::optional<float> mSentPlaybackLatencyAutoMs;
  // Listeners owed a full snapshot, staged from the OSC thread. Guarded by mControlMutex.
  std::vector<oscpack::IpEndpointName> mPendingOscSnapshots;
  std::atomic<bool> mNeedsOscSnapshots{false};
  // Throttle for the periodic low-rate re-publish (see republishOscState).
  std::chrono::steady_clock::time_point mLastOscRepublish{};

  // The effective Link peer name currently announced (mirrors mLink's peer name), so
  // applyLinkPeerName() only calls setPeerName() when it actually changes.
  std::string mAppliedLinkPeerName;
  std::atomic<bool> mNeedsApplyPeerName{false};
  // set when port labels/grouping need to be (re)applied to our audio ports
  bool mUpdatePortMeta = false;
};
