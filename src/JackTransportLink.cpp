#include "JackTransportLink.hpp"

#include <jack/midiport.h>
#include <jack/uuid.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>

// debugging defines

// #define DO_CLICK_OUT

// send midi start at the start of every bar
// #define MIDI_SEND_REPEATED_STARTS

#define MIDI_PPQ 24

namespace {
const char *decimal_type = "https://www.w3.org/2001/XMLSchema#decimal";
const char *int_type = "https://www.w3.org/2001/XMLSchema#integer";
const char *bool_type = "https://www.w3.org/2001/XMLSchema#boolean";
const std::string bpm_key("http://www.x37v.info/jack/metadata/bpm");
const std::string linksync_key("http://www.x37v.info/jack/metadata/linksync");
const std::string
    linknumpeers_key("http://www.x37v.info/jack/metadata/linkpeers");
const std::string
    start_stop_key("http://www.x37v.info/jack/metadata/link/start-stop-sync");
// Writable master Link on/off. false => leave the Link session (invisible to peers, no tempo
// sync, no Link Audio); jtl still runs as the local JACK transport master.
const std::string
    link_enabled_key("http://www.x37v.info/jack/metadata/link/enabled");
// The explicit, ordered source/sink lists. Both are R/W: a client writes the desired list,
// and we publish the canonical (key-tagged, display-ordered) applied value back.
const std::string
    linkaudio_sinks_key("http://www.x37v.info/jack/metadata/linkaudio/sinks");
const std::string
    linkaudio_sources_key("http://www.x37v.info/jack/metadata/linkaudio/sources");
// Per-source live receive telemetry (connected, buffered ms, dropout count, jitter ms),
// key-tagged and in display order, published periodically so a client can show receive
// quality. Kept out of the writable `sources` list so ~4 Hz telemetry doesn't churn a
// property that is also a write target.
const std::string
    linkaudio_source_status_key("http://www.x37v.info/jack/metadata/linkaudio/source-status");
// Write-only command: zero a source's cumulative dropout count (value = slot key, or "*" for
// every source), so the count reads as "dropouts since I last changed a setting". We remove the
// property once we've acted on it, which is also what lets the same value be sent twice.
const std::string
    linkaudio_reset_dropouts_key("http://www.x37v.info/jack/metadata/linkaudio/reset-dropouts");
const char *string_type = "text/plain";
// JACK's standard port presentation metadata (grouping + display name), set on our own
// audio ports so patchbays (e.g. the RNBO runner's graph editor) group and label them.
const std::string port_group_key(JACK_METADATA_PORT_GROUP);
const std::string pretty_name_key(JACK_METADATA_PRETTY_NAME);
const std::string order_key(JACK_METADATA_ORDER);
const char *link_audio_port_group = "jack-link-audio";
// JACK recommends this type for the order key; it also matches what the RNBO runner parses.
const char *order_type = "http://www.w3.org/2001/XMLSchema#int";
const std::string
    linkaudio_channels_key("http://www.x37v.info/jack/metadata/linkaudio/channels");
// The local Link peer name broadcast to the session, decoupled from the JACK client name.
const std::string
    linkaudio_peer_name_key("http://www.x37v.info/jack/metadata/linkaudio/peer-name");
// Writable receiver playout buffer, in milliseconds (converted to beats at the current tempo).
const std::string
    linkaudio_latency_key("http://www.x37v.info/jack/metadata/linkaudio/latency");
// Writable "Sync to Incoming Audio" toggle: whether to delay the local transport timeline to
// match the (always-applied) receive buffer, so transport-locked local generators align with
// incoming audio. It does NOT gate the receive buffer itself.
const std::string
    linkaudio_sync_key("http://www.x37v.info/jack/metadata/linkaudio/sync-to-incoming");
// Read-only (GET) effective I/O latency actually applied to the beat mapping, in milliseconds,
// plus the auto-detected-only value (effective = auto + user trim). Published so a client can
// display what compensation is in effect.
const std::string
    linkaudio_capture_latency_key("http://www.x37v.info/jack/metadata/linkaudio/capture-latency");
const std::string
    linkaudio_playback_latency_key("http://www.x37v.info/jack/metadata/linkaudio/playback-latency");
const std::string
    linkaudio_capture_latency_auto_key("http://www.x37v.info/jack/metadata/linkaudio/capture-latency-auto");
const std::string
    linkaudio_playback_latency_auto_key("http://www.x37v.info/jack/metadata/linkaudio/playback-latency-auto");
const std::array<std::string, 2> true_values = {"true", "1"};

const std::array<uint8_t, 1> midi_clock_buf = {248};
const std::array<uint8_t, 1> midi_start_buf = {250};
const std::array<uint8_t, 1> midi_stop_buf = {252};

// helper to deal with dealloc and std::string
bool get_property(jack_uuid_t subject, const std::string &key,
                  std::string &value_out, std::string &type_out) {
  char *values = nullptr;
  char *types = nullptr;
  if (jack_get_property(subject, key.c_str(), &values, &types) != 0)
    return false;
  if (values) {
    value_out = std::string(values);
    jack_free(values);
  }
  if (types) {
    type_out = std::string(types);
    jack_free(types);
  }
  return true;
}

// Sanitize a requested playout-buffer value: non-finite (NaN/inf) reverts to the default,
// otherwise clamp to [0, 2000]. NaN must be caught here — std::clamp passes NaN through, and a
// NaN mLatencyMs would break the self-feedback guard (NaN != NaN) and produce NaN beat targets.
double clampLatencyMs(double v) {
  if (!std::isfinite(v)) return 100.0;
  return std::clamp(v, 0.0, 2000.0);
}

std::optional<double>
GetOscDouble(const oscpack::ReceivedMessageArgument &arg) {
  if (arg.IsDouble()) {
    return arg.AsDoubleUnchecked();
  }
  if (arg.IsFloat()) {
    return static_cast<double>(arg.AsFloatUnchecked());
  }
  if (arg.IsInt64()) {
    return static_cast<double>(arg.AsInt64());
  }
  if (arg.IsInt32()) {
    return static_cast<double>(arg.AsInt32());
  }
  return std::nullopt;
}

// A slot's key: the low 48 bits of an FNV-1a-64 hash of its identity, as 12 lowercase hex
// digits. Derived, never allocated or persisted — the same identity always yields the same key
// (and therefore the same JACK port names) across restarts and toolchains.
//
// FNV-1a rather than std::hash: std::hash is implementation-defined and not stable across
// libstdc++ versions or platforms, which would silently change our port names (breaking
// connections saved in a set) on a toolchain bump.
std::string slotKey(const std::string& identity) {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (unsigned char c : identity) {
    h ^= static_cast<uint64_t>(c);
    h *= 0x100000001b3ULL;
  }
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%012llx",
                static_cast<unsigned long long>(h & 0x0000ffffffffffffULL));
  return std::string(buf);
}

// A sink is identified by its announced channel name.
std::string sinkSlotKey(const std::string& name) { return slotKey(name); }

// A source is identified by the exact peer name + channel name. Link's ChannelId is only valid
// for a channel's lifetime, so it can't be persisted. The NUL separator keeps ("a","bc") and
// ("ab","c") from hashing to the same key.
std::string sourceSlotKey(const std::string& peer, const std::string& channel) {
  std::string identity = peer;
  identity.push_back('\0');
  identity += channel;
  return slotKey(identity);
}

// Parse a desired-sink list: [{"key":…,"name":…}, …]. `key` is optional (an entry with only a
// name is self-keying). Returns false on a parse error / non-array value.
bool parseDesiredSinks(const std::string& s,
                       std::vector<JackTransportLink::DesiredSink>& out) {
  try {
    auto j = nlohmann::json::parse(s);
    if (!j.is_array()) return false;
    out.clear();
    for (const auto& e : j) {
      if (!e.is_object()) continue;
      JackTransportLink::DesiredSink d;
      d.key  = e.value("key",  std::string());
      d.name = e.value("name", std::string());
      out.push_back(std::move(d));
    }
    return true;
  } catch (...) {
    return false;
  }
}

// Parse a desired-source list: [{"key":…,"peer":…,"channel":…}, …].
bool parseDesiredSources(const std::string& s,
                         std::vector<JackTransportLink::DesiredSource>& out) {
  try {
    auto j = nlohmann::json::parse(s);
    if (!j.is_array()) return false;
    out.clear();
    for (const auto& e : j) {
      if (!e.is_object()) continue;
      JackTransportLink::DesiredSource d;
      d.key     = e.value("key",     std::string());
      d.peer    = e.value("peer",    std::string());
      d.channel = e.value("channel", std::string());
      out.push_back(std::move(d));
    }
    return true;
  } catch (...) {
    return false;
  }
}

} // namespace

