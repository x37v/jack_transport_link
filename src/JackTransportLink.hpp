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
                    std::string linkPeerName = "");
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
  void setBPMProperty(double bpm);
  void setEnableStartStopProperty(bool enable);
  void setSyncProperty(bool sync);
  void setNumPeersProperty(size_t peers);
  void setLinkAudioChannelsProperty(const std::vector<ableton::LinkAudio::Channel>& channels);
  void setLinkAudioSourceProperty();
  void setLinkAudioSourceFiltersProperty();
  void setLinkAudioInStereoChannelsProperty(size_t n);
  void setLinkAudioOutStereoChannelsProperty(size_t n);
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
