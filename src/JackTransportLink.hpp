#pragma once

#include <chrono>
#include <atomic>

#include <jack/jack.h>
#include <jack/types.h>
#include <jack/metadata.h>

#include <ableton/Link.hpp>
#include <ableton/link/HostTimeFilter.hpp>
#include <ableton/platforms/Config.hpp>

class JackTransportLink {
  public:
    enum class MIDIClockRunState {
      Running,
      Stopped,
      NeedsSync
    };

    JackTransportLink(
        jack_client_t * client,
        bool enableStartStopSync = true,
        double initialBPM = 100.,
        double initialQuantum = 4.,
        float initialTimeSigDenom = 4.,
        double initialTicksPerBeat = 1920.
    );
    ~JackTransportLink();

    static int processCallback(jack_nframes_t nframes, void *arg);
    static void timeBaseCallback(jack_transport_state_t state, jack_nframes_t nframes, jack_position_t *pos, int new_pos, void *arg);
    static int syncCallback(jack_transport_state_t state, jack_position_t *pos, void *arg);
    static void propertyChangeCallback(jack_uuid_t subject, const char *key, jack_property_change_t change, void *arg);
  private:
    int processCallback(jack_nframes_t nframes);
    void timeBaseCallback(jack_transport_state_t state, jack_nframes_t nframes, jack_position_t *pos, bool posIsNew);
    int syncCallback(jack_transport_state_t state, jack_position_t *pos);
    void propertyChangeCallback(jack_uuid_t subject, const char *key, jack_property_change_t change);
    void setBPMProperty(double bpm);
    void setEnableStartStopProperty(bool enable);

    void invalidateClockSyncBBT();

    jack_client_t * mJackClient;
    ableton::Link mLink;

    jack_port_t * mMIDIClockOut = nullptr;
    MIDIClockRunState mMIDIClockRunState = MIDIClockRunState::Stopped;
    int mMIDIClockCount = 0;
    double mClockFrameDelay = 0; //first clock tick gets a delay, track it across process calls

    jack_port_t * mClickPort = nullptr;
    double mInternalBeat = 0.0;

    int32_t mBeatLast = -1;
    int32_t mBarLast = -1;
    double mTickLast = -1.0;

    std::chrono::microseconds mTime;
    std::chrono::microseconds mTimeNext;

    jack_transport_state_t mTransportStateReportedLast = jack_transport_state_t::JackTransportStopped;

    std::atomic<double> mBPM;
    double mBPMLast;
    double mInitialQuantum; //time sig num, called quantum in link
    float mInitialTimeSigDenom;
    double mInitialTicksPerBeat;

    jack_uuid_t mJackClientUUID;
};