JackTransportLink::JackTransportLink(jack_client_t *client,
                                     bool enableStartStopSync,
                                     double initialBPM, double initialQuantum,
                                     float initialTimeSigDenom,
                                     double initialTicksPerBeat,
                                     bool enableLinkAudio,
                                     bool syncLink,
                                     std::string configPath,
                                     std::vector<std::string> sinkNames,
                                     std::vector<std::pair<std::string, std::string>> sources,
                                     std::string linkPeerName,
                                     double captureLatencyTrimMs,
                                     double playbackLatencyTrimMs,
                                     double latencyMs,
                                     bool syncToIncomingAudio,
                                     bool linkEnabled)
    : mJackClient(client),
      mSampleRate(static_cast<double>(jack_get_sample_rate(client))),
      mLinkPeerName(std::move(linkPeerName)),
      mBPM(initialBPM), mLinkBPM(initialBPM), mQuantum(initialQuantum),
      mInitialQuantum(initialQuantum),
      mInitialTimeSigDenom(initialTimeSigDenom),
      mInitialTicksPerBeat(initialTicksPerBeat),
      // Link peer name is decoupled from the JACK client name (which stays
      // "jack-transport-link" for the runner/port-bridge); default to the hostname.
      mLink(initialBPM, effectiveLinkPeerName()),
      mJackClientUUID(0),
      mSyncLink(syncLink),
      mWasSyncLink(syncLink),
      mConfigPath(std::move(configPath)) {
  // setup listener

  mCaptureLatencyTrimMs.store(captureLatencyTrimMs, std::memory_order_release);
  mPlaybackLatencyTrimMs.store(playbackLatencyTrimMs, std::memory_order_release);
  mLatencyMs.store(clampLatencyMs(latencyMs), std::memory_order_release);
  mSyncToIncomingAudio.store(syncToIncomingAudio, std::memory_order_release);
  mLinkEnabledDesired.store(linkEnabled, std::memory_order_release);

  // setup link
  mLink.setTempoCallback([this](double bpm) {
    mLinkBPM.store(bpm, std::memory_order_release);
    if (mSyncLink.load(std::memory_order_acquire)) {
      mBPM.store(bpm, std::memory_order_release);
      mReportBPM.store(true, std::memory_order_release);
    }
  });
  if (enableStartStopSync) {
    mLink.setStartStopCallback([this](bool isPlaying) {
      if (mLink.isStartStopSyncEnabled() &&
          mSyncLink.load(std::memory_order_acquire)) {
        if (isPlaying) {
          jack_transport_start(mJackClient);
        } else {
          jack_transport_stop(mJackClient);
        }
      }
    });
  }
  mLink.enableStartStopSync(enableStartStopSync);
  mLink.setNumPeersCallback(
      [this](std::size_t numPeers) { setNumPeersProperty(numPeers); });
  // Master Link on/off: when disabled, we never join the session, so peers don't see us.
  mLink.enable(mLinkEnabledDesired.load(std::memory_order_acquire));

  // intialize our properties
  {
    // try to get our uuid, if we can get it, we set the property and property
    // callback
    char *uuids;
    if ((uuids = jack_get_uuid_for_client_name(
             mJackClient, jack_get_client_name(mJackClient))) != nullptr &&
        jack_uuid_parse(uuids, &mJackClientUUID) == 0) {
      setBPMProperty(mBPM.load(std::memory_order_acquire));
      setEnableStartStopProperty(mLink.isStartStopSyncEnabled());
      setSyncProperty(mSyncLink.load(std::memory_order_acquire));
      setLinkEnabledProperty();
      setNumPeersProperty(mLink.numPeers());
      // publish the effective Link peer name (always non-empty: override or hostname)
      mAppliedLinkPeerName = effectiveLinkPeerName();
      setLinkAudioPeerNameProperty();
      setLinkAudioLatencyMsProperty();
      setLinkAudioSyncToIncomingProperty();
      jack_set_property_change_callback(
          mJackClient, JackTransportLink::propertyChangeCallback, this);
    } else {
      std::cerr << "cannot get client uuid, property based settings won't work"
                << std::endl;
    }
    if (uuids) {
      jack_free(uuids);
    }
  }

  mMIDIClockOut =
      jack_port_register(mJackClient, "clock", JACK_DEFAULT_MIDI_TYPE,
                         JackPortFlags::JackPortIsOutput, 0);

#ifdef DO_CLICK_OUT
  mClickPort =
      jack_port_register(mJackClient, "clickout", JACK_DEFAULT_AUDIO_TYPE,
                         JackPortFlags::JackPortIsOutput, 0);
#endif

  mLinkAudioEnabled = enableLinkAudio;
  if (mLinkAudioEnabled) {
    mLink.enableLinkAudio(true);
    mLink.setChannelsChangedCallback([this]() {
      mChannelsChanged.store(true, std::memory_order_release);
    });

    // Build the initial slots from config. Keys are re-derived from the identities, which is
    // what makes the JACK port names stable across restarts. Nothing is auto-selected: a
    // device with no configured sinks/sources comes up with no Link Audio ports at all.
    std::vector<DesiredSink> desiredSinks;
    desiredSinks.reserve(sinkNames.size());
    for (auto& n : sinkNames)
      desiredSinks.push_back({std::string(), std::move(n)});
    reconcileSinks(desiredSinks);

    std::vector<DesiredSource> desiredSources;
    desiredSources.reserve(sources.size());
    for (auto& s : sources)
      desiredSources.push_back({std::string(), std::move(s.first), std::move(s.second)});
    reconcileSources(desiredSources);
  }
  if (mLinkAudioEnabled && !jack_uuid_empty(mJackClientUUID)) {
    setLinkAudioChannelsProperty({});
  }
  mUpdatePortMeta = false;
  updateAudioPortMetadata();

  // setup jack, become the timebase master, unconditionally
  jack_set_process_callback(mJackClient, JackTransportLink::processCallback,
                            this);
  jack_set_timebase_callback(mJackClient, 0,
                             JackTransportLink::timeBaseCallback, this);
  jack_set_sync_callback(mJackClient, JackTransportLink::syncCallback, this);
  // Auto-detect I/O latency: JACK invokes this (non-RT) on graph/buffer-size changes, once per
  // direction, and we read the corresponding port latency range. Registered even when Link Audio
  // is disabled (no ports -> detected latency stays 0, only the user trim applies).
  jack_set_latency_callback(mJackClient, JackTransportLink::latencyCallback, this);
  jack_activate(mJackClient);
  mJackActivated = true;
  // Seed effective latency + publish read-backs now (auto values arrive via the latency callback
  // once ports are connected; until then effective = user trim only).
  recomputeEffectiveLatency();
}

JackTransportLink::~JackTransportLink() {
  // flush any config change that the debounce in processEvents hasn't written yet,
  // so a clean exit (e.g. Ctrl-C) doesn't lose recent changes
  {
    std::lock_guard<std::recursive_mutex> lock(mControlMutex);
    if (mNeedsSaveConfig.load(std::memory_order_acquire))
      saveConfig();
  }
  jack_set_sync_callback(mJackClient, nullptr, nullptr);
  jack_release_timebase(mJackClient);
  jack_deactivate(mJackClient);
  jack_client_close(mJackClient);
}

void JackTransportLink::latencyCallback(jack_latency_callback_mode_t, void *arg) {
  // Runs on JACK's notification thread. Reading the port latency ranges here would race with
  // reconcileSinks/Sources mutating the slot vectors on the main thread (and jack_set_property is
  // illegal from this thread), so just flag it; processEvents does the read + republish.
  static_cast<JackTransportLink *>(arg)->mNeedsRecomputeLatency.store(
      true, std::memory_order_release);
}

void JackTransportLink::updateLatencyRanges() {
  // Main-thread (serialized with reconcileSinks/Sources). We are a terminal client and add no
  // latency of our own, so we don't declare any; we only read the systemic latency JACK has
  // computed for our ports. Capture latency of a sink port = how long ago its audio was captured
  // (send offset); playback latency of a source port = how long until its audio is heard.
  jack_latency_range_t range;
  auto accum = [&range](jack_port_t* p, jack_latency_callback_mode_t m,
                        jack_nframes_t& maxFrames) {
    if (!p) return;
    jack_port_get_latency_range(p, m, &range);
    maxFrames = std::max(maxFrames, range.max);
  };
  jack_nframes_t capture = 0, playback = 0;
  for (const auto& s : mSinks) {
    accum(s.portL, JackCaptureLatency, capture);
    accum(s.portR, JackCaptureLatency, capture);
  }
  for (const auto& s : mSources) {
    accum(s.portL, JackPlaybackLatency, playback);
    accum(s.portR, JackPlaybackLatency, playback);
  }
  mAutoCaptureLatencyFrames.store(capture, std::memory_order_release);
  mAutoPlaybackLatencyFrames.store(playback, std::memory_order_release);
  recomputeEffectiveLatency();
}

void JackTransportLink::recomputeEffectiveLatency() {
  const double sr = mSampleRate > 0.0 ? mSampleRate : 48000.0;
  auto effFrames = [sr](jack_nframes_t autoFrames, double trimMs) -> jack_nframes_t {
    const double frames = static_cast<double>(autoFrames) + trimMs * sr / 1000.0;
    return frames > 0.0 ? static_cast<jack_nframes_t>(std::llround(frames)) : 0;
  };
  mEffCaptureLatencyFrames.store(
      effFrames(mAutoCaptureLatencyFrames.load(std::memory_order_acquire),
                mCaptureLatencyTrimMs.load(std::memory_order_acquire)),
      std::memory_order_release);
  mEffPlaybackLatencyFrames.store(
      effFrames(mAutoPlaybackLatencyFrames.load(std::memory_order_acquire),
                mPlaybackLatencyTrimMs.load(std::memory_order_acquire)),
      std::memory_order_release);
  // All callers (constructor + processEvents) are on the main thread, so publishing the read-only
  // metadata directly here is safe (jack_set_property must not run on the notification thread).
  setLinkAudioLatencyProperties();
}

void JackTransportLink::setLinkAudioLatencyProperties() {
  if (jack_uuid_empty(mJackClientUUID)) return;
  const double sr = mSampleRate > 0.0 ? mSampleRate : 48000.0;
  auto ms = [sr](jack_nframes_t frames) {
    return std::to_string(1000.0 * static_cast<double>(frames) / sr);
  };
  jack_set_property(mJackClient, mJackClientUUID, linkaudio_capture_latency_key.c_str(),
                    ms(mEffCaptureLatencyFrames.load(std::memory_order_acquire)).c_str(),
                    decimal_type);
  jack_set_property(mJackClient, mJackClientUUID, linkaudio_playback_latency_key.c_str(),
                    ms(mEffPlaybackLatencyFrames.load(std::memory_order_acquire)).c_str(),
                    decimal_type);
  jack_set_property(mJackClient, mJackClientUUID, linkaudio_capture_latency_auto_key.c_str(),
                    ms(mAutoCaptureLatencyFrames.load(std::memory_order_acquire)).c_str(),
                    decimal_type);
  jack_set_property(mJackClient, mJackClientUUID, linkaudio_playback_latency_auto_key.c_str(),
                    ms(mAutoPlaybackLatencyFrames.load(std::memory_order_acquire)).c_str(),
                    decimal_type);
}

void JackTransportLink::processEvents() {
  std::lock_guard<std::recursive_mutex> lock(mControlMutex);
  if (mLinkAudioEnabled) {
    if (mNeedsReconcileSinks.exchange(false, std::memory_order_acq_rel)) {
      reconcileSinks(mDesiredSinks);
    }
    if (mNeedsReconcileSources.exchange(false, std::memory_order_acq_rel)) {
      reconcileSources(mDesiredSources);
    }
    if (mNeedsResetDropouts.exchange(false, std::memory_order_acq_rel)) {
      resetSourceDropouts(mResetDropoutsTarget);
      // one-shot command: clear it so the same value can be sent again
      removePropertyIfExists(linkaudio_reset_dropouts_key);
    }
    if (mChannelsChanged.exchange(false, std::memory_order_acq_rel)) {
      if (updateLinkAudioSource()) { mReportLinkAudioSource = true; mUpdatePortMeta = true; }
      mReportLinkAudioChannels = true;
    }
    if (mNeedsSourceUpdate.exchange(false, std::memory_order_acq_rel)) {
      if (updateLinkAudioSource()) { mReportLinkAudioSource = true; mUpdatePortMeta = true; }
    }
    if (mReportLinkAudioChannels.exchange(false, std::memory_order_acq_rel)) {
      setLinkAudioChannelsProperty(mLink.channels());
    }
    if (mReportLinkAudioSource.exchange(false, std::memory_order_acq_rel)) {
      // A connect/disconnect should show up at once rather than waiting out the telemetry
      // timer, so publish now and restart the throttle.
      mLastHealthPublish = std::chrono::steady_clock::now();
      setLinkAudioSourceStatusProperty();
    }
    if (mUpdatePortMeta) {
      mUpdatePortMeta = false;
      updateAudioPortMetadata();
    }
    // Publish live receive telemetry on a timer (values change every audio block; a client only
    // needs a few updates/second). Runs on the main thread, so jack_set_property is legal here.
    if (!mSources.empty()) {
      auto now = std::chrono::steady_clock::now();
      if (now - mLastHealthPublish >= std::chrono::milliseconds(250)) {
        mLastHealthPublish = now;
        setLinkAudioSourceStatusProperty();
      }
    }
  }
  // Link peer name applies regardless of Link Audio: Link itself is always enabled.
  if (mNeedsApplyPeerName.exchange(false, std::memory_order_acq_rel)) {
    applyLinkPeerName();
  }
  if (mNeedsRecomputeLatency.exchange(false, std::memory_order_acq_rel)) {
    updateLatencyRanges();
  }
  if (mNeedsPublishLatencyMs.exchange(false, std::memory_order_acq_rel)) {
    setLinkAudioLatencyMsProperty();
  }
  if (mNeedsPublishSyncToIncoming.exchange(false, std::memory_order_acq_rel)) {
    setLinkAudioSyncToIncomingProperty();
  }
  if (mNeedsApplyLinkEnabled.exchange(false, std::memory_order_acq_rel)) {
    // mLink.enable() is not RT-safe; applying here on the main thread is correct.
    mLink.enable(mLinkEnabledDesired.load(std::memory_order_acquire));
  }
  if (mNeedsPublishLinkEnabled.exchange(false, std::memory_order_acq_rel)) {
    setLinkEnabledProperty();
  }
  if (mReportBPM.exchange(false, std::memory_order_acq_rel)) {
    setBPMProperty(mBPM.load(std::memory_order_acquire));
  }
  if (mReportLinkSync.exchange(false, std::memory_order_acq_rel)) {
    setSyncProperty(mSyncLink.load(std::memory_order_acquire));
  }
  if (mReportStartStopEnable.exchange(false, std::memory_order_acq_rel)) {
    setEnableStartStopProperty(mLink.isStartStopSyncEnabled());
  }
  if (mNeedsSaveConfig.load(std::memory_order_acquire)) {
    auto now = std::chrono::steady_clock::now();
    if (now - mLastConfigSave >= std::chrono::seconds(1)) {
      mNeedsSaveConfig.store(false, std::memory_order_release);
      mLastConfigSave  = now;
      saveConfig();
    }
  }
}

