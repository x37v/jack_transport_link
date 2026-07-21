#include "JackTransportLink.hpp"

#include <jack/midiport.h>
#include <jack/uuid.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

// debugging defines

// #define DO_CLICK_OUT

// send midi start at the start of every bar
// #define MIDI_SEND_REPEATED_STARTS

#define MIDI_PPQ 24

static constexpr int kMaxLinkAudioStereoPairs = 64;

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
const std::string
    linkaudio_source_key("http://www.x37v.info/jack/metadata/linkaudio/source");
const std::string
    linkaudio_sink_key("http://www.x37v.info/jack/metadata/linkaudio/sink");
const std::string
    linkaudio_source_filters_key("http://www.x37v.info/jack/metadata/linkaudio/source-filters");
// Currently-connected peer/channel per source, published here (not on the source key)
// so our own status publishing doesn't feed back into the desired-filter parsing.
const std::string
    linkaudio_source_status_key("http://www.x37v.info/jack/metadata/linkaudio/source-status");
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
const std::string
    linkaudio_in_stereo_key("http://www.x37v.info/jack/metadata/linkaudio/in-stereo-channels");
const std::string
    linkaudio_out_stereo_key("http://www.x37v.info/jack/metadata/linkaudio/out-stereo-channels");
// The local Link peer name broadcast to the session, decoupled from the JACK client name.
const std::string
    linkaudio_peer_name_key("http://www.x37v.info/jack/metadata/linkaudio/peer-name");
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

// Parse a source filter value into per-receiver peer/channel filter vectors.
// Accepts a single object (sets index 0 only) or an array (sets each index by position).
// {} / [] clears the affected filters, reverting to auto-select.
// Returns false on parse error; leaves vectors unchanged on error.
bool parseLinkAudioSourceFilters(const std::string& s,
                                 std::vector<std::string>& peersOut,
                                 std::vector<std::string>& channelsOut) {
  try {
    auto j = nlohmann::json::parse(s);
    if (j.is_object()) {
      if (!peersOut.empty()) {
        peersOut[0]    = j.value("peer",    "");
        channelsOut[0] = j.value("channel", "");
      }
      return true;
    } else if (j.is_array()) {
      const size_t n = std::min(j.size(), peersOut.size());
      for (size_t i = 0; i < n; ++i) {
        peersOut[i]    = j[i].value("peer",    "");
        channelsOut[i] = j[i].value("channel", "");
      }
      return true;
    }
    return false;
  } catch (...) {
    return false;
  }
}

// Returns the receiver index from a key of the form linkaudio_source_key + "/<N>", else -1.
int linkAudioSourceIndex(const char* key) {
  if (!key) return -1;
  const size_t plen = linkaudio_source_key.size();
  if (std::strncmp(key, linkaudio_source_key.c_str(), plen) != 0) return -1;
  if (key[plen] != '/') return -1;
  const char* idxStr = key + plen + 1;
  if (!*idxStr) return -1;
  char* end;
  long idx = std::strtol(idxStr, &end, 10);
  if (*end != '\0' || idx < 0) return -1;
  return static_cast<int>(idx);
}

// Returns the slot index from a key of the form base + "/<N>/name", else -1.
// base is linkaudio_source_key or linkaudio_sink_key.
int linkAudioNameIndex(const char* key, const std::string& base) {
  if (!key) return -1;
  const size_t blen = base.size();
  if (std::strncmp(key, base.c_str(), blen) != 0) return -1;
  if (key[blen] != '/') return -1;
  const char* idxStr = key + blen + 1;
  char* end;
  long idx = std::strtol(idxStr, &end, 10);
  if (end == idxStr || idx < 0) return -1;
  if (std::strcmp(end, "/name") != 0) return -1;
  return static_cast<int>(idx);
}

// Parse "<prefix><N><suffix>" OSC addresses, returning N (>=0), else -1.
long parseOscNameIndex(const char* address, const char* prefix, const char* suffix) {
  const size_t plen = std::strlen(prefix);
  if (std::strncmp(address, prefix, plen) != 0) return -1;
  const char* idxStr = address + plen;
  char* end;
  long idx = std::strtol(idxStr, &end, 10);
  if (end == idxStr || idx < 0) return -1;
  if (std::strcmp(end, suffix) != 0) return -1;
  return idx;
}

} // namespace

