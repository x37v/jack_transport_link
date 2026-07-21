#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <jack/jack.h>
#include <jack/metadata.h>
#include <jack/types.h>

#include <ableton/LinkAudio.hpp>
#include <ableton/link/HostTimeFilter.hpp>
#include <ableton/platforms/Config.hpp>
#include "LinkAudioRenderer.hpp"

#include <osc/OscPacketListener.h>
#include <osc/OscReceivedElements.h>

class JackTransportLink : public oscpack::OscPacketListener {
public:
  using SinkRenderer = ableton::linkaudio::LinkAudioSinkRenderer<ableton::LinkAudio>;
  using SourceRenderer = ableton::linkaudio::LinkAudioSourceRenderer<ableton::LinkAudio>;

  enum class MIDIClockRunState { Running, Stopped, NeedsSync };

  JackTransportLink(jack_client_t *client,
                    bool enableStartStopSync = true,
                    double initialBPM = 100.,
                    double initialQuantum = 4.,
                    float initialTimeSigDenom = 4.,
                    double initialTicksPerBeat = 1920.,
                    bool enableLinkAudio = true,
                    size_t linkAudioStereoInChannels = 1,
                    size_t linkAudioStereoOutChannels = 0,
                    bool syncLink = true,
                    std::string configPath = "",
                    std::vector<std::string> sinkNames = {},
                    std::vector<std::string> sourceNames = {},
                    std::string linkPeerName = "",
                    double captureLatencyTrimMs = 0.0,
                    double playbackLatencyTrimMs = 0.0,
                    double latencyMs = 100.0,
                    bool syncToIncomingAudio = true,
                    bool linkEnabled = true);
  ~JackTransportLink();

  void processEvents();
  void applySourceFiltersFromConfig(const std::string& jsonText);

  static int processCallback(jack_nframes_t nframes, void *arg);
  static void timeBaseCallback(jack_transport_state_t state,
                               jack_nframes_t nframes, jack_position_t *pos,
                               int new_pos, void *arg);
  static int syncCallback(jack_transport_state_t state, jack_position_t *pos,
                          void *arg);
  static void propertyChangeCallback(jack_uuid_t subject, const char *key,
                                     jack_property_change_t change, void *arg);
  static int bufferSizeCallback(jack_nframes_t nframes, void *arg);
  static void latencyCallback(jack_latency_callback_mode_t mode, void *arg);

protected:
  virtual void ProcessMessage(const oscpack::ReceivedMessage &m,
                              const oscpack::IpEndpointName &remoteEndpoint);

private:
  int processCallback(jack_nframes_t nframes);
  void timeBaseCallback(jack_transport_state_t state, jack_nframes_t nframes,
                        jack_position_t *pos, bool posIsNew);
  int syncCallback(jack_transport_state_t state, jack_position_t *pos);
  void propertyChangeCallback(jack_uuid_t subject, const char *key,
                              jack_property_change_t change);
  int bufferSizeCallback(jack_nframes_t nframes);
  // Re-read JACK's measured capture/playback latency from our in_N/out_N ports, then recompute
  // effective values. Called from processEvents (main thread) — NOT from the latency callback —
  // so the port-vector reads are serialized with rebuildAudioPorts() on the same thread.
  void updateLatencyRanges();
  // Fold auto-detected latency + user trim (ms) into the effective frame offsets (atomics
  // read by the RT process callback) and republish the read-only latency metadata.
  void recomputeEffectiveLatency();
  // Publish effective + auto capture/playback latency (ms) as read-only JACK metadata.
  void setLinkAudioLatencyProperties();
  void setBPMProperty(double bpm);
  void setEnableStartStopProperty(bool enable);
  void setSyncProperty(bool sync);
  void setNumPeersProperty(size_t peers);
  void setLinkAudioChannelsProperty(const std::vector<ableton::LinkAudio::Channel>& channels);
  void setLinkAudioSourceProperty();
  void setLinkAudioSourceHealthProperty();
  void setLinkAudioLatencyMsProperty();
  void setLinkAudioSyncToIncomingProperty();
  void setLinkEnabledProperty();
  void setLinkAudioSourceFiltersProperty();
  void setLinkAudioInStereoChannelsProperty(size_t n);
  void setLinkAudioOutStereoChannelsProperty(size_t n);
  void removePropertyIfExists(const std::string& key);
  void setLinkAudioSlotNameProperty(const std::string& key, const std::string& name);
  void setLinkAudioSinkNameProperty(size_t i);
  void setLinkAudioSourceNameProperty(size_t i);
  // Effective Link peer name = override (mLinkPeerName, if non-empty) else the hostname.
  std::string effectiveLinkPeerName() const;
  // Recompute the effective peer name; if it changed, rename the Link peer and republish.
  void applyLinkPeerName();
  void setLinkAudioPeerNameProperty();
  bool updateLinkAudioSource();
  std::string effectiveSinkName(size_t i) const;
  std::string linkAudioSourcePortLabel(size_t i);
  void updateAudioPortMetadata();
  void applySinkNames();
  void rebuildAudioPorts(size_t newIn, size_t newOut);
  void saveConfig();