int JackTransportLink::processCallback(jack_nframes_t nframes, void *arg) {
  return reinterpret_cast<JackTransportLink *>(arg)->processCallback(nframes);
}

void updateBBT(int32_t &bar, int32_t &beat, double &tick, double ticks_per_beat,
               int beats_per_bar) {
  if (tick >= ticks_per_beat) {
    beat += 1;
    tick = std::fmod(tick, ticks_per_beat);
    if (beat >= beats_per_bar) {
      beat = beat % beats_per_bar;
      bar += 1;
    }
  }
}

int JackTransportLink::processCallback(jack_nframes_t nframes) {
  // compute the time, the timeBaseCallback is called right after this
  // processCallback
  {
    jack_nframes_t frameTime;
    jack_time_t cur, next;
    float period;
    if (jack_get_cycle_times(mJackClient, &frameTime, &cur, &next, &period) ==
        0) {
      mTime = std::chrono::microseconds(cur);
      mTimeNext = std::chrono::microseconds(next);
    } else {
      // report?
    }
  }

  jack_position_t pos;

  double beatrequest = -1.0;

  // if sync has changed, and we are now syncing, we request the beat we're at
  const bool syncLink = mSyncLink.load(std::memory_order_acquire);
  if (syncLink != mWasSyncLink) {
    mWasSyncLink = syncLink;
    if (syncLink) {
      beatrequest = mInternalBeat; // might already be equal from above
    }
  }

  // TODO in follower mode, always report transport state changes,
  // also, mBPM won't contain a valid BPM for the transport

  // when the session state is stopped, timeBaseCallback isn't called, so we
  // report start/stop in the processCallback
  auto transportState = jack_transport_query(mJackClient, &pos);
  bool bbtValid = pos.valid & JackPositionBBT;
  // always considered "playing" if it isn't stopped
  auto rolling = transportState != jack_transport_state_t::JackTransportStopped;
  bool stateChange = transportState != mTransportStateReportedLast;
  double bpm = mBPM.load(std::memory_order_acquire);
  bool bpmChange = bbtValid && pos.beats_per_minute != bpm;
  auto linkTime = mTimeNext; // now plus some latency
  if (syncLink && (stateChange || bpmChange || beatrequest >= 0.0)) {
    bool havePeers = mLink.numPeers() > 0;
    auto sessionState = mLink.captureAudioSessionState();
    if (stateChange) {
      sessionState.setIsPlaying(rolling, linkTime);
      // request beat while starting (or if we missed staring somehow)
      if (transportState == jack_transport_state_t::JackTransportStarting ||
          (transportState == jack_transport_state_t::JackTransportRolling &&
           mTransportStateReportedLast !=
               jack_transport_state_t::JackTransportStarting)) {
        if (havePeers) {
          sessionState.requestBeatAtStartPlayingTime(mInternalBeat, mQuantum);
        } else {
          sessionState.forceBeatAtTime(mInternalBeat, linkTime, mQuantum);
          beatrequest = -1.0;
        }
      }
      mTransportStateReportedLast = transportState;
    }
    if (bpmChange) {
      sessionState.setTempo(bpm, linkTime);
    }

    if (beatrequest >= 0) {
      if (havePeers) {
        sessionState.requestBeatAtTime(mInternalBeat, linkTime, mQuantum);
      } else {
        sessionState.forceBeatAtTime(mInternalBeat, linkTime, mQuantum);
      }
    }
    mLink.commitAudioSessionState(sessionState);
  }

  if (beatrequest >= 0.0) {
    mMIDIClockRunState = MIDIClockRunState::NeedsSync;
    invalidateClockSyncBBT();
  }

#ifndef DO_CLICK_OUT
  // write midi sync
  auto midi_buf = jack_port_get_buffer(mMIDIClockOut, nframes);
  jack_midi_clear_buffer(midi_buf);

  if (bbtValid) {
    if (rolling) {
      const double clocksPerBeat = MIDI_PPQ;
      const double sr = static_cast<double>(jack_get_sample_rate(mJackClient));

      int32_t beat = pos.beat - 1;
      int32_t bar = pos.bar - 1;
      double tick = static_cast<double>(pos.tick);

      double framesPerTick =
          60.0 * sr / (pos.ticks_per_beat * pos.beats_per_minute);
      double ticksPerClock = pos.ticks_per_beat / clocksPerBeat;
      double framesPerClock = framesPerTick * ticksPerClock;

      // offset from buffer tick start to the tick where we should issue the
      // first clock
      double offsetTicks = std::fmod(tick, ticksPerClock);
      offsetTicks = offsetTicks <= 0.0 ? 0.0 : (ticksPerClock - offsetTicks);

      // update the tick
      tick = tick + offsetTicks;
      const int beatsPerBar = static_cast<int>(pos.beats_per_bar);
      updateBBT(bar, beat, tick, pos.ticks_per_beat, beatsPerBar);
      double nextClockFrame = offsetTicks * framesPerTick;

      // skip dupes
      if (mBarLast == bar && mBeatLast == beat &&
          std::abs(mTickLast - tick) < 0.5) {
        // std::cout << "dupe found: " << bar << ":" << beat << ":" << tick <<
        // std::endl;
        tick += ticksPerClock;
        nextClockFrame += ticksPerClock * framesPerTick;
        updateBBT(bar, beat, tick, pos.ticks_per_beat, beatsPerBar);
      }

      if (mMIDIClockRunState == MIDIClockRunState::NeedsSync) {
        jack_midi_event_write(midi_buf, 0, midi_stop_buf.data(),
                              midi_stop_buf.size());
        mMIDIClockRunState = MIDIClockRunState::Stopped;
      }

      double frame = nextClockFrame;
      while (floor(frame + mClockFrameDelay) < static_cast<double>(nframes)) {
        if (mMIDIClockRunState == MIDIClockRunState::Running) {
          jack_nframes_t f =
              static_cast<jack_nframes_t>(frame + mClockFrameDelay);
          mClockFrameDelay = 0;

          // verify that we're keeping in sync with 24 clocks per quarter note
          bool resync = false;
          if (mMIDIClockCount == 0) {
            resync = tick >= ticksPerClock;
          } else if (tick < ticksPerClock) {
            resync = true;
          }

          if (resync) {
            // std::cout << "clock out of sync? bar: " << bar << " beat: " <<
            // beat << " tick: " << tick << " clock count: " << mMIDIClockCount
            // << std::endl; std::cout << "\tframes per clock: " <<
            // framesPerClock << " ticks per clock:" <<  ticksPerClock << "
            // frames per tick: " << framesPerTick << std::endl;
            // TODO could we be smarter and simply issue some extra or skip some
            // clocks?
            mMIDIClockRunState = MIDIClockRunState::NeedsSync;
            jack_midi_event_write(midi_buf, f, midi_stop_buf.data(),
                                  midi_stop_buf.size());
            break;
          }

#ifdef MIDI_SEND_REPEATED_STARTS
          if (beat == 0 && mMIDIClockCount == 0) {
            jack_midi_event_write(midi_buf, frame, midi_start_buf.data(),
                                  midi_start_buf.size());
          }
#endif

          jack_midi_event_write(midi_buf, f, midi_clock_buf.data(),
                                midi_clock_buf.size());
          mMIDIClockCount = (mMIDIClockCount + 1) % MIDI_PPQ;
        } else if (beat == 0 && tick < ticksPerClock && tick >= 0 && bar >= 0) {
          // see if we need to send a start
          mMIDIClockRunState = MIDIClockRunState::Running;
#ifndef MIDI_SEND_REPEATED_STARTS
          jack_midi_event_write(midi_buf, static_cast<jack_nframes_t>(frame),
                                midi_start_buf.data(), midi_start_buf.size());
#endif

          // std::cout << "start " << frame << " tick: " << tick << std::endl;

          // delay clock 1ms or half a clock period
          // http://midi.teragonaudio.com/tech/midispec.htm
          mClockFrameDelay = std::min(framesPerClock / 2.0, sr / 1000.0);
          mMIDIClockCount = 0;
          continue; // restart loop
        }

        mTickLast = tick;
        mBeatLast = beat;
        mBarLast = bar;

        tick += ticksPerClock;
        frame = frame + framesPerClock;
        updateBBT(bar, beat, tick, pos.ticks_per_beat, beatsPerBar);
      }
    } else if (transportState == JackTransportStopped &&
               mMIDIClockRunState != MIDIClockRunState::Stopped) {
      mClockFrameDelay = 0;
      mMIDIClockRunState = MIDIClockRunState::Stopped;
      jack_midi_event_write(midi_buf, 0, midi_stop_buf.data(),
                            midi_stop_buf.size());
      invalidateClockSyncBBT();
    }
  }
#else
  if (mClickPort != nullptr) {
    jack_default_audio_sample_t *buf =
        reinterpret_cast<jack_default_audio_sample_t *>(
            jack_port_get_buffer(mClickPort, nframes));

    // zero out
    std::memset(buf, 0, nframes * sizeof(jack_default_audio_sample_t));
    if (bbtValid && rolling) {

      const double clicksPerBeat = 4;
      const double sr = static_cast<double>(jack_get_sample_rate(mJackClient));

      int32_t beat = pos.beat - 1;
      int32_t bar = pos.bar - 1;
      double tick = static_cast<double>(pos.tick);

      double framesPerTick =
          60.0 * sr / (pos.ticks_per_beat * pos.beats_per_minute);
      double ticksPerClick = pos.ticks_per_beat / clicksPerBeat;
      double framesPerClick = framesPerTick * ticksPerClick;

      const int beatsPerBar = static_cast<int>(pos.beats_per_bar);
      double offsetTicks = std::fmod(tick, ticksPerClick);
      offsetTicks = offsetTicks <= 0.0 ? 0.0 : ticksPerClick - offsetTicks;
      tick = tick + offsetTicks;
      updateBBT(bar, beat, tick, pos.ticks_per_beat, beatsPerBar);

      // skip dupes
      if (mBarLast == bar && mBeatLast == beat && mTickLast == tick) {
        // std::cout << bar << ":" << beat << ":" << tick << " last: " <<
        // mBarLast << ":" << mBeatLast << ":" << mTickLast << std::endl;
        tick += ticksPerClick;
        offsetTicks += ticksPerClick;
        updateBBT(bar, beat, tick, pos.ticks_per_beat, beatsPerBar);
      }

      double nextClickFrame = offsetTicks * framesPerTick;

      double frame = nextClickFrame;
      while (ceil(frame) < static_cast<double>(nframes)) {
        jack_nframes_t f = static_cast<jack_nframes_t>(frame);
        buf[f] = 1.0; // 0.5 + 0.5 *
                      // static_cast<jack_default_audio_sample_t>(beat) / 4.0;

        mTickLast = tick;
        mBeatLast = beat;
        mBarLast = bar;

        tick += ticksPerClick;
        frame = frame + framesPerClick;
        updateBBT(bar, beat, tick, pos.ticks_per_beat, beatsPerBar);
      }
    }
  }
#endif

  if (mLinkAudioEnabled && (!mSinks.empty() || !mSources.empty())) {
    auto sessionState = mLink.captureAudioSessionState();

    // I/O latency compensation (see header): audio in the in_N buffer was captured
    // capture-latency frames ago, so stamp it to that earlier beat; audio written to out_N
    // will be heard playback-latency frames from now, so target the beat for that later time.
    const auto framesToUsec = [this](jack_nframes_t frames) {
      if (frames == 0 || mSampleRate <= 0.0) return std::chrono::microseconds(0);
      return std::chrono::microseconds(
          std::llround(1.0e6 * static_cast<double>(frames) / mSampleRate));
    };
    const auto sendHostTime =
        mTimeNext - framesToUsec(mEffCaptureLatencyFrames.load(std::memory_order_acquire));
    const auto recvHostTime =
        mTimeNext + framesToUsec(mEffPlaybackLatencyFrames.load(std::memory_order_acquire));

    for (auto& sink : mSinks) {
      if (!sink.renderer || !sink.portL || !sink.portR) continue;
      const float* sendPtrs[2] = {
          static_cast<const float *>(jack_port_get_buffer(sink.portL, nframes)),
          static_cast<const float *>(jack_port_get_buffer(sink.portR, nframes))};
      sink.renderer->send(sendPtrs, nframes, sessionState,
                          mSampleRate, sendHostTime, mQuantum);
    }

    for (auto& source : mSources) {
      if (!source.renderer || !source.portL || !source.portR) continue;
      float* recvPtrs[2] = {
          static_cast<float *>(jack_port_get_buffer(source.portL, nframes)),
          static_cast<float *>(jack_port_get_buffer(source.portR, nframes))};
      // The streaming playout buffer always applies to received audio — network buffers arrive
      // late, so without it there is nothing to play (the live-beat target drops every buffer as
      // too old). "Sync to Incoming Audio" does NOT gate this; it only decides whether the local
      // transport timeline is delayed to match (see timeBaseCallback). Off => incoming still
      // plays buffered, just not phase-aligned with transport-locked local generators.
      source.renderer->receive(recvPtrs, nframes, sessionState,
                               mSampleRate, recvHostTime, mQuantum,
                               mLatencyMs.load(std::memory_order_acquire));
    }
  }

  return 0;
}

