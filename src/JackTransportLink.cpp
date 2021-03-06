#include "JackTransportLink.hpp"

#include <jack/uuid.h>

namespace {
  const char * decimal_type = "https://www.w3.org/2001/XMLSchema#decimal";
  const std::string(bpm_key) = "http://www.x37v.info/jack/metadata/bpm";
}

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
  mBPMLast(initialBPM),
  mInitialQuantum(initialQuantum),
  mInitialTimeSigDenom(initialTimeSigDenom),
  mInitialTicksPerBeat(initialTicksPerBeat),
  mLink(initialBPM),
  mRequestPosition(false),
  mJackClientUUID(0)
{
  //setup link
  mLink.setTempoCallback([this](double bpm) {
    mBPM.store(bpm, std::memory_order_release);
    setBPMProperty(bpm);
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

  //intialize our bpm property
  {
    //try to get our uuid, if we can get it, we set the property and property callback
    char * uuids;
    if ((uuids = jack_get_uuid_for_client_name(mJackClient, jack_get_client_name(mJackClient))) != nullptr
        && jack_uuid_parse(uuids, &mJackClientUUID) == 0) {
      setBPMProperty(mBPM.load(std::memory_order_acquire));
      jack_set_property_change_callback(mJackClient, JackTransportLink::propertyChangeCallback, this);
    }
  }

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

  jack_position_t pos;

  //TODO in follower mode, always report transport state changes,
  //also, mBPM won't contain a valid BPM for the transport
  
  //when the session state is stopped, timeBaseCallback isn't called, so we report start/stop in the processCallback
  auto transportState = jack_transport_query(mJackClient, &pos);
  bool bbtValid = pos.valid & JackPositionBBT;
  auto rolling = transportState == jack_transport_state_t::JackTransportRolling;
  bool stateChange = transportState != mTransportStateReportedLast && (rolling || transportState == jack_transport_state_t::JackTransportStopped);
  double bpm = mBPM.load(std::memory_order_acquire);
  bool bpmChange = bbtValid && pos.beats_per_minute != bpm;
  if (stateChange || bpmChange || mRequestPosition) {
    auto sessionState = mLink.captureAudioSessionState();
    if (stateChange) {
      sessionState.setIsPlaying(rolling, mTime);
      mTransportStateReportedLast = transportState;
    }
    if (bpmChange) {
      sessionState.setTempo(bpm, mTime);
      mBPMLast = bpm;
    }
    //XXX in follower mode, do we always just request position?
    if (mRequestPosition) {
      double quantum = bbtValid ? pos.beats_per_bar : mInitialQuantum;
      //request position
      double linkBeat = static_cast<double>(pos.bar - 1) * quantum
        + static_cast<double>(pos.beat - 1)
        + static_cast<double>(pos.tick) / pos.ticks_per_beat;
      //TODO offset time based on bbtOffset?
      sessionState.requestBeatAtTime(linkBeat, mTime, quantum);
      mRequestPosition = false;
    }
    mLink.commitAudioSessionState(sessionState);
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
    mRequestPosition = true;
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

void JackTransportLink::propertyChangeCallback(jack_uuid_t subject, const char *key, jack_property_change_t change, void *arg) {
  return reinterpret_cast<JackTransportLink *>(arg)->propertyChangeCallback(subject, key, change);
}

void JackTransportLink::propertyChangeCallback(jack_uuid_t subject, const char *key, jack_property_change_t change) {
  //if the subject is all or us and the key is all (empty) or bpm
  if ((jack_uuid_empty(subject) || subject == mJackClientUUID) && (!key || bpm_key.compare(key) == 0)) {
    if (change == jack_property_change_t::PropertyChanged) {
      char * values = nullptr;
      char * types = nullptr;
      if (0 == jack_get_property(mJackClientUUID, bpm_key.c_str(), &values, &types)) {
        //convert to double and store if success
        char* pEnd = nullptr;
        double bpm = std::strtod(values, &pEnd);
        if (*pEnd == 0)
          mBPM.store(bpm, std::memory_order_release);
        //free
        if (values)
          jack_free(values);
        if (types)
          jack_free(types);
      }
    } else if (change == jack_property_change_t::PropertyDeleted) {
      setBPMProperty(mBPM.load(std::memory_order_acquire));
    }
  }
}

void JackTransportLink::setBPMProperty(double bpm) {
  if (!jack_uuid_empty(mJackClientUUID)) {
    std::string bpms = std::to_string(bpm);
    jack_set_property(mJackClient, mJackClientUUID, bpm_key.c_str(), bpms.c_str(), decimal_type);
  }
}
