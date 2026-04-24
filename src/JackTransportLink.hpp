#pragma once

#include <atomic>
#include <chrono>
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

/// XXX OSC CONTROL??
///
/// position
/// start/stop
/// bpm??
/// sync to link vs internal?

class JackTransportLink : public oscpack::OscPacketListener {
public:
  enum class MIDIClockRunState { Running, Stopped, NeedsSync };

  JackTransportLink(jack_client_t *client,
                    bool enableStartStopSync = true,
                    double initialBPM = 100.,
                    double initialQuantum = 4.,
                    float initialTimeSigDenom = 4.,
                    double initialTicksPerBeat = 1920.,
                    bool enableLinkAudio = true,
                    size_t linkAudioInChannels = 2,
                    size_t linkAudioOutChannels = 2);
  ~JackTransportLink();

  void processEvents();

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

  void invalidateClockSyncBBT();

  jack_client_t *mJackClient;

  // mSampleRate, channel counts, and mLink must be declared before mLinkAudioRenderer
  double mSampleRate;
  size_t mLinkAudioInChannels;
  size_t mLinkAudioOutChannels;
  ableton::LinkAudio mLink;
  ableton::linkaudio::LinkAudioRenderer<ableton::LinkAudio> mLinkAudioRenderer;

  jack_port_t *mMIDIClockOut = nullptr;
  MIDIClockRunState mMIDIClockRunState = MIDIClockRunState::Stopped;
  int mMIDIClockCount = 0;
  // first clock tick gets a delay, track it across process calls
  double mClockFrameDelay = 0;

  jack_port_t *mClickPort = nullptr;
  double mInternalBeat = 0.0;
  bool mSyncLink = true;
  bool mWasSyncLink = true;

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

  std::vector<double> mAudioSendBuf;
  std::vector<double> mAudioRecvBuf;
  std::vector<double*> mSendPtrs;
  std::vector<double*> mRecvPtrs;

  std::vector<jack_port_t*> mAudioIns;
  std::vector<jack_port_t*> mAudioOuts;
  std::atomic<bool> mChannelsChanged{false};
  bool mLinkAudioEnabled = false;
};