void JackTransportLink::timeBaseCallback(jack_transport_state_t state,
                                         jack_nframes_t nframes,
                                         jack_position_t *pos, int new_pos,
                                         void *arg) {
  reinterpret_cast<JackTransportLink *>(arg)->timeBaseCallback(state, nframes,
                                                               pos, new_pos);
}

// timebase callback, only called while transport is running or starting
void JackTransportLink::timeBaseCallback(jack_transport_state_t transportState,
                                         jack_nframes_t nframes,
                                         jack_position_t *pos, bool posIsNew) {
  auto sessionState = mLink.captureAudioSessionState();
  bool bbtValid = pos->valid & JackPositionBBT;

  double bpm = mBPM.load(std::memory_order_acquire);
  mQuantum = bbtValid ? pos->beats_per_bar : mInitialQuantum;
  double ticksPerBeat = bbtValid ? pos->ticks_per_beat : mInitialTicksPerBeat;

  auto linkTime = mTime;
  const bool sync = mSyncLink.load(std::memory_order_acquire);

  if (sync) {
    mInternalBeat = sessionState.beatAtTime(linkTime, mQuantum);
  }

  if (posIsNew) {
    /*
     *  copied from transport.c -- JACK transport master example client.
     *
     *  Copyright (C) 2003 Jack O'Quin.
     */

    // beat/tick etc after a seek are simply based on frame and the current bpm
    double min = pos->frame / ((double)pos->frame_rate * 60.0);
    double abs_tick = min * pos->beats_per_minute * pos->ticks_per_beat;
    double abs_beat = abs_tick / pos->ticks_per_beat;

    mInternalBeat = abs_beat;

    if (sync) {
      if (mLink.numPeers() > 0) {
        sessionState.requestBeatAtTime(mInternalBeat, linkTime, mQuantum);
      } else {
        sessionState.forceBeatAtTime(mInternalBeat, linkTime, mQuantum);
      }
      mLink.commitAudioSessionState(sessionState);
      mInternalBeat = sessionState.beatAtTime(linkTime, mQuantum);
    }

    // need to sync again since we repositioned
    mMIDIClockRunState = MIDIClockRunState::NeedsSync;
    invalidateClockSyncBBT();
  }

  // "Sync to Incoming Audio": the receiver plays the incoming stream kLatencyInBeats behind the
  // live beat (see the receive path). Shift the transport position we report by the same amount
  // so JACK-transport consumers (the RNBO engine's phasor, MIDI clock, click) run on that same
  // delayed timeline and stay phase-aligned with the received audio. The Link Audio send/receive
  // paths query the Link session beat directly, not this BBT, so they are unaffected (external
  // sources stay synced; sent audio stays globally correct). Uses the reported bpm for exact
  // consistency with the pos we publish; mInternalBeat itself is left intact for Link sync.
  //
  // Only shift when we actually have incoming audio to sync to (Link Audio enabled with >=1
  // source). Otherwise (send-only or Link Audio off) there is nothing to monitor against, and
  // delaying the transport would only misalign a transport-locked generator feeding a sink (its
  // content would lag while the send path still stamps the live Link beat).
  double reportedBeat = mInternalBeat;
  if (mSyncToIncomingAudio.load(std::memory_order_acquire) && bpm > 0.0
      && mLinkAudioEnabled && !mSources.empty()) {
    reportedBeat -= (mLatencyMs.load(std::memory_order_acquire) / 1000.0) * (bpm / 60.0);
  }

  // what if quantum changes? Does link keep track of that or should we compute
  // bar some other way?
  auto bar = std::floor(reportedBeat / mQuantum);
  auto beat = std::fmod(reportedBeat, mQuantum);
  if (beat < 0.0) beat += mQuantum; // keep phase/tick positive through the start-up transient
  auto tick = trunc(ticksPerBeat * (beat - trunc(beat)));
  float beatType = bbtValid ? pos->beat_type : mInitialTimeSigDenom;

  pos->valid = JackPositionBBT;
  pos->bar = static_cast<int32_t>(bar) + 1;
  pos->beat = static_cast<int32_t>(beat) + 1;
  pos->tick = static_cast<int32_t>(tick);
  pos->bar_start_tick = bar * mQuantum * ticksPerBeat;
  pos->beats_per_bar = static_cast<float>(mQuantum);
  pos->beat_type = beatType;
  pos->ticks_per_beat = ticksPerBeat;
  pos->beats_per_minute = bpm;

  if (!sync && transportState == jack_transport_state_t::JackTransportRolling) {
    mInternalBeat +=
        bpm * static_cast<double>(nframes) /
        (static_cast<double>(jack_get_sample_rate(mJackClient)) * 60.0);
  }
}

int JackTransportLink::syncCallback(jack_transport_state_t state,
                                    jack_position_t *pos, void *arg) {
  return reinterpret_cast<JackTransportLink *>(arg)->syncCallback(state, pos);
}

int JackTransportLink::syncCallback(jack_transport_state_t /*transportState*/,
                                    jack_position_t * /*pos*/) {
  // TODO delay start to sync with time from session?
  return 1;
}

void JackTransportLink::propertyChangeCallback(jack_uuid_t subject,
                                               const char *key,
                                               jack_property_change_t change,
                                               void *arg) {
  return reinterpret_cast<JackTransportLink *>(arg)->propertyChangeCallback(
      subject, key, change);
}

