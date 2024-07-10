#include "JackTransportLink.hpp"

#include <jack/midiport.h>
#include <jack/uuid.h>
#include <string>

//debugging defines

//#define DO_CLICK_OUT
//#define USE_INTERNAL_BEAT

//send midi start at the start of every bar
//#define MIDI_SEND_REPEATED_STARTS

#define MIDI_PPQ 24

namespace {
  const char * decimal_type = "https://www.w3.org/2001/XMLSchema#decimal";
  const char * bool_type = "https://www.w3.org/2001/XMLSchema#boolean";
  const std::string bpm_key("http://www.x37v.info/jack/metadata/bpm");
  const std::string start_stop_key("http://www.x37v.info/jack/metadata/link/start-stop-sync");
  const std::array<std::string, 2> true_values = { "true", "1" };

  const std::array<uint8_t, 1> midi_clock_buf = { 248 };
  const std::array<uint8_t, 1> midi_start_buf = { 250 };
  const std::array<uint8_t, 1> midi_stop_buf = { 252 };

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

  mMIDIClockOut = jack_port_register(mJackClient, "clock", JACK_DEFAULT_MIDI_TYPE, JackPortFlags::JackPortIsOutput, 0);

#ifdef DO_CLICK_OUT
  mClickPort = jack_port_register(mJackClient, "clickout", JACK_DEFAULT_AUDIO_TYPE, JackPortFlags::JackPortIsOutput, 0);
#endif

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

void updateBBT(int32_t& bar, int32_t& beat, double& tick, double ticks_per_beat) {
  if (tick >= ticks_per_beat) {
    beat += 1;
    tick = std::fmod(tick, ticks_per_beat);
    if (beat >= 4) {
      beat = beat % 4;
      bar += 1;
    }
  }
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

#ifndef DO_CLICK_OUT
  //write midi sync
  auto midi_buf = jack_port_get_buffer(mMIDIClockOut, nframes);
  jack_midi_clear_buffer(midi_buf);

  if (bbtValid) {
    if (rolling) {
      const double clocksPerBeat = MIDI_PPQ;
      const double sr = static_cast<double>(jack_get_sample_rate(mJackClient));

      int32_t beat = pos.beat - 1;
      int32_t bar = pos.bar - 1;
      double tick = static_cast<double>(pos.tick);

      double framesPerTick = 60.0 * sr / (pos.ticks_per_beat * pos.beats_per_minute);
      double ticksPerClock = pos.ticks_per_beat / clocksPerBeat;
      double framesPerClock = framesPerTick * ticksPerClock;

      //offset from buffer tick start to the tick where we should issue the first clock
      double offsetTicks = std::fmod(tick, ticksPerClock);
      offsetTicks = offsetTicks <= 0.0 ? 0.0 : (ticksPerClock - offsetTicks);

      //update the tick
      tick = tick + offsetTicks;
      updateBBT(bar, beat, tick, pos.ticks_per_beat);
      double nextClockFrame = offsetTicks * framesPerTick;

      //skip dupes
      if (mBarLast == bar && mBeatLast == beat && mTickLast == tick) {
        // std::cout << "dupe found: " << bar << ":" << beat << ":" << tick << std::endl;
        tick += ticksPerClock;
        nextClockFrame += ticksPerClock * framesPerTick;
        updateBBT(bar, beat, tick, pos.ticks_per_beat);
      }

      if (mMIDIClockRunState == MIDIClockRunState::NeedsSync) {
        jack_midi_event_write(midi_buf, 0, midi_stop_buf.data(), midi_stop_buf.size());
        mMIDIClockRunState = MIDIClockRunState::Stopped;
      }

      double frame = nextClockFrame;
      while (floor(frame + mClockFrameDelay) < static_cast<double>(nframes)) {
        if (mMIDIClockRunState == MIDIClockRunState::Running) {
          jack_nframes_t f = static_cast<jack_nframes_t>(frame + mClockFrameDelay);
          mClockFrameDelay = 0;

          //verify that we're keeping in sync with 24 clocks per quarter note
          bool resync = false;
          if (mMIDIClockCount == 0) {
            resync = tick >= ticksPerClock;
          } else if (tick < ticksPerClock) {
            resync = true;
          }

          if (resync) {
            //std::cout << "clock out of sync? bar: " << bar << " beat: " << beat << " tick: " << tick << " clock count: " << mMIDIClockCount << std::endl;
            //std::cout << "\tframes per clock: " << framesPerClock << " ticks per clock:" <<  ticksPerClock << " frames per tick: " << framesPerTick << std::endl;
            //TODO could we be smarter and simply issue some extra or skip some clocks?
            mMIDIClockRunState = MIDIClockRunState::NeedsSync;
            jack_midi_event_write(midi_buf, f, midi_stop_buf.data(), midi_stop_buf.size());
            break;
          }

#ifdef MIDI_SEND_REPEATED_STARTS
          if (beat == 0 && mMIDIClockCount == 0) {
            jack_midi_event_write(midi_buf, frame, midi_start_buf.data(), midi_start_buf.size());
          }
#endif

          jack_midi_event_write(midi_buf, f, midi_clock_buf.data(), midi_clock_buf.size());
          mMIDIClockCount = (mMIDIClockCount + 1) % MIDI_PPQ;
        } else if (beat == 0 && tick < ticksPerClock && tick >= 0 && bar > 0) { //XXX what about bar == 0 ?
          //see if we need to send a start
          mMIDIClockRunState = MIDIClockRunState::Running;
#ifndef MIDI_SEND_REPEATED_STARTS
          jack_midi_event_write(midi_buf, static_cast<jack_nframes_t>(frame), midi_start_buf.data(), midi_start_buf.size());
#endif

          //std::cout << "start " << frame << " tick: " << tick << std::endl;

          //delay clock 1ms or half a clock period
          //http://midi.teragonaudio.com/tech/midispec.htm
          //TODO mClockFrameDelay = std::min(framesPerClock / 2, sr / 1000.0);
          mMIDIClockCount = 0;
          continue; //restart loop
        }

        mTickLast = tick;
        mBeatLast = beat;
        mBarLast = bar;

        tick += ticksPerClock;
        frame = frame + framesPerClock;
        updateBBT(bar, beat, tick, pos.ticks_per_beat);
      }
    } else if (transportState == JackTransportStopped && mMIDIClockRunState != MIDIClockRunState::Stopped) {
      mClockFrameDelay = 0;
      mMIDIClockRunState = MIDIClockRunState::Stopped;
      jack_midi_event_write(midi_buf, 0, midi_stop_buf.data(), midi_stop_buf.size());
      invalidateClockSyncBBT();
    }
  }
#else 
  if (mClickPort != nullptr) {
    jack_default_audio_sample_t * buf = reinterpret_cast<jack_default_audio_sample_t *>(jack_port_get_buffer(mClickPort, nframes));

    //zero out
    std::memset(buf, 0, nframes * sizeof(jack_default_audio_sample_t));
    if (bbtValid && rolling) {

      const double clicksPerBeat = 4;
      const double sr = static_cast<double>(jack_get_sample_rate(mJackClient));

      int32_t beat = pos.beat - 1;
      int32_t bar = pos.bar - 1;
      double tick = static_cast<double>(pos.tick);

      double framesPerTick = 60.0 * sr / (pos.ticks_per_beat * pos.beats_per_minute);
      double ticksPerClick = pos.ticks_per_beat / clicksPerBeat;
      double framesPerClick = framesPerTick * ticksPerClick;

      double offsetTicks = std::fmod(tick, ticksPerClick);
      offsetTicks = offsetTicks <= 0.0 ? 0.0 : ticksPerClick - offsetTicks;
      tick = tick + offsetTicks;
      updateBBT(bar, beat, tick, pos.ticks_per_beat);

      //skip dupes
      if (mBarLast == bar && mBeatLast == beat && mTickLast == tick) {
        //std::cout << bar << ":" << beat << ":" << tick << " last: " << mBarLast << ":" << mBeatLast << ":" << mTickLast << std::endl;
        tick += ticksPerClick;
        offsetTicks += ticksPerClick;
        updateBBT(bar, beat, tick, pos.ticks_per_beat);
      }

      double nextClickFrame = offsetTicks * framesPerTick;

      double frame = nextClickFrame;
      while (ceil(frame) < static_cast<double>(nframes)) {
        jack_nframes_t f = static_cast<jack_nframes_t>(frame);
        buf[f] = 1.0; //0.5 + 0.5 * static_cast<jack_default_audio_sample_t>(beat) / 4.0;

        mTickLast = tick;
        mBeatLast = beat;
        mBarLast = bar;

        tick += ticksPerClick;
        frame = frame + framesPerClick;
        updateBBT(bar, beat, tick, pos.ticks_per_beat);
      }

    }
  }
#endif

  return 0;
}

void JackTransportLink::timeBaseCallback(jack_transport_state_t state, jack_nframes_t nframes, jack_position_t *pos, int new_pos, void *arg) {
  reinterpret_cast<JackTransportLink *>(arg)->timeBaseCallback(state, nframes, pos, new_pos);
}


//timebase callback, only called while transport is running or starting
void JackTransportLink::timeBaseCallback(jack_transport_state_t transportState, jack_nframes_t nframes, jack_position_t *pos, bool posIsNew) {
  auto sessionState = mLink.captureAudioSessionState();
  bool bbtValid = pos->valid & JackPositionBBT;

  double bpm = mBPM.load(std::memory_order_acquire);
  double quantum = bbtValid ? pos->beats_per_bar : mInitialQuantum;
  double ticksPerBeat = bbtValid ? pos->ticks_per_beat : mInitialTicksPerBeat;

  auto linkTime = mTime;

#ifndef USE_INTERNAL_BEAT
  double linkBeat = std::max(0.0, sessionState.beatAtTime(linkTime, quantum));
#else
  double linkBeat = mInternalBeat;
#endif

  //TODO handle negative
  if (linkBeat < 0.0) {
    linkBeat = 0.0;
  }

  double tickCurrent = linkBeat * ticksPerBeat;

#ifndef USE_INTERNAL_BEAT
  if (posIsNew) {
    double time = pos->frame / (static_cast<double>(pos->frame_rate) * 60.0);
    tickCurrent = time * bpm * ticksPerBeat;
    linkBeat = tickCurrent / ticksPerBeat;

    //use frame to compute the beat
    sessionState.requestBeatAtTime(linkBeat, linkTime, quantum);

    mLink.commitAudioSessionState(sessionState);
    linkBeat = sessionState.beatAtTime(linkTime, quantum);
    if (linkBeat < 0.0) {
      linkBeat = 0.0;
      //std::cout << "beat is negative: " << linkBeat << std::endl;
    }
    tickCurrent = linkBeat * ticksPerBeat;

    //need to sync again since we repositioned
    mMIDIClockRunState = MIDIClockRunState::NeedsSync;
    invalidateClockSyncBBT();
  }
#endif

  //what if quantum changes? Does link keep track of that or should we compute bar some other way?
  auto bar = std::floor(linkBeat / quantum);
  auto beat = std::fmod(linkBeat, quantum);
  auto tick = trunc(ticksPerBeat * (beat - trunc(beat)));
  float beatType = bbtValid ? pos->beat_type : mInitialTimeSigDenom; 

  pos->valid = JackPositionBBT;
  pos->bar = static_cast<int32_t>(bar) + 1;
  pos->beat = static_cast<int32_t>(beat) + 1;
  pos->tick = static_cast<int32_t>(tick);
  pos->bar_start_tick = bar * quantum * ticksPerBeat;
  pos->beats_per_bar = static_cast<float>(quantum);
  pos->beat_type = beatType;
  pos->ticks_per_beat = ticksPerBeat;
  pos->beats_per_minute = bpm;

#ifdef USE_INTERNAL_BEAT
  if (transportState == jack_transport_state_t::JackTransportRolling) {
    mInternalBeat += bpm * static_cast<double>(nframes) / (static_cast<double>(jack_get_sample_rate(mJackClient)) * 60.0);
  }
#endif
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

void JackTransportLink::invalidateClockSyncBBT() {
  mBeatLast = -1;
  mBarLast = -1;
  mTickLast = -1.0;
}

