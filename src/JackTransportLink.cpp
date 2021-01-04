#include "JackTransportLink.hpp"

JackTransportLink::JackTransportLink(
    jack_client_t * client,
    bool enableStartStopSync,
    double initialBPM,
    double initialQuantum,
    float initialTimeSigDenom,
    double initialTicksPerBeat
) :
  mJackClient(client),
  mBPM(initialBPM),
  mInitialQuantum(initialQuantum),
  mInitialTimeSigDenom(initialTimeSigDenom),
  mInitialTicksPerBeat(initialTicksPerBeat),
  mLink(initialBPM)
{
  //setup link
  mLink.setTempoCallback([this](double bpm) {
    mBPM.store(bpm, std::memory_order_release);
  });
  if (enableStartStopSync) {
    mLink.setStartStopCallback([this](bool isPlaying) {
        if (isPlaying) {
          jack_transport_start(mJackClient);
        } else {
          jack_transport_stop(mJackClient);
        }
    });
  }
  mLink.enableStartStopSync(enableStartStopSync);
  mLink.enable(true);

  //setup jack, become the timebase master, unconditionally
  jack_set_process_callback(mJackClient, JackTransportLink::processCallback, this);
  jack_set_timebase_callback(mJackClient, 0, JackTransportLink::timeBaseCallback, this);
  jack_set_sync_callback(mJackClient, JackTransportLink::syncCallback, this);
  jack_activate(mJackClient);
}

JackTransportLink::~JackTransportLink() {
  jack_set_sync_callback(mJackClient, nullptr, nullptr);
  jack_release_timebase(mJackClient);
  jack_deactivate(mJackClient);
  jack_client_close(mJackClient);
}

int JackTransportLink::processCallback(jack_nframes_t nframes, void *arg) {
  return reinterpret_cast<JackTransportLink *>(arg)->processCallback(nframes);
}

int JackTransportLink::processCallback(jack_nframes_t nframes) {
  //compute the time, the timeBaseCallback is called right after this processCallback
  {
    jack_nframes_t frameTime;
    jack_time_t cur, next;
    float period;
    if (jack_get_cycle_times(mJackClient, &frameTime, &cur, &next, &period) == 0) {
      mTime = std::chrono::microseconds(cur);
      mTimeNext = std::chrono::microseconds(next);
    } else {
      //report?
    }
  }

  //when the session state is stopped, timeBaseCallback isn't called, so we report start/stop in the processCallback
  auto transportState = jack_transport_query(mJackClient, nullptr);
  auto rolling = transportState == jack_transport_state_t::JackTransportRolling;
  if (transportState != mTransportStateReportedLast && (rolling || transportState == jack_transport_state_t::JackTransportStopped)) {
    auto sessionState = mLink.captureAudioSessionState();
    sessionState.setIsPlaying(rolling, mTime);
    mLink.commitAudioSessionState(sessionState);
    mTransportStateReportedLast = transportState;
  }

  return 0;
}

void JackTransportLink::timeBaseCallback(jack_transport_state_t state, jack_nframes_t nframes, jack_position_t *pos, int new_pos, void *arg) {
  reinterpret_cast<JackTransportLink *>(arg)->timeBaseCallback(state, nframes, pos, new_pos);
}

//timebase callback, only called while transport is running or starting
void JackTransportLink::timeBaseCallback(jack_transport_state_t transportState, jack_nframes_t /*nframes*/, jack_position_t *pos, bool posIsNew) {
  auto sessionState = mLink.captureAudioSessionState();
  bool bbtValid = pos->valid & JackPositionBBT;

  double bpm = mBPM.load(std::memory_order_acquire);
  double quantum = bbtValid ? pos->beats_per_bar : mInitialQuantum;

  if (posIsNew && bbtValid) {
    //update tempo
    bpm = pos->beats_per_minute;
    sessionState.setTempo(bpm, mTime); 

    //request position
    double linkBeat = static_cast<double>(pos->bar - 1) * quantum
      + static_cast<double>(pos->beat - 1)
      + static_cast<double>(pos->tick) / pos->ticks_per_beat;
    //TODO offset time based on bbtOffset?
    sessionState.requestBeatAtTime(linkBeat, mTime, quantum);

    mLink.commitAudioSessionState(sessionState);
  }

  auto linkBeat = std::max(0.0, sessionState.beatAtTime(mTime, quantum));
  auto linkPhase = sessionState.phaseAtTime(mTime, quantum);

  //what if quantum changes? Does link keep track of that or should we compute bar some other way?
  auto bar = std::floor(linkBeat / quantum);
  auto beat = std::max(0.0, linkPhase);
  double ticksPerBeat = bbtValid ? pos->ticks_per_beat : mInitialTicksPerBeat;
  float beatType = bbtValid ? pos->beat_type : mInitialTimeSigDenom; 

  double tickCurrent = linkBeat * ticksPerBeat;

  pos->valid = JackPositionBBT;
  pos->bar = static_cast<int32_t>(bar) + 1;
  pos->beat = static_cast<int32_t>(beat) + 1;
  pos->tick = static_cast<int32_t>(ticksPerBeat * (beat - floor(beat)));
  pos->bar_start_tick = tickCurrent - beat * ticksPerBeat;
  pos->beats_per_bar = static_cast<float>(quantum);
  pos->beat_type = beatType;
  pos->ticks_per_beat = ticksPerBeat;
  pos->beats_per_minute = bpm;
}

int JackTransportLink::syncCallback(jack_transport_state_t state, jack_position_t *pos, void *arg) {
  return reinterpret_cast<JackTransportLink *>(arg)->syncCallback(state, pos);
}

int JackTransportLink::syncCallback(jack_transport_state_t /*transportState*/, jack_position_t * /*pos*/) {
  //TODO delay start to sync with time from session?
  return 1;
}