void JackTransportLink::propertyChangeCallback(jack_uuid_t subject,
                                               const char *key,
                                               jack_property_change_t change) {
  std::lock_guard<std::recursive_mutex> lock(mControlMutex);
  // if the subject is all or us and the key is all (empty) or bpm
  if ((jack_uuid_empty(subject) || subject == mJackClientUUID)) {
    bool isbpm = !key || bpm_key.compare(key) == 0;
    bool islinksync = !key || linksync_key.compare(key) == 0;
    bool isenable = !key || start_stop_key.compare(key) == 0;
    bool ispeername    = !key || linkaudio_peer_name_key.compare(key) == 0;
    bool islatency     = !key || linkaudio_latency_key.compare(key) == 0;
    bool issync        = !key || linkaudio_sync_key.compare(key) == 0;
    bool islinkenabled = !key || link_enabled_key.compare(key) == 0;
    bool issinks       = !key || linkaudio_sinks_key.compare(key) == 0;
    bool issources     = !key || linkaudio_sources_key.compare(key) == 0;
    bool isresetdrops  = key && linkaudio_reset_dropouts_key.compare(key) == 0;
    // Treat a newly-created property the same as a changed one. Properties that
    // jack_transport_link doesn't publish itself arrive as PropertyCreated, not
    // PropertyChanged.
    if (change == jack_property_change_t::PropertyChanged ||
        change == jack_property_change_t::PropertyCreated) {
      std::string values;
      std::string types;
      if (isbpm && get_property(mJackClientUUID, bpm_key, values, types)) {
        // convert to double and store if success
        char *pEnd = nullptr;
        double bpm = std::strtod(values.c_str(), &pEnd);
        if (*pEnd == 0)
          mBPM.store(bpm, std::memory_order_release);
      } else if (isenable &&
                 get_property(mJackClientUUID, start_stop_key, values, types)) {
        bool set = std::find(true_values.begin(), true_values.end(), values) !=
                   true_values.end();
        mLink.enableStartStopSync(set);
        mNeedsSaveConfig = true;
      } else if (islinksync &&
                 get_property(mJackClientUUID, linksync_key, values, types)) {

        const bool was = mSyncLink.load(std::memory_order_acquire);
        const bool sync = std::find(true_values.begin(), true_values.end(), values) !=
                          true_values.end();
        mSyncLink.store(sync, std::memory_order_release);

        if (sync && !was) {
          mBPM.store(mLinkBPM.load(std::memory_order_acquire),
                     std::memory_order_release);
          mReportBPM = true;
        }
        mNeedsSaveConfig = true;
      } else if (islinkenabled &&
                 get_property(mJackClientUUID, link_enabled_key, values, types)) {
        const bool set = std::find(true_values.begin(), true_values.end(), values) !=
                         true_values.end();
        if (set != mLinkEnabledDesired.load(std::memory_order_acquire)) {
          mLinkEnabledDesired.store(set, std::memory_order_release);
          mNeedsApplyLinkEnabled.store(true, std::memory_order_release);
          mNeedsSaveConfig = true;
        }
        // Correct any non-canonical write (e.g. "1"/"0") back to "true"/"false"; our own echo
        // matches and is skipped.
        if (values != std::string(set ? "true" : "false")) {
          mNeedsPublishLinkEnabled.store(true, std::memory_order_release);
        }
      } else if (ispeername &&
                 get_property(mJackClientUUID, linkaudio_peer_name_key, values, types)) {
        // The published value is the *effective* name (override or hostname), so a
        // client can display what's broadcast. Only treat a write as a new override
        // when it differs from the current effective name — that filters out our own
        // republished value (and a redundant write of the current hostname), which
        // would otherwise latch the hostname in as an override and break auto mode.
        if (values != effectiveLinkPeerName()) {
          mLinkPeerName = values;
          mNeedsApplyPeerName.store(true, std::memory_order_release);
          mNeedsSaveConfig = true;
        }
      } else if (islatency &&
                 get_property(mJackClientUUID, linkaudio_latency_key, values, types)) {
        double clamped;
        try {
          clamped = clampLatencyMs(std::stod(values));
        } catch (...) {
          // unparseable write -> keep the current value, but correct the metadata below
          clamped = mLatencyMs.load(std::memory_order_acquire);
        }
        if (clamped != mLatencyMs.load(std::memory_order_acquire)) {
          mLatencyMs.store(clamped, std::memory_order_release);
          mNeedsSaveConfig = true;
        }
        // Republish unless the metadata already holds our canonical form. This both filters our
        // own echo (values == canonical -> skip, no loop) and corrects any out-of-range / NaN /
        // unparseable / differently-formatted client write back to the applied value, so the
        // read-back stays consistent even when the sanitized value equals the current one.
        if (values != std::to_string(clamped)) {
          mNeedsPublishLatencyMs.store(true, std::memory_order_release);
        }
      } else if (issync &&
                 get_property(mJackClientUUID, linkaudio_sync_key, values, types)) {
        const bool set = std::find(true_values.begin(), true_values.end(), values) !=
                         true_values.end();
        if (set != mSyncToIncomingAudio.load(std::memory_order_acquire)) {
          mSyncToIncomingAudio.store(set, std::memory_order_release);
          mNeedsSaveConfig = true;
        }
        // Republish our canonical form unless the metadata already matches it (filters our own
        // echo; corrects any non-canonical write, e.g. "1"/"0", back to "true"/"false").
        if (values != std::string(set ? "true" : "false")) {
          mNeedsPublishSyncToIncoming.store(true, std::memory_order_release);
        }
      } else if (mLinkAudioEnabled && issinks &&
                 get_property(mJackClientUUID, linkaudio_sinks_key, values, types)) {
        // Skip our own echo of the canonical value; anything else is a client's desired list.
        // Unparseable JSON still triggers a reconcile of the unchanged list, whose republish
        // corrects the property back to the canonical value.
        if (values != mPublishedSinksJson) {
          std::vector<DesiredSink> desired;
          if (parseDesiredSinks(values, desired)) {
            mDesiredSinks = std::move(desired);
          } else {
            std::cerr << "warning: unparseable linkaudio/sinks write, ignoring\n";
            pendingSinks(); // reconcile the unchanged list so the republish corrects it
          }
          mNeedsReconcileSinks.store(true, std::memory_order_release);
        }
      } else if (mLinkAudioEnabled && isresetdrops &&
                 get_property(mJackClientUUID, linkaudio_reset_dropouts_key, values, types)) {
        mResetDropoutsTarget = values;
        mNeedsResetDropouts.store(true, std::memory_order_release);
      } else if (mLinkAudioEnabled && issources &&
                 get_property(mJackClientUUID, linkaudio_sources_key, values, types)) {
        if (values != mPublishedSourcesJson) {
          std::vector<DesiredSource> desired;
          if (parseDesiredSources(values, desired)) {
            mDesiredSources = std::move(desired);
          } else {
            std::cerr << "warning: unparseable linkaudio/sources write, ignoring\n";
            pendingSources();
          }
          mNeedsReconcileSources.store(true, std::memory_order_release);
        }
      }
    } else if (change == jack_property_change_t::PropertyDeleted) {
      if (isbpm)
        mReportBPM = true;
      if (isenable)
        mReportStartStopEnable = true;
      if (islinksync)
        mReportLinkSync = true;
      if (islinkenabled) {
        // clearing reverts to the default (enabled); re-apply + republish
        mLinkEnabledDesired.store(true, std::memory_order_release);
        mNeedsApplyLinkEnabled.store(true, std::memory_order_release);
        mNeedsPublishLinkEnabled.store(true, std::memory_order_release);
        mNeedsSaveConfig = true;
      }
      if (ispeername) {
        // clearing the property reverts to auto (hostname); republish the effective name
        mLinkPeerName.clear();
        mNeedsApplyPeerName.store(true, std::memory_order_release);
        mNeedsSaveConfig = true;
      }
      if (islatency) {
        // clearing reverts to the default; republish so clients reflect it
        mLatencyMs.store(100.0, std::memory_order_release);
        mNeedsPublishLatencyMs.store(true, std::memory_order_release);
        mNeedsSaveConfig = true;
      }
      if (issync) {
        // clearing reverts to the default (off); republish so clients reflect it
        mSyncToIncomingAudio.store(false, std::memory_order_release);
        mNeedsPublishSyncToIncoming.store(true, std::memory_order_release);
        mNeedsSaveConfig = true;
      }
      // Deleting a list property reverts it to its default: the empty list (all slots removed),
      // matching how the other writable keys revert on delete.
      if (mLinkAudioEnabled && issinks) {
        mDesiredSinks.clear();
        mNeedsReconcileSinks.store(true, std::memory_order_release);
      }
      if (mLinkAudioEnabled && issources) {
        mDesiredSources.clear();
        mNeedsReconcileSources.store(true, std::memory_order_release);
      }
    }
  }
}

void JackTransportLink::setBPMProperty(double bpm) {
  if (!jack_uuid_empty(mJackClientUUID)) {
    std::string bpms = std::to_string(bpm);
    jack_set_property(mJackClient, mJackClientUUID, bpm_key.c_str(),
                      bpms.c_str(), decimal_type);
  }
}

void JackTransportLink::setEnableStartStopProperty(bool enable) {
  if (!jack_uuid_empty(mJackClientUUID)) {
    std::string enables = enable ? "true" : "false";
    jack_set_property(mJackClient, mJackClientUUID, start_stop_key.c_str(),
                      enables.c_str(), bool_type);
  }
}

void JackTransportLink::setSyncProperty(bool sync) {
  if (!jack_uuid_empty(mJackClientUUID)) {
    std::string s = sync ? "true" : "false";
    jack_set_property(mJackClient, mJackClientUUID, linksync_key.c_str(),
                      s.c_str(), bool_type);
  }
}

void JackTransportLink::setLinkEnabledProperty() {
  if (jack_uuid_empty(mJackClientUUID)) return;
  const char* s = mLinkEnabledDesired.load(std::memory_order_acquire) ? "true" : "false";
  jack_set_property(mJackClient, mJackClientUUID, link_enabled_key.c_str(), s, bool_type);
}

void JackTransportLink::setNumPeersProperty(size_t peers) {
  if (!jack_uuid_empty(mJackClientUUID)) {
    std::string s = std::to_string(peers);
    jack_set_property(mJackClient, mJackClientUUID, linknumpeers_key.c_str(),
                      s.c_str(), int_type);
  }
}

void JackTransportLink::setLinkAudioChannelsProperty(
    const std::vector<ableton::LinkAudio::Channel>& channels) {
  if (jack_uuid_empty(mJackClientUUID)) return;

  // Group channels by peer name, preserving first-seen order
  nlohmann::json peers = nlohmann::json::array();
  std::vector<std::string> order;
  std::map<std::string, nlohmann::json> byPeer;
  for (const auto& ch : channels) {
    if (byPeer.find(ch.peerName) == byPeer.end()) {
      order.push_back(ch.peerName);
      byPeer[ch.peerName] = {{"peer", ch.peerName}, {"channels", nlohmann::json::array()}};
    }
    byPeer[ch.peerName]["channels"].push_back(ch.name);
  }
  for (const auto& name : order)
    peers.push_back(byPeer[name]);

  const auto value = peers.dump();
  jack_set_property(mJackClient, mJackClientUUID, linkaudio_channels_key.c_str(),
                    value.c_str(), "application/json");
}

// Publish the canonical sink list — key-tagged, in display order. Only writes when the metadata
// doesn't already hold this value: that filters our own echo (no notification loop) and, when a
// client's write was partly rejected, corrects the property back to the applied value so the
// client's read-back self-corrects. Mirrors the sanitize-then-republish-if-different pattern
// used for linkaudio/latency.
void JackTransportLink::setLinkAudioSinksProperty() {
  if (jack_uuid_empty(mJackClientUUID)) return;
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& key : mSinkOrder) {
    const size_t i = findSinkByKey(key);
    if (i == std::string::npos) continue;
    arr.push_back({{"key", mSinks[i].key}, {"name", mSinks[i].name}});
  }
  mPublishedSinksJson = arr.dump();
  std::string cur, type;
  if (!(get_property(mJackClientUUID, linkaudio_sinks_key, cur, type) &&
        cur == mPublishedSinksJson))
    jack_set_property(mJackClient, mJackClientUUID, linkaudio_sinks_key.c_str(),
                      mPublishedSinksJson.c_str(), "application/json");
}

// Publish the canonical source list — key-tagged identities, in display order. See
// setLinkAudioSinksProperty for the write-only-on-difference rationale.
void JackTransportLink::setLinkAudioSourcesProperty() {
  if (jack_uuid_empty(mJackClientUUID)) return;
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& key : mSourceOrder) {
    const size_t i = findSource(key);
    if (i == std::string::npos) continue;
    arr.push_back({{"key", mSources[i].key},
                   {"peer", mSources[i].peer},
                   {"channel", mSources[i].channel}});
  }
  mPublishedSourcesJson = arr.dump();
  std::string cur, type;
  if (!(get_property(mJackClientUUID, linkaudio_sources_key, cur, type) &&
        cur == mPublishedSourcesJson))
    jack_set_property(mJackClient, mJackClientUUID, linkaudio_sources_key.c_str(),
                      mPublishedSourcesJson.c_str(), "application/json");
}

// Per-source live receive telemetry, key-tagged and in display order. buffered() is in seconds
// (from the renderer); dropouts and jitter come from the renderer's atomics. Published on a
// timer from processEvents, and immediately on a connect/disconnect.
void JackTransportLink::setLinkAudioSourceStatusProperty() {
  if (jack_uuid_empty(mJackClientUUID)) return;
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& key : mSourceOrder) {
    const size_t i = findSource(key);
    if (i == std::string::npos) continue;
    const auto& s = mSources[i];
    arr.push_back({
        {"key",         s.key},
        {"connected",   s.currentId.has_value()},
        // connected && !receiving = subscribed but producing pure silence; see the renderer's
        // receiving() note for why the dropout count can't report that case
        {"receiving",   s.renderer && s.renderer->receiving()},
        {"buffered_ms", s.renderer ? 1000.0 * s.renderer->buffered() : 0.0},
        {"dropouts",    s.renderer ? s.renderer->dropoutCount() : 0u},
        // nonzero = audio is arriving but stamped in a different Link session, so it can't be
        // placed on our beat timeline and is discarded. No latency value helps.
        {"unmappable",  s.renderer ? s.renderer->unmappableCount() : 0u},
        // measured: how far behind the live beat the newest arrived audio begins. The playout
        // buffer has to exceed this, so it is what a too-small `latency` should be compared to.
        {"arrival_offset_ms", s.renderer ? s.renderer->arrivalOffsetMs() : 0.0f},
        {"jitter_ms",   s.renderer ? s.renderer->jitterMs() : 0.0f},
    });
  }
  jack_set_property(mJackClient, mJackClientUUID, linkaudio_source_status_key.c_str(),
                    arr.dump().c_str(), "application/json");
}

size_t JackTransportLink::findSinkByKey(const std::string& key) const {
  for (size_t i = 0; i < mSinks.size(); ++i)
    if (mSinks[i].key == key) return i;
  return std::string::npos;
}

// Name first, then key: the OSC commands take a human-typed identifier, and a name is what a
// user actually has to hand.
size_t JackTransportLink::findSink(const std::string& nameOrKey) const {
  for (size_t i = 0; i < mSinks.size(); ++i)
    if (mSinks[i].name == nameOrKey) return i;
  return findSinkByKey(nameOrKey);
}