JackTransportLink::JackTransportLink(jack_client_t *client,
                                     bool enableStartStopSync,
                                     double initialBPM, double initialQuantum,
                                     float initialTimeSigDenom,
                                     double initialTicksPerBeat,
                                     bool enableLinkAudio,
                                     size_t linkAudioStereoInChannels,
                                     size_t linkAudioStereoOutChannels,
                                     bool syncLink,
                                     std::string configPath,
                                     std::vector<std::string> sinkNames,
                                     std::vector<std::string> sourceNames,
                                     std::string linkPeerName,
                                     double captureLatencyTrimMs,
                                     double playbackLatencyTrimMs)
    : mJackClient(client),
      mSampleRate(static_cast<double>(jack_get_sample_rate(client))),
      mNumStereoInChannels(linkAudioStereoInChannels),
      mNumStereoOutChannels(linkAudioStereoOutChannels),
      mLinkPeerName(std::move(linkPeerName)),
      mBPM(initialBPM), mQuantum(initialQuantum),
      mInitialQuantum(initialQuantum),
      mInitialTimeSigDenom(initialTimeSigDenom),
      mInitialTicksPerBeat(initialTicksPerBeat),
      // Link peer name is decoupled from the JACK client name (which stays
      // "jack-transport-link" for the runner/port-bridge); default to the hostname.
      mLink(initialBPM, effectiveLinkPeerName()),
      mJackClientUUID(0),
      mSyncLink(syncLink),
      mWasSyncLink(syncLink),
      mConfigPath(std::move(configPath)),
      mSinkNames(std::move(sinkNames)),
      mSourceNames(std::move(sourceNames)) {
  // setup listener

  mCaptureLatencyTrimMs.store(captureLatencyTrimMs, std::memory_order_release);
  mPlaybackLatencyTrimMs.store(playbackLatencyTrimMs, std::memory_order_release);

  // setup link
  mLink.setTempoCallback([this](double bpm) {
    mLinkBPM = bpm;
    if (mSyncLink) {
      mBPM.store(bpm, std::memory_order_release);
      mReportBPM = true;
    }
  });
  if (enableStartStopSync) {
    mLink.setStartStopCallback([this](bool isPlaying) {
      if (mLink.isStartStopSyncEnabled() && mSyncLink) {
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
  mLink.enable(true);

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
      setSyncProperty(mSyncLink);
      setNumPeersProperty(mLink.numPeers());
      // publish the effective Link peer name (always non-empty: override or hostname)
      mAppliedLinkPeerName = effectiveLinkPeerName();
      setLinkAudioPeerNameProperty();
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
    mSinkNames.resize(mNumStereoInChannels);
    mSourceNames.resize(mNumStereoOutChannels);
    mAppliedSinkNames.resize(mNumStereoInChannels);
    for (size_t i = 0; i < mNumStereoInChannels; ++i) {
      mAppliedSinkNames[i] = effectiveSinkName(i);
      mSendRenderers.push_back(std::make_unique<SinkRenderer>(
          mLink, mAppliedSinkNames[i], 2, mSampleRate));
    }
    for (size_t i = 0; i < mNumStereoOutChannels; ++i)
      mRecvRenderers.push_back(std::make_unique<SourceRenderer>(
          mLink, 2, mSampleRate));

    mCurrentSourceChannelIds.resize(mNumStereoOutChannels);
    mCurrentSourcePeerNames.resize(mNumStereoOutChannels);
    mCurrentSourceChannelNames.resize(mNumStereoOutChannels);
    mLinkAudioPeerFilters.resize(mNumStereoOutChannels);
    mLinkAudioChannelFilters.resize(mNumStereoOutChannels);

    jack_nframes_t bufSize = jack_get_buffer_size(mJackClient);
    mStereoSendBuf.resize(mNumStereoInChannels * 2 * bufSize);
    mStereoRecvBuf.resize(mNumStereoOutChannels * 2 * bufSize);

    jack_set_buffer_size_callback(mJackClient,
                                  JackTransportLink::bufferSizeCallback, this);

    mAudioIns.resize(2 * mNumStereoInChannels);
    mAudioOuts.resize(2 * mNumStereoOutChannels);
    for (size_t i = 0; i < mNumStereoInChannels; ++i) {
      mAudioIns[i * 2]     = jack_port_register(mJackClient,
          ("in_" + std::to_string(i * 2 + 1)).c_str(),
          JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput | JackPortIsTerminal, 0);
      mAudioIns[i * 2 + 1] = jack_port_register(mJackClient,
          ("in_" + std::to_string(i * 2 + 2)).c_str(),
          JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput | JackPortIsTerminal, 0);
    }
    for (size_t i = 0; i < mNumStereoOutChannels; ++i) {
      mAudioOuts[i * 2]     = jack_port_register(mJackClient,
          ("out_" + std::to_string(i * 2 + 1)).c_str(),
          JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput | JackPortIsTerminal, 0);
      mAudioOuts[i * 2 + 1] = jack_port_register(mJackClient,
          ("out_" + std::to_string(i * 2 + 2)).c_str(),
          JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput | JackPortIsTerminal, 0);
    }

    mLink.enableLinkAudio(true);
    mLink.setChannelsChangedCallback([this]() {
      mChannelsChanged.store(true, std::memory_order_release);
    });
  }
  if (mLinkAudioEnabled && !jack_uuid_empty(mJackClientUUID)) {
    setLinkAudioSourceProperty();
    setLinkAudioSourceFiltersProperty();
    setLinkAudioChannelsProperty({});
    setLinkAudioInStereoChannelsProperty(mNumStereoInChannels);
    setLinkAudioOutStereoChannelsProperty(mNumStereoOutChannels);
    for (size_t i = 0; i < mNumStereoInChannels; ++i)
      setLinkAudioSinkNameProperty(i);
    for (size_t i = 0; i < mNumStereoOutChannels; ++i)
      setLinkAudioSourceNameProperty(i);
  }
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
  // Seed effective latency + publish read-backs now (auto values arrive via the latency callback
  // once ports are connected; until then effective = user trim only).
  recomputeEffectiveLatency();
}

JackTransportLink::~JackTransportLink() {
  // flush any config change that the debounce in processEvents hasn't written yet,
  // so a clean exit (e.g. Ctrl-C) doesn't lose recent changes
  if (mNeedsSaveConfig)
    saveConfig();
  jack_set_sync_callback(mJackClient, nullptr, nullptr);
  jack_release_timebase(mJackClient);
  jack_deactivate(mJackClient);
  jack_client_close(mJackClient);
}

int JackTransportLink::bufferSizeCallback(jack_nframes_t nframes, void *arg) {
  return static_cast<JackTransportLink *>(arg)->bufferSizeCallback(nframes);
}

int JackTransportLink::bufferSizeCallback(jack_nframes_t nframes) {
  mStereoSendBuf.resize(mNumStereoInChannels * 2 * nframes);
  mStereoRecvBuf.resize(mNumStereoOutChannels * 2 * nframes);
  return 0;
}

void JackTransportLink::latencyCallback(jack_latency_callback_mode_t, void *arg) {
  // Runs on JACK's notification thread. Reading the port latency ranges here would race with
  // rebuildAudioPorts() mutating the port vectors on the main thread (and jack_set_property is
  // illegal from this thread), so just flag it; processEvents does the read + republish.
  static_cast<JackTransportLink *>(arg)->mNeedsRecomputeLatency.store(
      true, std::memory_order_release);
}

void JackTransportLink::updateLatencyRanges() {
  // Main-thread (serialized with rebuildAudioPorts). We are a terminal client and add no latency
  // of our own, so we don't declare any; we only read the systemic latency JACK has computed for
  // our ports. Capture latency of an in_N port = how long ago its audio was captured (send
  // offset); playback latency of an out_N port = how long until its audio is heard (recv offset).
  auto maxLatency = [](const std::vector<jack_port_t *> &ports,
                       jack_latency_callback_mode_t m) -> jack_nframes_t {
    jack_nframes_t maxFrames = 0;
    for (auto *p : ports) {
      if (!p) continue;
      jack_latency_range_t range;
      jack_port_get_latency_range(p, m, &range);
      maxFrames = std::max(maxFrames, range.max);
    }
    return maxFrames;
  };
  mAutoCaptureLatencyFrames.store(maxLatency(mAudioIns, JackCaptureLatency),
                                  std::memory_order_release);
  mAutoPlaybackLatencyFrames.store(maxLatency(mAudioOuts, JackPlaybackLatency),
                                   std::memory_order_release);
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
  {
    int reqIn  = mRequestedStereoInChannels.exchange(-1, std::memory_order_acq_rel);
    int reqOut = mRequestedStereoOutChannels.exchange(-1, std::memory_order_acq_rel);
    if (reqIn >= 0 || reqOut >= 0) {
      size_t newIn  = reqIn  >= 0 ? static_cast<size_t>(reqIn)  : mNumStereoInChannels;
      size_t newOut = reqOut >= 0 ? static_cast<size_t>(reqOut) : mNumStereoOutChannels;
      rebuildAudioPorts(newIn, newOut);
    }
  }
  if (mLinkAudioEnabled) {
    if (mNeedsApplySinkNames.exchange(false, std::memory_order_acq_rel)) {
      applySinkNames();
    }
    if (mChannelsChanged.exchange(false, std::memory_order_acq_rel)) {
      if (updateLinkAudioSource()) { mReportLinkAudioSource = true; mUpdatePortMeta = true; }
      mReportLinkAudioChannels = true;
    }
    if (mNeedsSourceUpdate.exchange(false, std::memory_order_acq_rel)) {
      if (updateLinkAudioSource()) { mReportLinkAudioSource = true; mUpdatePortMeta = true; }
    }
    if (mReportLinkAudioChannels) {
      mReportLinkAudioChannels = false;
      setLinkAudioChannelsProperty(mLink.channels());
    }
    if (mReportLinkAudioSource) {
      mReportLinkAudioSource = false;
      setLinkAudioSourceProperty();
    }
    if (mReportLinkAudioSourceFilters) {
      mReportLinkAudioSourceFilters = false;
      setLinkAudioSourceFiltersProperty();
      // the configured filter is the fallback source label, so refresh port names too
      mUpdatePortMeta = true;
    }
    if (mUpdatePortMeta) {
      mUpdatePortMeta = false;
      updateAudioPortMetadata();
    }
  }
  // Link peer name applies regardless of Link Audio: Link itself is always enabled.
  if (mNeedsApplyPeerName.exchange(false, std::memory_order_acq_rel)) {
    applyLinkPeerName();
  }
  if (mNeedsRecomputeLatency.exchange(false, std::memory_order_acq_rel)) {
    updateLatencyRanges();
  }
  if (mReportBPM) {
    mReportBPM = false;
    setBPMProperty(mBPM.load(std::memory_order_acquire));
  }
  if (mReportLinkSync) {
    mReportLinkSync = false;
    setSyncProperty(mSyncLink);
  }
  if (mReportStartStopEnable) {
    mReportStartStopEnable = false;
    setEnableStartStopProperty(mLink.isStartStopSyncEnabled());
  }
  if (mNeedsSaveConfig) {
    auto now = std::chrono::steady_clock::now();
    if (now - mLastConfigSave >= std::chrono::seconds(1)) {
      mNeedsSaveConfig = false;
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
  if (mSyncLink != mWasSyncLink) {
    mWasSyncLink = mSyncLink;
    if (mSyncLink) {
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
  if (mSyncLink && (stateChange || bpmChange || beatrequest >= 0.0)) {
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

  if (mLinkAudioEnabled && (!mSendRenderers.empty() || !mRecvRenderers.empty())) {
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

    for (size_t i = 0; i < mNumStereoInChannels; ++i) {
      double* sendPtrs[2];
      for (size_t ch = 0; ch < 2; ++ch) {
        sendPtrs[ch] = mStereoSendBuf.data() + (i * 2 + ch) * nframes;
        const auto *inBuf = static_cast<const float *>(
            jack_port_get_buffer(mAudioIns[i * 2 + ch], nframes));
        for (jack_nframes_t f = 0; f < nframes; ++f)
          sendPtrs[ch][f] = static_cast<double>(inBuf[f]);
      }
      mSendRenderers[i]->send(sendPtrs, nframes, sessionState,
                              mSampleRate, sendHostTime, mQuantum);
    }

    for (size_t i = 0; i < mNumStereoOutChannels; ++i) {
      double* recvPtrs[2];
      for (size_t ch = 0; ch < 2; ++ch)
        recvPtrs[ch] = mStereoRecvBuf.data() + (i * 2 + ch) * nframes;
      mRecvRenderers[i]->receive(recvPtrs, nframes, sessionState,
                                 mSampleRate, recvHostTime, mQuantum);
      for (size_t ch = 0; ch < 2; ++ch) {
        auto *outBuf = static_cast<float *>(
            jack_port_get_buffer(mAudioOuts[i * 2 + ch], nframes));
        for (jack_nframes_t f = 0; f < nframes; ++f)
          outBuf[f] = static_cast<float>(recvPtrs[ch][f]);
      }
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
  auto sync = mSyncLink;

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

  // what if quantum changes? Does link keep track of that or should we compute
  // bar some other way?
  auto bar = std::floor(mInternalBeat / mQuantum);
  auto beat = std::fmod(mInternalBeat, mQuantum);
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
  // if the subject is all or us and the key is all (empty) or bpm
  if ((jack_uuid_empty(subject) || subject == mJackClientUUID)) {
    bool isbpm = !key || bpm_key.compare(key) == 0;
    bool islinksync = !key || linksync_key.compare(key) == 0;
    bool isenable = !key || start_stop_key.compare(key) == 0;
    bool islinkaudiosource = !key || linkaudio_source_key.compare(key) == 0;
    bool ispeername    = !key || linkaudio_peer_name_key.compare(key) == 0;
    bool is_in_stereo  = !key || linkaudio_in_stereo_key.compare(key) == 0;
    bool is_out_stereo = !key || linkaudio_out_stereo_key.compare(key) == 0;
    int  linkaudiosourceidx = key ? linkAudioSourceIndex(key) : -1;
    int  srcNameIdx  = key ? linkAudioNameIndex(key, linkaudio_source_key) : -1;
    int  sinkNameIdx = key ? linkAudioNameIndex(key, linkaudio_sink_key) : -1;
    // Treat a newly-created property the same as a changed one. Properties that
    // jack_transport_link doesn't publish itself (e.g. the optional slot-name keys,
    // which are absent until first set) arrive as PropertyCreated, not PropertyChanged.
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

        bool was = mSyncLink;
        mSyncLink = std::find(true_values.begin(), true_values.end(), values) !=
                    true_values.end();

        if (mSyncLink && !was) {
          mBPM.store(mLinkBPM, std::memory_order_release);
          mReportBPM = true;
        }
        mNeedsSaveConfig = true;
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
      } else if (mLinkAudioEnabled && islinkaudiosource &&
                 get_property(mJackClientUUID, linkaudio_source_key, values, types)) {
        bool ok;
        {
          std::lock_guard<std::mutex> lock(mSourceFilterMutex);
          ok = parseLinkAudioSourceFilters(values, mLinkAudioPeerFilters, mLinkAudioChannelFilters);
        }
        if (ok) {
          mNeedsSourceUpdate.store(true, std::memory_order_release);
          mReportLinkAudioSourceFilters = true;
          mNeedsSaveConfig = true;
        }
      } else if (mLinkAudioEnabled && linkaudiosourceidx >= 0
                 && static_cast<size_t>(linkaudiosourceidx) < mRecvRenderers.size()) {
        const auto indexKey = linkaudio_source_key + "/" + std::to_string(linkaudiosourceidx);
        if (get_property(mJackClientUUID, indexKey, values, types)) {
          try {
            auto j = nlohmann::json::parse(values);
            if (j.is_object()) {
              const size_t i = static_cast<size_t>(linkaudiosourceidx);
              {
                std::lock_guard<std::mutex> lock(mSourceFilterMutex);
                mLinkAudioPeerFilters[i]    = j.value("peer",    "");
                mLinkAudioChannelFilters[i] = j.value("channel", "");
              }
              mNeedsSourceUpdate.store(true, std::memory_order_release);
              mReportLinkAudioSourceFilters = true;
              mNeedsSaveConfig = true;
            }
          } catch (...) {}
        }
      } else if (mLinkAudioEnabled && srcNameIdx >= 0
                 && static_cast<size_t>(srcNameIdx) < mSourceNames.size()) {
        const auto nameKey = linkaudio_source_key + "/" + std::to_string(srcNameIdx) + "/name";
        if (get_property(mJackClientUUID, nameKey, values, types)) {
          mSourceNames[static_cast<size_t>(srcNameIdx)] = values;
          mNeedsSaveConfig = true;
        }
      } else if (mLinkAudioEnabled && sinkNameIdx >= 0
                 && static_cast<size_t>(sinkNameIdx) < mSinkNames.size()) {
        const auto nameKey = linkaudio_sink_key + "/" + std::to_string(sinkNameIdx) + "/name";
        if (get_property(mJackClientUUID, nameKey, values, types)) {
          mSinkNames[static_cast<size_t>(sinkNameIdx)] = values;
          mNeedsApplySinkNames.store(true, std::memory_order_release);
          mNeedsSaveConfig = true;
        }
      } else if (mLinkAudioEnabled && is_in_stereo &&
                 get_property(mJackClientUUID, linkaudio_in_stereo_key, values, types)) {
        char* end;
        errno = 0;
        long n = std::strtol(values.c_str(), &end, 10);
        if (*end == '\0' && errno == 0 && n >= 0 && n <= kMaxLinkAudioStereoPairs)
          mRequestedStereoInChannels.store(static_cast<int>(n));
      } else if (mLinkAudioEnabled && is_out_stereo &&
                 get_property(mJackClientUUID, linkaudio_out_stereo_key, values, types)) {
        char* end;
        errno = 0;
        long n = std::strtol(values.c_str(), &end, 10);
        if (*end == '\0' && errno == 0 && n >= 0 && n <= kMaxLinkAudioStereoPairs)
          mRequestedStereoOutChannels.store(static_cast<int>(n));
      }
    } else if (change == jack_property_change_t::PropertyDeleted) {
      if (isbpm)
        mReportBPM = true;
      if (isenable)
        mReportStartStopEnable = true;
      if (islinksync)
        mReportLinkSync = true;
      if (ispeername) {
        // clearing the property reverts to auto (hostname); republish the effective name
        mLinkPeerName.clear();
        mNeedsApplyPeerName.store(true, std::memory_order_release);
        mNeedsSaveConfig = true;
      }
      if (mLinkAudioEnabled && linkaudiosourceidx >= 0
          && static_cast<size_t>(linkaudiosourceidx) < mRecvRenderers.size()) {
        const size_t i = static_cast<size_t>(linkaudiosourceidx);
        {
          std::lock_guard<std::mutex> lock(mSourceFilterMutex);
          mLinkAudioPeerFilters[i]    = {};
          mLinkAudioChannelFilters[i] = {};
        }
        mNeedsSourceUpdate.store(true, std::memory_order_release);
        mReportLinkAudioSourceFilters = true;
      }
      if (mLinkAudioEnabled && srcNameIdx >= 0
          && static_cast<size_t>(srcNameIdx) < mSourceNames.size()) {
        mSourceNames[static_cast<size_t>(srcNameIdx)] = {};
        mNeedsSaveConfig = true;
      }
      if (mLinkAudioEnabled && sinkNameIdx >= 0
          && static_cast<size_t>(sinkNameIdx) < mSinkNames.size()) {
        mSinkNames[static_cast<size_t>(sinkNameIdx)] = {};
        mNeedsApplySinkNames.store(true, std::memory_order_release);
        mNeedsSaveConfig = true;
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

// Publish the currently-connected peer/channel for each source as an index-aligned
// array on the dedicated status key. The source key itself is left as a write-only
// desired-filter target (written by clients, parsed in propertyChangeCallback) so we
// never re-ingest our own status as a filter.
void JackTransportLink::setLinkAudioSourceProperty() {
  if (jack_uuid_empty(mJackClientUUID)) return;
  nlohmann::json arr = nlohmann::json::array();
  for (size_t i = 0; i < mRecvRenderers.size(); ++i) {
    nlohmann::json entry = nlohmann::json::object();
    if (mCurrentSourceChannelIds[i].has_value()) {
      entry = {{"peer", mCurrentSourcePeerNames[i]},
               {"channel", mCurrentSourceChannelNames[i]}};
    }
    arr.push_back(entry);
  }
  jack_set_property(mJackClient, mJackClientUUID, linkaudio_source_status_key.c_str(),
                    arr.dump().c_str(), "application/json");
}

// Read-only reflection of the *configured* per-source filters (distinct from the
// current-connection status published by setLinkAudioSourceProperty). Lets readers
// see what was requested even when no matching peer is online.
void JackTransportLink::setLinkAudioSourceFiltersProperty() {
  if (jack_uuid_empty(mJackClientUUID)) return;
  nlohmann::json arr = nlohmann::json::array();
  {
    std::lock_guard<std::mutex> lock(mSourceFilterMutex);
    for (size_t i = 0; i < mLinkAudioPeerFilters.size(); ++i) {
      nlohmann::json entry = nlohmann::json::object();
      if (!mLinkAudioPeerFilters[i].empty())    entry["peer"]    = mLinkAudioPeerFilters[i];
      if (!mLinkAudioChannelFilters[i].empty()) entry["channel"] = mLinkAudioChannelFilters[i];
      arr.push_back(entry);
    }
  }
  jack_set_property(mJackClient, mJackClientUUID, linkaudio_source_filters_key.c_str(),
                    arr.dump().c_str(), "application/json");
}

std::string JackTransportLink::effectiveSinkName(size_t i) const {
  if (i < mSinkNames.size() && !mSinkNames[i].empty())
    return mSinkNames[i];
  return "Send " + std::to_string(i + 1);
}

// Effective Link peer name: the user override if set, otherwise the device hostname.
// Link truncates names beyond 256 chars; the hostname buffer bounds us well under that.
std::string JackTransportLink::effectiveLinkPeerName() const {
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

// jackd logs a "DB_NOTFOUND" error when asked to remove a property that was never set.
// The per-slot filter/name keys are only ever written by clients (e.g. the runner), so a
// slot left untouched has no property; guard removal on existence to avoid that log spam.
void JackTransportLink::removePropertyIfExists(const std::string& key) {
  if (jack_uuid_empty(mJackClientUUID)) return;
  std::string v, t;
  if (get_property(mJackClientUUID, key, v, t))
    jack_remove_property(mJackClient, mJackClientUUID, key.c_str());
}

// JACK disallows empty metadata values, so "no custom name" is represented by the
// property being absent.
void JackTransportLink::setLinkAudioSlotNameProperty(const std::string& key, const std::string& name) {
  if (jack_uuid_empty(mJackClientUUID)) return;
  if (name.empty()) {
    removePropertyIfExists(key);
  } else {
    jack_set_property(mJackClient, mJackClientUUID, key.c_str(), name.c_str(), string_type);
  }
}

void JackTransportLink::setLinkAudioSinkNameProperty(size_t i) {
  if (i >= mSinkNames.size()) return;
  setLinkAudioSlotNameProperty(linkaudio_sink_key + "/" + std::to_string(i) + "/name", mSinkNames[i]);
}

void JackTransportLink::setLinkAudioSourceNameProperty(size_t i) {
  if (i >= mSourceNames.size()) return;
  setLinkAudioSlotNameProperty(linkaudio_source_key + "/" + std::to_string(i) + "/name", mSourceNames[i]);
}

// Display label for an incoming (source) stereo pair: the currently-connected
// peer/channel, else the configured filter, else a generic default.
std::string JackTransportLink::linkAudioSourcePortLabel(size_t i) {
  auto join = [](const std::string& peer, const std::string& channel) -> std::string {
    if (peer.size() && channel.size()) return peer + ": " + channel;
    return peer + channel;
  };
  if (i < mCurrentSourceChannelIds.size() && mCurrentSourceChannelIds[i].has_value()) {
    auto label = join(mCurrentSourcePeerNames[i], mCurrentSourceChannelNames[i]);
    if (label.size()) return label;
  }
  {
    std::lock_guard<std::mutex> lock(mSourceFilterMutex);
    if (i < mLinkAudioPeerFilters.size()) {
      auto label = join(mLinkAudioPeerFilters[i], mLinkAudioChannelFilters[i]);
      if (label.size()) return label;
    }
  }
  return "Link In " + std::to_string(i + 1);
}

// Set the port-group + pretty-name on our own audio ports so patchbays present them
// as a "jack-link-audio" node: sinks (in_N) by their announced name, sources (out_N)
// by the connected peer/channel. Only writes on change to avoid notification churn.
// JACK removes this metadata automatically when the ports are unregistered.
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
  // order is a stable per-port index so patchbays keep in_N/out_N in slot order
  auto decorate = [&](jack_port_t* port, const std::string& pretty, int order) {
    setIfChanged(port, port_group_key, link_audio_port_group, string_type);
    setIfChanged(port, pretty_name_key, pretty, string_type);
    setIfChanged(port, order_key, std::to_string(order), order_type);
  };
  for (size_t i = 0; i < mNumStereoInChannels && (i * 2 + 1) < mAudioIns.size(); ++i) {
    const auto base = effectiveSinkName(i);
    decorate(mAudioIns[i * 2],     base + " L", static_cast<int>(i * 2 + 1));
    decorate(mAudioIns[i * 2 + 1], base + " R", static_cast<int>(i * 2 + 2));
  }
  for (size_t i = 0; i < mNumStereoOutChannels && (i * 2 + 1) < mAudioOuts.size(); ++i) {
    const auto base = linkAudioSourcePortLabel(i);
    decorate(mAudioOuts[i * 2],     base + " L", static_cast<int>(i * 2 + 1));
    decorate(mAudioOuts[i * 2 + 1], base + " R", static_cast<int>(i * 2 + 2));
  }
}

// Recreate any SinkRenderer whose announced name no longer matches its slot's
// effective name. Recreating a renderer races the RT process callback, so this
// mirrors rebuildAudioPorts: deactivate, swap, reactivate, restore connections.
void JackTransportLink::applySinkNames() {
  if (!mLinkAudioEnabled) return;
  mAppliedSinkNames.resize(mSendRenderers.size());
  bool any = false;
  for (size_t i = 0; i < mSendRenderers.size(); ++i)
    if (effectiveSinkName(i) != mAppliedSinkNames[i]) { any = true; break; }
  if (!any) return;

  // jack_deactivate drops ALL of our ports' connections, not just the sinks we're
  // recreating. Snapshot every connection (inputs, outputs, clock) and restore them,
  // otherwise e.g. renaming a sink would silently drop the source (out_N) routing.
  struct Conn { std::string mine; std::string other; bool output; };
  std::vector<Conn> conns;
  auto snapshot = [&conns](jack_port_t* port, bool output) {
    if (!port) return;
    const char** c = jack_port_get_connections(port);
    if (c) {
      const std::string mine = jack_port_name(port);
      for (const char** p = c; *p; ++p) conns.push_back({mine, *p, output});
      jack_free(c);
    }
  };
  for (auto* p : mAudioIns)  snapshot(p, false);
  for (auto* p : mAudioOuts) snapshot(p, true);
  snapshot(mMIDIClockOut, true);

  jack_deactivate(mJackClient);
  for (size_t i = 0; i < mSendRenderers.size(); ++i) {
    const auto name = effectiveSinkName(i);
    if (name != mAppliedSinkNames[i]) {
      mSendRenderers[i] = std::make_unique<SinkRenderer>(mLink, name, 2, mSampleRate);
      mAppliedSinkNames[i] = name;
    }
  }
  jack_activate(mJackClient);

  for (const auto& c : conns) {
    if (c.output)
      jack_connect(mJackClient, c.mine.c_str(), c.other.c_str());
    else
      jack_connect(mJackClient, c.other.c_str(), c.mine.c_str());
  }

  mUpdatePortMeta = true;
}

static std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), ::tolower);
  return s;
}

bool JackTransportLink::updateLinkAudioSource() {
  auto channels = mLink.channels();
  bool anyChanged = false;

  // Copy filters under the lock so we don't hold it during the channel search.
  std::vector<std::string> peerFilters, channelFilters;
  {
    std::lock_guard<std::mutex> lock(mSourceFilterMutex);
    peerFilters    = mLinkAudioPeerFilters;
    channelFilters = mLinkAudioChannelFilters;
  }

  // channels already taken by a source this pass, so auto (unfiltered) sources don't
  // all collapse onto the same first-available channel
  std::vector<ableton::ChannelId> claimed;
  auto isClaimed = [&claimed](const ableton::ChannelId& id) {
    return std::find(claimed.begin(), claimed.end(), id) != claimed.end();
  };

  for (size_t i = 0; i < mRecvRenderers.size(); ++i) {
    const auto peerNeedle    = toLower(peerFilters[i]);
    const auto channelNeedle = toLower(channelFilters[i]);
    const bool isAuto = peerNeedle.empty() && channelNeedle.empty();

    std::optional<ableton::LinkAudio::Channel> target;
    for (const auto& ch : channels) {
      bool peerMatch    = peerNeedle.empty()
                          || toLower(ch.peerName).find(peerNeedle) != std::string::npos;
      bool channelMatch = channelNeedle.empty()
                          || toLower(ch.name).find(channelNeedle) != std::string::npos;
      // an auto source skips channels another source has already claimed
      if (peerMatch && channelMatch && !(isAuto && isClaimed(ch.id))) {
        target = ch;
        break;
      }
    }

    if (target) {
      claimed.push_back(target->id);
      bool needSwitch = !mRecvRenderers[i]->hasSource()
          || !mCurrentSourceChannelIds[i].has_value()
          || (*mCurrentSourceChannelIds[i] != target->id);
      if (needSwitch) {
        mRecvRenderers[i]->removeSource();
        mRecvRenderers[i]->createSource(target->id);
        mCurrentSourceChannelIds[i]   = target->id;
        mCurrentSourcePeerNames[i]    = target->peerName;
        mCurrentSourceChannelNames[i] = target->name;
        anyChanged = true;
      }
    } else {
      if (mRecvRenderers[i]->hasSource()) {
        mRecvRenderers[i]->removeSource();
        mCurrentSourceChannelIds[i]   = std::nullopt;
        mCurrentSourcePeerNames[i]    = {};
        mCurrentSourceChannelNames[i] = {};
        anyChanged = true;
      }
    }
  }
  return anyChanged;
}

void JackTransportLink::invalidateClockSyncBBT() {
  mBeatLast = -1;
  mBarLast = -1;
  mTickLast = -1.0;
}

void JackTransportLink::ProcessMessage(
    const oscpack::ReceivedMessage &m,
    const oscpack::IpEndpointName &remoteEndpoint) {
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
        bool was = mSyncLink;
        mSyncLink = arg->AsBoolUnchecked();
        if (mSyncLink && !was) {
          mBPM.store(mLinkBPM, std::memory_order_release);
          mReportBPM = true;
        }
        setSyncProperty(mSyncLink);
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
    } else if (std::strcmp("/jacklink/linkaudio/peer-name", m.AddressPattern()) == 0) {
      // Non-empty sets the override; empty string clears it (reverts to hostname).
      // Unlike JACK metadata, OSC can carry an empty string, so we handle it here.
      if (arg != m.ArgumentsEnd() && arg->IsString()) {
        mLinkPeerName = arg->AsStringUnchecked();
        mNeedsApplyPeerName.store(true, std::memory_order_release);
        mNeedsSaveConfig = true;
      }
    } else if (mLinkAudioEnabled &&
               std::strcmp("/jacklink/linkaudio/source", m.AddressPattern()) == 0) {
      // pairs: peer0 channel0 peer1 channel1 ...
      bool changed = false;
      {
        std::lock_guard<std::mutex> lock(mSourceFilterMutex);
        for (size_t i = 0; i < mRecvRenderers.size() && arg != m.ArgumentsEnd(); ++i) {
          if (!arg->IsString()) break;
          mLinkAudioPeerFilters[i] = arg->AsStringUnchecked();
          ++arg;
          mLinkAudioChannelFilters[i] = (arg != m.ArgumentsEnd() && arg->IsString())
              ? (arg++)->AsStringUnchecked() : "";
          changed = true;
        }
      }
      if (changed) {
        mNeedsSourceUpdate.store(true, std::memory_order_release);
        mReportLinkAudioSourceFilters = true;
        mNeedsSaveConfig = true;
      }
    } else if (mLinkAudioEnabled
               && parseOscNameIndex(m.AddressPattern(), "/jacklink/linkaudio/source/", "/name") >= 0) {
      long idx = parseOscNameIndex(m.AddressPattern(), "/jacklink/linkaudio/source/", "/name");
      if (static_cast<size_t>(idx) < mSourceNames.size()
          && arg != m.ArgumentsEnd() && arg->IsString()) {
        mSourceNames[static_cast<size_t>(idx)] = arg->AsStringUnchecked();
        setLinkAudioSourceNameProperty(static_cast<size_t>(idx));
        mNeedsSaveConfig = true;
      }
    } else if (mLinkAudioEnabled
               && parseOscNameIndex(m.AddressPattern(), "/jacklink/linkaudio/sink/", "/name") >= 0) {
      long idx = parseOscNameIndex(m.AddressPattern(), "/jacklink/linkaudio/sink/", "/name");
      if (static_cast<size_t>(idx) < mSinkNames.size()
          && arg != m.ArgumentsEnd() && arg->IsString()) {
        mSinkNames[static_cast<size_t>(idx)] = arg->AsStringUnchecked();
        setLinkAudioSinkNameProperty(static_cast<size_t>(idx));
        mNeedsApplySinkNames.store(true, std::memory_order_release);
        mNeedsSaveConfig = true;
      }
    } else if (mLinkAudioEnabled
               && std::strncmp("/jacklink/linkaudio/source/", m.AddressPattern(),
                                sizeof("/jacklink/linkaudio/source/") - 1) == 0) {
      const char* idxStr = m.AddressPattern() + sizeof("/jacklink/linkaudio/source/") - 1;
      char* end;
      long idx = std::strtol(idxStr, &end, 10);
      if (*end == '\0' && idx >= 0
          && static_cast<size_t>(idx) < mRecvRenderers.size()
          && arg != m.ArgumentsEnd() && arg->IsString()) {
        const size_t i = static_cast<size_t>(idx);
        {
          std::lock_guard<std::mutex> lock(mSourceFilterMutex);
          mLinkAudioPeerFilters[i] = arg->AsStringUnchecked();
          ++arg;
          mLinkAudioChannelFilters[i] = (arg != m.ArgumentsEnd() && arg->IsString())
              ? arg->AsStringUnchecked() : "";
        }
        mNeedsSourceUpdate.store(true, std::memory_order_release);
        mReportLinkAudioSourceFilters = true;
        mNeedsSaveConfig = true;
      }
    } else if (mLinkAudioEnabled &&
               std::strcmp("/jacklink/linkaudio/in-stereo-channels", m.AddressPattern()) == 0) {
      if (arg != m.ArgumentsEnd()) {
        auto v = GetOscDouble(*arg);
        if (v && *v >= 0.0 && *v <= kMaxLinkAudioStereoPairs)
          mRequestedStereoInChannels.store(static_cast<int>(*v));
      }
    } else if (mLinkAudioEnabled &&
               std::strcmp("/jacklink/linkaudio/out-stereo-channels", m.AddressPattern()) == 0) {
      if (arg != m.ArgumentsEnd()) {
        auto v = GetOscDouble(*arg);
        if (v && *v >= 0.0 && *v <= kMaxLinkAudioStereoPairs)
          mRequestedStereoOutChannels.store(static_cast<int>(*v));
      }
    }
  } catch (oscpack::Exception &e) {
    std::cerr << "error while parsing message: " << m.AddressPattern() << ": "
              << e.what() << "\n";
  }
}

void JackTransportLink::setLinkAudioInStereoChannelsProperty(size_t n) {
  if (!jack_uuid_empty(mJackClientUUID)) {
    std::string s = std::to_string(n);
    jack_set_property(mJackClient, mJackClientUUID, linkaudio_in_stereo_key.c_str(),
                      s.c_str(), int_type);
  }
}

void JackTransportLink::setLinkAudioOutStereoChannelsProperty(size_t n) {
  if (!jack_uuid_empty(mJackClientUUID)) {
    std::string s = std::to_string(n);
    jack_set_property(mJackClient, mJackClientUUID, linkaudio_out_stereo_key.c_str(),
                      s.c_str(), int_type);
  }
}

void JackTransportLink::rebuildAudioPorts(size_t newIn, size_t newOut) {
  if (!mLinkAudioEnabled) return;
  if (newIn == mNumStereoInChannels && newOut == mNumStereoOutChannels) return;

  const size_t oldIn  = mNumStereoInChannels;
  const size_t oldOut = mNumStereoOutChannels;

  // Save connections for ports that will survive
  std::map<size_t, std::vector<std::string>> inConns, outConns;
  for (size_t i = 0; i < std::min(newIn, oldIn) * 2; ++i) {
    if (mAudioIns[i]) {
      const char** conns = jack_port_get_connections(mAudioIns[i]);
      if (conns) {
        for (const char** c = conns; *c; ++c) inConns[i].push_back(*c);
        jack_free(conns);
      }
    }
  }
  for (size_t i = 0; i < std::min(newOut, oldOut) * 2; ++i) {
    if (mAudioOuts[i]) {
      const char** conns = jack_port_get_connections(mAudioOuts[i]);
      if (conns) {
        for (const char** c = conns; *c; ++c) outConns[i].push_back(*c);
        jack_free(conns);
      }
    }
  }

  jack_deactivate(mJackClient);

  // Shrink send side
  for (size_t i = newIn; i < oldIn; ++i) {
    jack_port_unregister(mJackClient, mAudioIns[i * 2]);
    jack_port_unregister(mJackClient, mAudioIns[i * 2 + 1]);
  }
  if (newIn < oldIn) {
    mAudioIns.resize(2 * newIn);
    mSendRenderers.resize(newIn);
  }

  // Shrink recv side
  for (size_t i = newOut; i < oldOut; ++i) {
    mRecvRenderers[i]->removeSource();
    jack_port_unregister(mJackClient, mAudioOuts[i * 2]);
    jack_port_unregister(mJackClient, mAudioOuts[i * 2 + 1]);
  }
  if (newOut < oldOut) {
    mAudioOuts.resize(2 * newOut);
    mRecvRenderers.resize(newOut);
  }

  // Resize per-receiver tracking vectors
  {
    std::lock_guard<std::mutex> lock(mSourceFilterMutex);
    mLinkAudioPeerFilters.resize(newOut);
    mLinkAudioChannelFilters.resize(newOut);
  }
  mCurrentSourceChannelIds.resize(newOut);
  mCurrentSourcePeerNames.resize(newOut);
  mCurrentSourceChannelNames.resize(newOut);

  // Resize per-slot name vectors (preserving surviving slots' names)
  mSinkNames.resize(newIn);
  mAppliedSinkNames.resize(newIn);
  mSourceNames.resize(newOut);

  // Grow send side
  for (size_t i = oldIn; i < newIn; ++i) {
    mAppliedSinkNames[i] = effectiveSinkName(i);
    mSendRenderers.push_back(std::make_unique<SinkRenderer>(
        mLink, mAppliedSinkNames[i], 2, mSampleRate));
    mAudioIns.push_back(jack_port_register(mJackClient,
        ("in_" + std::to_string(i * 2 + 1)).c_str(),
        JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput | JackPortIsTerminal, 0));
    mAudioIns.push_back(jack_port_register(mJackClient,
        ("in_" + std::to_string(i * 2 + 2)).c_str(),
        JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput | JackPortIsTerminal, 0));
  }

  // Grow recv side
  for (size_t i = oldOut; i < newOut; ++i) {
    mRecvRenderers.push_back(std::make_unique<SourceRenderer>(mLink, 2, mSampleRate));
    mAudioOuts.push_back(jack_port_register(mJackClient,
        ("out_" + std::to_string(i * 2 + 1)).c_str(),
        JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput | JackPortIsTerminal, 0));
    mAudioOuts.push_back(jack_port_register(mJackClient,
        ("out_" + std::to_string(i * 2 + 2)).c_str(),
        JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput | JackPortIsTerminal, 0));
  }

  mNumStereoInChannels  = newIn;
  mNumStereoOutChannels = newOut;

  jack_nframes_t bufSize = jack_get_buffer_size(mJackClient);
  mStereoSendBuf.resize(newIn  * 2 * bufSize);
  mStereoRecvBuf.resize(newOut * 2 * bufSize);

  jack_activate(mJackClient);

  // Restore saved connections
  for (auto& [idx, names] : inConns)
    for (const auto& name : names)
      jack_connect(mJackClient, name.c_str(), jack_port_name(mAudioIns[idx]));
  for (auto& [idx, names] : outConns)
    for (const auto& name : names)
      jack_connect(mJackClient, jack_port_name(mAudioOuts[idx]), name.c_str());

  if (newOut > oldOut)
    mNeedsSourceUpdate.store(true, std::memory_order_release);

  if (!jack_uuid_empty(mJackClientUUID)) {
    for (size_t i = newOut; i < oldOut; ++i) {
      removePropertyIfExists(linkaudio_source_key + "/" + std::to_string(i));
      removePropertyIfExists(linkaudio_source_key + "/" + std::to_string(i) + "/name");
    }
    for (size_t i = newIn; i < oldIn; ++i) {
      removePropertyIfExists(linkaudio_sink_key + "/" + std::to_string(i) + "/name");
    }
    setLinkAudioInStereoChannelsProperty(newIn);
    setLinkAudioOutStereoChannelsProperty(newOut);
    setLinkAudioSourceProperty();
    setLinkAudioSourceFiltersProperty();
    setLinkAudioChannelsProperty(mLink.channels());
    for (size_t i = 0; i < newIn; ++i)
      setLinkAudioSinkNameProperty(i);
    for (size_t i = 0; i < newOut; ++i)
      setLinkAudioSourceNameProperty(i);
  }

  mNeedsSaveConfig = true;
  mUpdatePortMeta = true; //newly-registered ports need their group/labels
}

void JackTransportLink::applySourceFiltersFromConfig(const std::string& jsonText) {
  if (parseLinkAudioSourceFilters(jsonText, mLinkAudioPeerFilters, mLinkAudioChannelFilters)) {
    mNeedsSourceUpdate.store(true, std::memory_order_release);
    // republish the configured-filter reflection: the constructor already published an
    // (empty) source-filters before config was loaded, so without this the runner/web
    // would keep showing "Auto" for filters restored from disk
    mReportLinkAudioSourceFilters = true;
  }
}

void JackTransportLink::saveConfig() {
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
  cfg["sync"]              = mSyncLink;
  cfg["link_audio_enabled"]    = mLinkAudioEnabled;
  cfg["in_stereo_channels"]    = mNumStereoInChannels;
  cfg["out_stereo_channels"]   = mNumStereoOutChannels;
  nlohmann::json filters = nlohmann::json::array();
  for (size_t i = 0; i < mLinkAudioPeerFilters.size(); ++i) {
    nlohmann::json entry = nlohmann::json::object();
    if (!mLinkAudioPeerFilters[i].empty())    entry["peer"]    = mLinkAudioPeerFilters[i];
    if (!mLinkAudioChannelFilters[i].empty()) entry["channel"] = mLinkAudioChannelFilters[i];
    filters.push_back(entry);
  }
  cfg["source_filters"] = filters;
  cfg["sink_names"]     = mSinkNames;
  cfg["source_names"]   = mSourceNames;
  cfg["link_peer_name"] = mLinkPeerName;
  cfg["link_audio_capture_latency_trim_ms"]  = mCaptureLatencyTrimMs.load(std::memory_order_acquire);
  cfg["link_audio_playback_latency_trim_ms"] = mPlaybackLatencyTrimMs.load(std::memory_order_acquire);

  std::ofstream f(mConfigPath);
  if (f.is_open())
    f << cfg.dump(2) << "\n";
}