  void invalidateClockSyncBBT();

  jack_client_t *mJackClient;

  // mSampleRate must precede mRenderers
  double mSampleRate;
  size_t mNumStereoInChannels;
  size_t mNumStereoOutChannels;
  // Link peer-name override (empty = auto/hostname). Declared before mLink because the
  // constructor seeds mLink's peer name from effectiveLinkPeerName(), which reads it.
  std::string mLinkPeerName;
  ableton::LinkAudio mLink;
  std::vector<std::unique_ptr<SinkRenderer>> mSendRenderers;    // one per stereo in pair
  std::vector<std::unique_ptr<SourceRenderer>> mRecvRenderers;  // one per stereo out pair

  jack_port_t *mMIDIClockOut = nullptr;
  MIDIClockRunState mMIDIClockRunState = MIDIClockRunState::Stopped;
  int mMIDIClockCount = 0;
  // first clock tick gets a delay, track it across process calls
  double mClockFrameDelay = 0;

  jack_port_t *mClickPort = nullptr;
  double mInternalBeat = 0.0;
  bool mSyncLink;
  bool mWasSyncLink;

  int32_t mBeatLast = -1;
  int32_t mBarLast = -1;
  double mTickLast = -1.0;

  std::chrono::microseconds mTime;
  std::chrono::microseconds mTimeNext;

  jack_transport_state_t mTransportStateReportedLast =
      jack_transport_state_t::JackTransportStopped;

  std::atomic<double> mBPM;
  double mLinkBPM;
  double mBPMLast;
  double mQuantum;
  double mInitialQuantum; // time sig num, called quantum in link
  float mInitialTimeSigDenom;
  double mInitialTicksPerBeat;

  jack_uuid_t mJackClientUUID;

  bool mReportBPM = false;
  bool mReportLinkSync = false;
  bool mReportStartStopEnable = false;

  // mStereoSendBuf: mNumStereoInChannels * 2 * nframes
  // mStereoRecvBuf: mNumStereoOutChannels * 2 * nframes
  std::vector<double> mStereoSendBuf;
  std::vector<double> mStereoRecvBuf;

  std::vector<jack_port_t*> mAudioIns;  // 2 * mNumStereoInChannels ports
  std::vector<jack_port_t*> mAudioOuts; // 2 * mNumStereoOutChannels ports
  std::atomic<bool> mChannelsChanged{false};
  bool mLinkAudioEnabled = false;

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
  std::atomic<double> mLatencyMs{100.0};
  // "Sync to Incoming Audio" (formerly Ableton's "Monitoring Mode"): when true, delay the local
  // transport timeline (the reported JACK BBT) by the receive buffer so transport-locked local
  // generators align with the incoming audio; when false, the transport runs live. The receive
  // buffer (mLatencyMs) is ALWAYS applied to incoming playout regardless of this toggle (network
  // buffers arrive late and need it) — this only gates the transport-BBT shift (timeBaseCallback).
  // Default true.
  std::atomic<bool> mSyncToIncomingAudio{true};
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

  // Per-receiver source filters — written from property/OSC callbacks, read in processEvents.
  // Empty string = any (auto). Guarded by mSourceFilterMutex.
  mutable std::mutex mSourceFilterMutex;
  std::vector<std::string> mLinkAudioPeerFilters;
  std::vector<std::string> mLinkAudioChannelFilters;
  // Per-renderer connection state
  std::vector<std::optional<ableton::ChannelId>> mCurrentSourceChannelIds;
  std::vector<std::string> mCurrentSourcePeerNames;
  std::vector<std::string> mCurrentSourceChannelNames;
  std::atomic<bool> mNeedsSourceUpdate{false};
  std::atomic<int> mRequestedStereoInChannels{-1};
  std::atomic<int> mRequestedStereoOutChannels{-1};
  bool mReportLinkAudioChannels = false;
  bool mReportLinkAudioSource = false;
  bool mReportLinkAudioSourceFilters = false;

  std::string mConfigPath;
  bool mNeedsSaveConfig = false;
  std::chrono::steady_clock::time_point mLastConfigSave{};
  // Throttle for periodic source-health metadata publishing (see processEvents).
  std::chrono::steady_clock::time_point mLastHealthPublish{};

  // Per-slot user names. Empty = use the default ("Send N" for sinks).
  // mSinkNames tracks the outgoing (SinkRenderer / in_N) slots, mSourceNames the
  // incoming (SourceRenderer / out_N) slots. mAppliedSinkNames mirrors the names
  // the currently-constructed SinkRenderers announce to the Link session.
  std::vector<std::string> mSinkNames;
  std::vector<std::string> mSourceNames;
  std::vector<std::string> mAppliedSinkNames;
  std::atomic<bool> mNeedsApplySinkNames{false};
  // The effective Link peer name currently announced (mirrors mLink's peer name), so
  // applyLinkPeerName() only calls setPeerName() when it actually changes.
  std::string mAppliedLinkPeerName;
  std::atomic<bool> mNeedsApplyPeerName{false};
  // set when port labels/grouping need to be (re)applied to our audio ports
  bool mUpdatePortMeta = false;
};