size_t JackTransportLink::findSource(const std::string& key) const {
  for (size_t i = 0; i < mSources.size(); ++i)
    if (mSources[i].key == key) return i;
  return std::string::npos;
}

// The desired list staged for the next reconcile. Seeded from the live slots (in display order)
// unless a reconcile is already pending, so back-to-back commands compose rather than the
// second one discarding the first.
std::vector<JackTransportLink::DesiredSink>& JackTransportLink::pendingSinks() {
  if (!mNeedsReconcileSinks.load(std::memory_order_acquire)) {
    mDesiredSinks.clear();
    for (const auto& key : mSinkOrder) {
      const size_t i = findSinkByKey(key);
      if (i != std::string::npos)
        mDesiredSinks.push_back({mSinks[i].key, mSinks[i].name});
    }
  }
  return mDesiredSinks;
}

std::vector<JackTransportLink::DesiredSource>& JackTransportLink::pendingSources() {
  if (!mNeedsReconcileSources.load(std::memory_order_acquire)) {
    mDesiredSources.clear();
    for (const auto& key : mSourceOrder) {
      const size_t i = findSource(key);
      if (i != std::string::npos)
        mDesiredSources.push_back({mSources[i].key, mSources[i].peer, mSources[i].channel});
    }
  }
  return mDesiredSources;
}

// Effective Link peer name: the user override if set, otherwise the device hostname.
// Link truncates names beyond 256 chars; the hostname buffer bounds us well under that.
std::string JackTransportLink::effectiveLinkPeerName() const {
  std::lock_guard<std::recursive_mutex> lock(mControlMutex);
  if (!mLinkPeerName.empty())
    return mLinkPeerName;
  char host[256];
  if (gethostname(host, sizeof(host)) == 0) {
    host[sizeof(host) - 1] = '\0';
    if (host[0] != '\0')
      return std::string(host);
  }
  // last-resort fallback if the hostname can't be read
  return "jack-transport-link";
}

// Publish the effective peer name so a client can display what's actually broadcast.
void JackTransportLink::setLinkAudioPeerNameProperty() {
  if (jack_uuid_empty(mJackClientUUID)) return;
  const auto eff = effectiveLinkPeerName();
  jack_set_property(mJackClient, mJackClientUUID, linkaudio_peer_name_key.c_str(),
                    eff.c_str(), string_type);
}

// Publish the current receiver playout buffer (ms) so a client reflects the applied value.
void JackTransportLink::setLinkAudioLatencyMsProperty() {
  if (jack_uuid_empty(mJackClientUUID)) return;
  std::string s = std::to_string(mLatencyMs.load(std::memory_order_acquire));
  jack_set_property(mJackClient, mJackClientUUID, linkaudio_latency_key.c_str(),
                    s.c_str(), decimal_type);
}

// Publish the current "Sync to Incoming Audio" toggle so a client reflects the applied value.
void JackTransportLink::setLinkAudioSyncToIncomingProperty() {
  if (jack_uuid_empty(mJackClientUUID)) return;
  const char* s = mSyncToIncomingAudio.load(std::memory_order_acquire) ? "true" : "false";
  jack_set_property(mJackClient, mJackClientUUID, linkaudio_sync_key.c_str(), s, bool_type);
}

// Recompute the effective peer name; rename the Link peer only when it changed (setPeerName
// is thread-safe/non-RT), then always republish so readers reflect the current value.
void JackTransportLink::applyLinkPeerName() {
  const auto eff = effectiveLinkPeerName();
  if (eff != mAppliedLinkPeerName) {
    mLink.setPeerName(eff);
    mAppliedLinkPeerName = eff;
  }
  setLinkAudioPeerNameProperty();
}

// jackd logs a "DB_NOTFOUND" error when asked to remove a property that was never set,
// so guard removal on existence to avoid that log spam.
void JackTransportLink::removePropertyIfExists(const std::string& key) {
  if (jack_uuid_empty(mJackClientUUID)) return;
  std::string v, t;
  if (get_property(mJackClientUUID, key, v, t))
    jack_remove_property(mJackClient, mJackClientUUID, key.c_str());
}

// Set the port-group + pretty-name on our own audio ports so patchbays present them as a
// "jack-link-audio" node. The port *names* are hash-derived (in_<key>_l), which is what makes
// them stable per identity; the pretty name is what a UI shows: a sink's announced name, a
// source's "<peer>: <channel>". Iterates the display-order vectors so ORDER follows the user's
// arrangement. Only writes on change to avoid notification churn; JACK removes this metadata
// automatically when the ports are unregistered.
void JackTransportLink::updateAudioPortMetadata() {
  if (!mLinkAudioEnabled) return;
  auto setIfChanged = [this](jack_port_t* port, const std::string& key, const std::string& val, const char* type) {
    if (!port) return;
    jack_uuid_t u = jack_port_uuid(port);
    if (jack_uuid_empty(u)) return;
    std::string cur, t;
    if (!(get_property(u, key, cur, t) && cur == val))
      jack_set_property(mJackClient, u, key.c_str(), val.c_str(), type);
  };
  auto decorate = [&](jack_port_t* port, const std::string& pretty, int order) {
    setIfChanged(port, port_group_key, link_audio_port_group, string_type);
    setIfChanged(port, pretty_name_key, pretty, string_type);
    setIfChanged(port, order_key, std::to_string(order), order_type);
  };
  int pos = 0;
  for (const auto& key : mSinkOrder) {
    const size_t i = findSinkByKey(key);
    if (i == std::string::npos) continue;
    decorate(mSinks[i].portL, mSinks[i].name + " L", 2 * pos + 1);
    decorate(mSinks[i].portR, mSinks[i].name + " R", 2 * pos + 2);
    ++pos;
  }
  pos = 0;
  for (const auto& key : mSourceOrder) {
    const size_t i = findSource(key);
    if (i == std::string::npos) continue;
    const auto& s = mSources[i];
    // fall back to the raw channel (or peer) when the other field is empty
    const std::string base = (s.peer.size() && s.channel.size())
        ? s.peer + ": " + s.channel : s.peer + s.channel;
    decorate(s.portL, base + " L", 2 * pos + 1);
    decorate(s.portR, base + " R", 2 * pos + 2);
    ++pos;
  }
}

// Zero the cumulative dropout count of one source (by slot key) or of every source (target empty
// or "*"). Only touches an atomic the receive path owns, so it needs no deactivate; callers hold
// mControlMutex, which is what serializes the mSources read against reconcileSources.
void JackTransportLink::resetSourceDropouts(const std::string& target) {
  const bool all = target.empty() || target == "*";
  bool any = false;
  for (auto& slot : mSources) {
    if (!slot.renderer) continue;
    if (!all && slot.key != target) continue;
    slot.renderer->resetDropoutCount();
    any = true;
  }
  // republish immediately rather than waiting out the telemetry timer, so the UI's count
  // visibly returns to zero the moment the reset is asked for
  if (any) mReportLinkAudioSource = true;
}

// Bind each source slot to the advertised channel whose peer *and* channel names match its
// configured identity exactly (case-sensitive). Nothing is ever auto-selected: a slot whose
// identity isn't currently advertised simply stays disconnected, keeping its ports, and
// reconnects on its own when a channel with that identity reappears.
bool JackTransportLink::updateLinkAudioSource() {
  auto channels = mLink.channels();
  bool anyChanged = false;

  for (auto& slot : mSources) {
    if (!slot.renderer) continue;

    const ableton::LinkAudio::Channel* target = nullptr;
    for (const auto& ch : channels) {
      if (ch.peerName == slot.peer && ch.name == slot.channel) {
        target = &ch;
        break;
      }
    }

    if (target) {
      const bool needSwitch = !slot.renderer->hasSource()
          || !slot.currentId.has_value()
          || (*slot.currentId != target->id);
      if (needSwitch) {
        slot.renderer->removeSource();
        slot.renderer->createSource(target->id);
        slot.currentId = target->id;
        anyChanged = true;
      }
    } else if (slot.renderer->hasSource() || slot.currentId.has_value()) {
      slot.renderer->removeSource();
      slot.currentId = std::nullopt;
      anyChanged = true;
    }
  }
  return anyChanged;
}

std::vector<JackTransportLink::PortConn> JackTransportLink::snapshotConnections() const {
  std::vector<PortConn> conns;
  auto snapshot = [&conns](jack_port_t* port, bool output) {
    if (!port) return;
    const char** c = jack_port_get_connections(port);
    if (!c) return;
    const std::string mine = jack_port_name(port);
    for (const char** p = c; *p; ++p) conns.push_back({mine, *p, output});
    jack_free(c);
  };
  for (const auto& s : mSinks)   { snapshot(s.portL, false); snapshot(s.portR, false); }
  for (const auto& s : mSources) { snapshot(s.portL, true);  snapshot(s.portR, true);  }
  snapshot(mMIDIClockOut, true);
  return conns;
}

void JackTransportLink::restoreConnections(
    const std::vector<PortConn>& conns,
    const std::map<std::string, std::string>& rename) {
  for (const auto& c : conns) {
    std::string mine = c.mine;
    const auto it = rename.find(mine);
    if (it != rename.end()) mine = it->second;
    // a removed slot's ports are gone; don't bother jackd with a doomed connect
    if (!jack_port_by_name(mJackClient, mine.c_str())) continue;
    if (c.output)
      jack_connect(mJackClient, mine.c_str(), c.other.c_str());
    else
      jack_connect(mJackClient, c.other.c_str(), mine.c_str());
  }
}

void JackTransportLink::registerSlotPorts(const std::string& key, bool input,
                                          jack_port_t*& portL, jack_port_t*& portR) {
  const std::string base = (input ? "in_" : "out_") + key;
  const auto flags = input ? (JackPortIsInput | JackPortIsTerminal)
                           : (JackPortIsOutput | JackPortIsTerminal);
  portL = jack_port_register(mJackClient, (base + "_l").c_str(),
                             JACK_DEFAULT_AUDIO_TYPE, flags, 0);
  portR = jack_port_register(mJackClient, (base + "_r").c_str(),
                             JACK_DEFAULT_AUDIO_TYPE, flags, 0);
}

