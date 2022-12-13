#include "JackTransportLink.hpp"

#include <jack/uuid.h>
#include <string>

namespace {
  const char * decimal_type = "https://www.w3.org/2001/XMLSchema#decimal";
  const char * bool_type = "https://www.w3.org/2001/XMLSchema#boolean";
  const std::string bpm_key("http://www.x37v.info/jack/metadata/bpm");
  const std::string start_stop_key("http://www.x37v.info/jack/metadata/link/start-stop-sync");
  const std::array<std::string, 2> true_values = { "true", "1" };

  //helper to deal with dealloc and std::string
  bool get_property(jack_uuid_t subject, const std::string& key, std::string& value_out, std::string& type_out) {
      char * values = nullptr;
      char * types = nullptr;
      if (jack_get_property(subject, key.c_str(), &values, &types) != 0)
        return false;
      value_out = std::string(values);
      type_out = std::string(type_out);
      if (values)
        jack_free(values);
      if (types)
        jack_free(types);
      return true;
  }

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
  mJackClientUUID(0)
{
  //setup link
  mLink.setTempoCallback([this](double bpm) {
    mBPM.store(bpm, std::memory_order_release);
    setBPMProperty(bpm);
  });
  if (enableStartStopSync) {
    mLink.setStartStopCallback([this](bool isPlaying) {
      if (mLink.isStartStopSyncEnabled()) {
        if (isPlaying) {
          jack_transport_start(mJackClient);
        } else {
          jack_transport_stop(mJackClient);
        }
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
      setEnableStartStopProperty(mLink.isStartStopSyncEnabled());
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
  if (stateChange || bpmChange) {
    auto sessionState = mLink.captureAudioSessionState();
    if (stateChange) {
      sessionState.setIsPlaying(rolling, mTime);
      mTransportStateReportedLast = transportState;
    }
    if (bpmChange) {
      sessionState.setTempo(bpm, mTime);
      mBPMLast = bpm;
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
  double ticksPerBeat = bbtValid ? pos->ticks_per_beat : mInitialTicksPerBeat;
  double linkBeat = std::max(0.0, sessionState.beatAtTime(mTimeNext, quantum));
  double tickCurrent = linkBeat * ticksPerBeat;

  if (posIsNew) {
    double time = pos->frame / (static_cast<double>(pos->frame_rate) * 60.0);
		tickCurrent = time * bpm * ticksPerBeat;
		linkBeat = tickCurrent / ticksPerBeat;

    //use frame to compute the beat
    sessionState.requestBeatAtTime(linkBeat, mTimeNext, quantum);

    mLink.commitAudioSessionState(sessionState);
    linkBeat = std::max(0.0, sessionState.beatAtTime(mTimeNext, quantum));
    tickCurrent = linkBeat * ticksPerBeat;
  }
  auto linkPhase = sessionState.phaseAtTime(mTimeNext, quantum);


  //what if quantum changes? Does link keep track of that or should we compute bar some other way?
  auto bar = std::floor(linkBeat / quantum);
  auto beat = std::max(0.0, linkPhase);
  float beatType = bbtValid ? pos->beat_type : mInitialTimeSigDenom; 


  pos->valid = JackPositionBBT;
  pos->bar = static_cast<int32_t>(bar) + 1;
  pos->beat = static_cast<int32_t>(beat) + 1;
  pos->tick = static_cast<int32_t>(ticksPerBeat * (beat - floor(beat)));
  pos->bar_start_tick = bar * quantum * ticksPerBeat;
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
  if ((jack_uuid_empty(subject) || subject == mJackClientUUID)) {
    bool bpm = !key || bpm_key.compare(key) == 0;
    bool enable = !key || start_stop_key.compare(key) == 0;
    if (change == jack_property_change_t::PropertyChanged) {
      std::string values;
      std::string types;
      if (bpm && get_property(mJackClientUUID, bpm_key, values, types)) {
        //convert to double and store if success
        char* pEnd = nullptr;
        double bpm = std::strtod(values.c_str(), &pEnd);
        if (*pEnd == 0)
          mBPM.store(bpm, std::memory_order_release);
      }
      if (enable && get_property(mJackClientUUID, start_stop_key, values, types)) {
        bool set = std::find(true_values.begin(), true_values.end(), values) != true_values.end();
        mLink.enableStartStopSync(set);
      }
    } else if (change == jack_property_change_t::PropertyDeleted) {
      if (bpm)
        setBPMProperty(mBPM.load(std::memory_order_acquire));
      if (enable)
        setEnableStartStopProperty(mLink.isStartStopSyncEnabled());
    }
  }
}

void JackTransportLink::setBPMProperty(double bpm) {
  if (!jack_uuid_empty(mJackClientUUID)) {
    std::string bpms = std::to_string(bpm);
    jack_set_property(mJackClient, mJackClientUUID, bpm_key.c_str(), bpms.c_str(), decimal_type);
  }
}

void JackTransportLink::setEnableStartStopProperty(bool enable) {
  if (!jack_uuid_empty(mJackClientUUID)) {
    std::string enables = enable ? "true" : "false";
    jack_set_property(mJackClient, mJackClientUUID, start_stop_key.c_str(), enables.c_str(), bool_type);
  }
}