// Reconcile the live sink slots against `desired`. One pass covers add, remove, rename and
// reorder, and it's idempotent — which is what a JACK metadata property (a value you both read
// and write) needs. Rejected entries are skipped and the rest applied, then the canonical value
// is republished so a client's read-back self-corrects.
void JackTransportLink::reconcileSinks(const std::vector<DesiredSink>& desired) {
  if (!mLinkAudioEnabled) return;

  // Resolve each desired entry to a target: which existing slot it binds to (npos = a new slot),
  // and the name/key it should end up with.
  struct Target { std::string name; std::string key; size_t slot; };
  std::vector<Target> targets;
  std::vector<bool> claimed(mSinks.size(), false);
  std::set<std::string> namesSeen, keysSeen;

  // Bind first, validate second. A rejected rename has to leave its slot alone under the
  // previous name — dropping the entry would *delete* the slot, and removal is only ever
  // expressed by omitting an entry, never by writing a bad name into one.
  struct Entry { std::string name; size_t slot; };
  std::vector<Entry> entries;
  entries.reserve(desired.size());
  std::vector<size_t> boundEntry(mSinks.size(), std::string::npos); // slot -> entry index
  for (const auto& d : desired) {
    // A key binds to an existing slot — that's how a rename arrives (existing key, new name).
    // Otherwise match by identity; a name-only entry is self-keying since the key derives from it.
    size_t slot = std::string::npos;
    if (!d.key.empty()) {
      for (size_t i = 0; i < mSinks.size(); ++i)
        if (boundEntry[i] == std::string::npos && mSinks[i].key == d.key) { slot = i; break; }
    }
    if (slot == std::string::npos && !d.name.empty()) {
      for (size_t i = 0; i < mSinks.size(); ++i)
        if (boundEntry[i] == std::string::npos && mSinks[i].name == d.name) { slot = i; break; }
    }
    if (slot != std::string::npos) boundEntry[slot] = entries.size();
    entries.push_back({d.name, slot});
  }

  auto nameProblem = [&](const std::string& n) -> const char* {
    if (n.empty()) return "is empty";
    if (namesSeen.count(n)) return "is already used by another sink";
    // Two distinct names hashing to the same 48-bit key. Refusing the later one is
    // order-independent and never renames an existing slot's ports.
    if (keysSeen.count(sinkSlotKey(n))) return "collides with another sink's slot key";
    return nullptr;
  };

  std::vector<bool> accepted(entries.size(), false);
  std::vector<std::string> finalName(entries.size());
  // Pass 1 — entries keeping their slot's current name reserve it up front. That's what makes a
  // rename onto a name another slot is holding get rejected regardless of where the two entries
  // fall in the list.
  for (size_t e = 0; e < entries.size(); ++e) {
    const auto& en = entries[e];
    if (en.slot == std::string::npos || en.name != mSinks[en.slot].name) continue;
    accepted[e] = true;
    finalName[e] = en.name;
    namesSeen.insert(en.name);
    keysSeen.insert(mSinks[en.slot].key);
  }
  // Pass 2 — adds and renames, first come first served.
  for (size_t e = 0; e < entries.size(); ++e) {
    if (accepted[e]) continue;
    const auto& en = entries[e];
    std::string name = en.name;
    if (const char* problem = nameProblem(name)) {
      if (en.slot != std::string::npos && !nameProblem(mSinks[en.slot].name)) {
        std::cerr << "warning: Link Audio sink name \"" << name << "\" " << problem
                  << ", keeping \"" << mSinks[en.slot].name << "\"\n";
        name = mSinks[en.slot].name;
      } else {
        std::cerr << "warning: ignoring Link Audio sink, name \"" << name << "\" "
                  << problem << "\n";
        continue;
      }
    }
    accepted[e] = true;
    finalName[e] = name;
    namesSeen.insert(name);
    keysSeen.insert(sinkSlotKey(name));
  }

  for (size_t e = 0; e < entries.size(); ++e) {
    if (!accepted[e]) continue;
    const size_t slot = entries[e].slot;
    if (slot != std::string::npos) claimed[slot] = true;
    targets.push_back({finalName[e], sinkSlotKey(finalName[e]), slot});
  }

  std::vector<std::string> order;
  order.reserve(targets.size());
  for (const auto& t : targets) order.push_back(t.key);

  bool structural = std::find(claimed.begin(), claimed.end(), false) != claimed.end();
  for (const auto& t : targets)
    if (t.slot == std::string::npos || mSinks[t.slot].name != t.name) structural = true;

  if (!structural) {
    // Reorder (or no-op): permute the display order and republish ORDER metadata. Nothing the RT
    // thread reads is touched, so no deactivate and no dropped connections.
    if (order != mSinkOrder) {
      mSinkOrder = std::move(order);
      mUpdatePortMeta = true;
      mNeedsSaveConfig = true;
    }
    setLinkAudioSinksProperty();
    return;
  }

  const auto conns = mJackActivated ? snapshotConnections() : std::vector<PortConn>();
  if (mJackActivated) jack_deactivate(mJackClient);

  for (size_t i = 0; i < mSinks.size(); ++i) {
    if (claimed[i]) continue;
    if (mSinks[i].portL) jack_port_unregister(mJackClient, mSinks[i].portL);
    if (mSinks[i].portR) jack_port_unregister(mJackClient, mSinks[i].portR);
    mSinks[i].portL = mSinks[i].portR = nullptr;
    mSinks[i].renderer.reset();
  }

  // The key is a function of the name, so a rename re-registers the port pair. Map the old port
  // names to the new ones so the live graph survives (connections already written into a *saved
  // set* won't match until the set is re-saved — a lost connection, never a wrong one).
  std::map<std::string, std::string> portRename;
  for (const auto& t : targets) {
    if (t.slot == std::string::npos) continue;
    auto& s = mSinks[t.slot];
    if (s.name == t.name) continue;
    const std::string oldL = s.portL ? jack_port_name(s.portL) : std::string();
    const std::string oldR = s.portR ? jack_port_name(s.portR) : std::string();
    if (s.portL) jack_port_unregister(mJackClient, s.portL);
    if (s.portR) jack_port_unregister(mJackClient, s.portR);
    s.name = t.name;
    s.key  = t.key;
    registerSlotPorts(s.key, true, s.portL, s.portR);
    s.renderer = std::make_unique<SinkRenderer>(mLink, s.name, 2, mSampleRate);
    if (!oldL.empty() && s.portL) portRename[oldL] = jack_port_name(s.portL);
    if (!oldR.empty() && s.portR) portRename[oldR] = jack_port_name(s.portR);
  }

  // Keep the surviving slots in creation order (the RT callback iterates by index; only the
  // display-order vector encodes the user's arrangement), then append the new ones.
  {
    std::vector<SinkSlot> kept;
    kept.reserve(targets.size());
    for (size_t i = 0; i < mSinks.size(); ++i)
      if (claimed[i]) kept.push_back(std::move(mSinks[i]));
    mSinks = std::move(kept);
  }
  for (const auto& t : targets) {
    if (t.slot != std::string::npos) continue;
    SinkSlot s;
    s.name = t.name;
    s.key  = t.key;
    registerSlotPorts(s.key, true, s.portL, s.portR);
    s.renderer = std::make_unique<SinkRenderer>(mLink, s.name, 2, mSampleRate);
    mSinks.push_back(std::move(s));
  }

  if (mJackActivated) {
    jack_activate(mJackClient);
    restoreConnections(conns, portRename);
  }

  mSinkOrder = std::move(order);
  setLinkAudioSinksProperty();
  mUpdatePortMeta = true;
  mNeedsSaveConfig = true;
}

// Reconcile the live source slots against `desired`. Sources have no rename: a different
// peer/channel is a different identity, so it's a remove plus an add.
void JackTransportLink::reconcileSources(const std::vector<DesiredSource>& desired) {
  if (!mLinkAudioEnabled) return;

  struct Target { std::string peer; std::string channel; std::string key; size_t slot; };
  std::vector<Target> targets;
  std::vector<bool> claimed(mSources.size(), false);
  std::set<std::string> keysSeen;

  for (const auto& d : desired) {
    std::string peer = d.peer, channel = d.channel;
    size_t slot = std::string::npos;
    if (channel.empty()) {
      // No identity: an entry carrying only a key refers to an existing slot (this is what an
      // order-only write looks like). A key that matches nothing can't create anything.
      if (d.key.empty()) continue;
      slot = findSource(d.key);
      if (slot == std::string::npos || claimed[slot]) {
        std::cerr << "warning: ignoring Link Audio source entry with unknown key \""
                  << d.key << "\"\n";
        continue;
      }
      peer    = mSources[slot].peer;
      channel = mSources[slot].channel;
    }
    const auto key = sourceSlotKey(peer, channel);
    if (keysSeen.count(key)) {
      std::cerr << "warning: ignoring duplicate Link Audio source \"" << peer << "\" / \""
                << channel << "\"\n";
      continue;
    }
    if (slot == std::string::npos) {
      for (size_t i = 0; i < mSources.size(); ++i)
        if (!claimed[i] && mSources[i].peer == peer && mSources[i].channel == channel) {
          slot = i;
          break;
        }
    }
    if (slot != std::string::npos) claimed[slot] = true;
    keysSeen.insert(key);
    targets.push_back({std::move(peer), std::move(channel), key, slot});
  }

  std::vector<std::string> order;
  order.reserve(targets.size());
  for (const auto& t : targets) order.push_back(t.key);

  bool structural = std::find(claimed.begin(), claimed.end(), false) != claimed.end();
  for (const auto& t : targets)
    if (t.slot == std::string::npos) structural = true;

  if (!structural) {
    if (order != mSourceOrder) {
      mSourceOrder = std::move(order);
      mUpdatePortMeta = true;
      mNeedsSaveConfig = true;
    }
    setLinkAudioSourcesProperty();
    return;
  }

  const auto conns = mJackActivated ? snapshotConnections() : std::vector<PortConn>();
  if (mJackActivated) jack_deactivate(mJackClient);

  for (size_t i = 0; i < mSources.size(); ++i) {
    if (claimed[i]) continue;
    if (mSources[i].renderer) mSources[i].renderer->removeSource();
    if (mSources[i].portL) jack_port_unregister(mJackClient, mSources[i].portL);
    if (mSources[i].portR) jack_port_unregister(mJackClient, mSources[i].portR);
    mSources[i].portL = mSources[i].portR = nullptr;
    mSources[i].renderer.reset();
    mSources[i].currentId = std::nullopt;
  }

  {
    std::vector<SourceSlot> kept;
    kept.reserve(targets.size());
    for (size_t i = 0; i < mSources.size(); ++i)
      if (claimed[i]) kept.push_back(std::move(mSources[i]));
    mSources = std::move(kept);
  }
  for (const auto& t : targets) {
    if (t.slot != std::string::npos) continue;
    SourceSlot s;
    s.peer    = t.peer;
    s.channel = t.channel;
    s.key     = t.key;
    registerSlotPorts(s.key, false, s.portL, s.portR);
    s.renderer = std::make_unique<SourceRenderer>(mLink, 2, mSampleRate);
    mSources.push_back(std::move(s));
  }

  if (mJackActivated) {
    jack_activate(mJackClient);
    restoreConnections(conns, {});
  }

  mSourceOrder = std::move(order);
  setLinkAudioSourcesProperty();
  mUpdatePortMeta = true;
  mNeedsSaveConfig = true;
  mNeedsSourceUpdate.store(true, std::memory_order_release);
  // Republish status even when no connection changed: a removed slot's stale entry has to go,
  // and with zero sources left the periodic publish in processEvents doesn't run at all.
  mReportLinkAudioSource = true;
}

void JackTransportLink::invalidateClockSyncBBT() {
  mBeatLast = -1;
  mBarLast = -1;
  mTickLast = -1.0;
}

void JackTransportLink::ProcessMessage(
    const oscpack::ReceivedMessage &m,
    const oscpack::IpEndpointName &remoteEndpoint) {
  std::lock_guard<std::recursive_mutex> lock(mControlMutex);
  try {
    oscpack::ReceivedMessageArgumentStream args = m.ArgumentStream();
    oscpack::ReceivedMessage::const_iterator arg = m.ArgumentsBegin();
    // std::cout << "got osc message " << m.AddressPattern() << std::endl;
    if (std::strcmp("/jacklink/bpm", m.AddressPattern()) == 0) {
      if (arg != m.ArgumentsEnd()) {
        std::optional<double> v = GetOscDouble(*arg);
        if (v && *v > 0.0) {
          mBPM.store(*v);
          mReportBPM = true;
        }
      }
    } else if (std::strcmp("/jacklink/beattime", m.AddressPattern()) == 0) {
      if (arg != m.ArgumentsEnd()) {
        std::optional<double> v = GetOscDouble(*arg);
        if (v && *v >= 0.0) {
          jack_position_t pos;
          jack_transport_query(mJackClient, &pos);

          const double tpb = static_cast<double>(pos.ticks_per_beat);
          double abs_tick = *v * tpb;
          double minute =
              abs_tick / (static_cast<double>(pos.beats_per_minute) * tpb);
          pos.frame = minute * static_cast<double>(pos.frame_rate) * 60.0;

          jack_transport_reposition(mJackClient, &pos);
        }
      }
    } else if (std::strcmp("/jacklink/sync", m.AddressPattern()) == 0) {
      if (arg != m.ArgumentsEnd() && arg->IsBool()) {
        const bool was = mSyncLink.load(std::memory_order_acquire);
        const bool sync = arg->AsBoolUnchecked();
        mSyncLink.store(sync, std::memory_order_release);
        if (sync && !was) {
          mBPM.store(mLinkBPM.load(std::memory_order_acquire),
                     std::memory_order_release);
          mReportBPM = true;
        }
        setSyncProperty(sync);
        mNeedsSaveConfig = true;
      }
    } else if (std::strcmp("/jacklink/rolling", m.AddressPattern()) == 0) {
      if (arg != m.ArgumentsEnd() && arg->IsBool()) {
        bool rolling = arg->AsBoolUnchecked();
        if (rolling) {
          jack_transport_start(mJackClient);
        } else {
          jack_transport_stop(mJackClient);
        }
      }
    } else if (std::strcmp("/jacklink/start-stop-sync", m.AddressPattern()) == 0) {
      if (arg != m.ArgumentsEnd() && arg->IsBool()) {
        bool v = arg->AsBoolUnchecked();
        mLink.enableStartStopSync(v);
        setEnableStartStopProperty(v);
        mNeedsSaveConfig = true;
      }
    } else if (std::strcmp("/jacklink/enabled", m.AddressPattern()) == 0) {
      // mLink.enable() isn't RT-safe, so defer the actual toggle to processEvents.
      if (arg != m.ArgumentsEnd() && arg->IsBool()) {
        bool v = arg->AsBoolUnchecked();
        if (v != mLinkEnabledDesired.load(std::memory_order_acquire)) {
          mLinkEnabledDesired.store(v, std::memory_order_release);
          mNeedsApplyLinkEnabled.store(true, std::memory_order_release);
          mNeedsSaveConfig = true;
        }
        setLinkEnabledProperty();
      }
    } else if (std::strcmp("/jacklink/audio/peer-name", m.AddressPattern()) == 0) {
      // Non-empty sets the override; empty string clears it (reverts to hostname).
      // Unlike JACK metadata, OSC can carry an empty string, so we handle it here.
      if (arg != m.ArgumentsEnd() && arg->IsString()) {
        mLinkPeerName = arg->AsStringUnchecked();
        mNeedsApplyPeerName.store(true, std::memory_order_release);
        mNeedsSaveConfig = true;
      }
    } else if (std::strcmp("/jacklink/audio/latency", m.AddressPattern()) == 0) {
      if (arg != m.ArgumentsEnd()) {
        std::optional<double> v = GetOscDouble(*arg);
        if (v) {
          const double clamped = clampLatencyMs(*v);
          if (clamped != mLatencyMs.load(std::memory_order_acquire)) {
            mLatencyMs.store(clamped, std::memory_order_release);
            mNeedsSaveConfig = true;
          }
          setLinkAudioLatencyMsProperty();
        }
      }
    } else if (std::strcmp("/jacklink/audio/sync-to-incoming", m.AddressPattern()) == 0) {
      if (arg != m.ArgumentsEnd() && arg->IsBool()) {
        bool v = arg->AsBoolUnchecked();
        if (v != mSyncToIncomingAudio.load(std::memory_order_acquire)) {
          mSyncToIncomingAudio.store(v, std::memory_order_release);
          mNeedsSaveConfig = true;
        }
        setLinkAudioSyncToIncomingProperty();
      }
    } else if (mLinkAudioEnabled &&
               std::strcmp("/jacklink/audio/sink/add", m.AddressPattern()) == 0) {
      // Imperative sugar over the declarative list: mutate the desired list and let
      // processEvents reconcile it (validation, including the duplicate-name rejection, lives
      // there). A name can contain a '/', so the identifier is an argument, not an address part.
      if (arg != m.ArgumentsEnd() && arg->IsString()) {
        pendingSinks().push_back({std::string(), arg->AsStringUnchecked()});
        mNeedsReconcileSinks.store(true, std::memory_order_release);
      }
    } else if (mLinkAudioEnabled &&
               std::strcmp("/jacklink/audio/sink/remove", m.AddressPattern()) == 0) {
      if (arg != m.ArgumentsEnd() && arg->IsString()) {
        const size_t i = findSink(arg->AsStringUnchecked());
        if (i != std::string::npos) {
          const auto key = mSinks[i].key;
          auto& desired = pendingSinks();
          desired.erase(std::remove_if(desired.begin(), desired.end(),
                                       [&key](const DesiredSink& d) { return d.key == key; }),
                        desired.end());
          mNeedsReconcileSinks.store(true, std::memory_order_release);
        }
      }
    } else if (mLinkAudioEnabled &&
               std::strcmp("/jacklink/audio/sink/rename", m.AddressPattern()) == 0) {
      if (arg != m.ArgumentsEnd() && arg->IsString()) {
        const std::string from = (arg++)->AsStringUnchecked();
        if (arg != m.ArgumentsEnd() && arg->IsString()) {
          const std::string to = arg->AsStringUnchecked();
          const size_t i = findSink(from);
          if (i != std::string::npos) {
            const auto key = mSinks[i].key;
            for (auto& d : pendingSinks())
              if (d.key == key) d.name = to;
            mNeedsReconcileSinks.store(true, std::memory_order_release);
          }
        }
      }
    } else if (mLinkAudioEnabled &&
               std::strcmp("/jacklink/audio/sinks/order", m.AddressPattern()) == 0) {
      // Slots named by the arguments come first, in the given order; anything omitted keeps its
      // relative order at the end.
      auto current = pendingSinks();
      std::vector<DesiredSink> reordered;
      for (; arg != m.ArgumentsEnd(); ++arg) {
        if (!arg->IsString()) continue;
        const size_t i = findSink(arg->AsStringUnchecked());
        if (i == std::string::npos) continue;
        const auto& key = mSinks[i].key;
        auto it = std::find_if(current.begin(), current.end(),
                               [&key](const DesiredSink& d) { return d.key == key; });
        if (it == current.end()) continue;
        reordered.push_back(*it);
        current.erase(it);
      }
      reordered.insert(reordered.end(), current.begin(), current.end());
      mDesiredSinks = std::move(reordered);
      mNeedsReconcileSinks.store(true, std::memory_order_release);
    } else if (mLinkAudioEnabled &&
               std::strcmp("/jacklink/audio/source/add", m.AddressPattern()) == 0) {
      if (arg != m.ArgumentsEnd() && arg->IsString()) {
        const std::string peer = (arg++)->AsStringUnchecked();
        if (arg != m.ArgumentsEnd() && arg->IsString()) {
          pendingSources().push_back({std::string(), peer, arg->AsStringUnchecked()});
          mNeedsReconcileSources.store(true, std::memory_order_release);
        }
      }
    } else if (mLinkAudioEnabled &&
               std::strcmp("/jacklink/audio/source/remove", m.AddressPattern()) == 0) {
      // Either (peer, channel) or a single key.
      if (arg != m.ArgumentsEnd() && arg->IsString()) {
        const std::string first = (arg++)->AsStringUnchecked();
        std::string key;
        if (arg != m.ArgumentsEnd() && arg->IsString()) {
          key = sourceSlotKey(first, arg->AsStringUnchecked());
        } else {
          key = first;
        }
        if (findSource(key) != std::string::npos) {
          auto& desired = pendingSources();
          desired.erase(std::remove_if(desired.begin(), desired.end(),
                                       [&key](const DesiredSource& d) {
                                         return (d.channel.empty() ? d.key
                                                 : sourceSlotKey(d.peer, d.channel)) == key;
                                       }),
                        desired.end());
          mNeedsReconcileSources.store(true, std::memory_order_release);
        }
      }
    } else if (mLinkAudioEnabled &&
               std::strcmp("/jacklink/audio/source/reset-dropouts", m.AddressPattern()) == 0) {
      // no args = every source; one string = a slot key; two = peer + channel
      std::string target = "*";
      if (arg != m.ArgumentsEnd() && arg->IsString()) {
        const std::string first = (arg++)->AsStringUnchecked();
        target = (arg != m.ArgumentsEnd() && arg->IsString())
            ? sourceSlotKey(first, arg->AsStringUnchecked()) : first;
      }
      resetSourceDropouts(target);
    } else if (mLinkAudioEnabled &&
               std::strcmp("/jacklink/audio/sources/order", m.AddressPattern()) == 0) {
      auto current = pendingSources();
      std::vector<DesiredSource> reordered;
      for (; arg != m.ArgumentsEnd(); ++arg) {
        if (!arg->IsString()) continue;
        const std::string key = arg->AsStringUnchecked();
        auto it = std::find_if(current.begin(), current.end(),
                               [&key](const DesiredSource& d) {
                                 return (d.channel.empty() ? d.key
                                         : sourceSlotKey(d.peer, d.channel)) == key;
                               });
        if (it == current.end()) continue;
        reordered.push_back(*it);
        current.erase(it);
      }
      reordered.insert(reordered.end(), current.begin(), current.end());
      mDesiredSources = std::move(reordered);
      mNeedsReconcileSources.store(true, std::memory_order_release);
    }
  } catch (oscpack::Exception &e) {
    std::cerr << "error while parsing message: " << m.AddressPattern() << ": "
              << e.what() << "\n";
  }
}

void JackTransportLink::saveConfig() {
  std::lock_guard<std::recursive_mutex> controlLock(mControlMutex);
  if (mConfigPath.empty()) return;
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::create_directories(fs::path(mConfigPath).parent_path(), ec);

  nlohmann::json cfg;
  cfg["bpm"]               = mBPM.load(std::memory_order_acquire);
  cfg["quantum"]           = mInitialQuantum;
  cfg["time_sig_denom"]    = mInitialTimeSigDenom;
  cfg["ticks_per_beat"]    = mInitialTicksPerBeat;
  cfg["start_stop_sync"]   = mLink.isStartStopSyncEnabled();
  cfg["sync"] = mSyncLink.load(std::memory_order_acquire);
  cfg["link_audio_enabled"]    = mLinkAudioEnabled;
  // Sinks and sources in display order, identities only — slot keys are re-derived on load,
  // which is what makes the JACK port names stable across restarts.
  nlohmann::json sinks = nlohmann::json::array();
  for (const auto& key : mSinkOrder) {
    const size_t i = findSinkByKey(key);
    if (i != std::string::npos)
      sinks.push_back({{"name", mSinks[i].name}});
  }
  cfg["sinks"] = sinks;
  nlohmann::json sources = nlohmann::json::array();
  for (const auto& key : mSourceOrder) {
    const size_t i = findSource(key);
    if (i != std::string::npos)
      sources.push_back({{"peer", mSources[i].peer}, {"channel", mSources[i].channel}});
  }
  cfg["sources"] = sources;
  cfg["link_peer_name"] = mLinkPeerName;
  cfg["link_audio_capture_latency_trim_ms"]  = mCaptureLatencyTrimMs.load(std::memory_order_acquire);
  cfg["link_audio_playback_latency_trim_ms"] = mPlaybackLatencyTrimMs.load(std::memory_order_acquire);
  cfg["link_audio_latency_ms"]               = mLatencyMs.load(std::memory_order_acquire);
  cfg["link_audio_sync_to_incoming"]         = mSyncToIncomingAudio.load(std::memory_order_acquire);
  cfg["link_enabled"]                        = mLinkEnabledDesired.load(std::memory_order_acquire);

  std::ofstream f(mConfigPath);
  if (f.is_open())
    f << cfg.dump(2) << "\n";
}
