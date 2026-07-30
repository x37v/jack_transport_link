#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#if defined(LINK_AUDIO)

#include <ableton/LinkAudio.hpp>
#include <ableton/link_audio/Buffer.hpp>
#include <ableton/link_audio/Queue.hpp>
#include <ableton/util/FloatIntConversion.hpp>

static_assert(std::atomic<float>::is_always_lock_free);
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::atomic<size_t>::is_always_lock_free);

namespace ableton {
namespace linkaudio {

namespace {

template <typename T> T cubicInterpolate(const std::array<T, 4> &p, double t) {
  double a = -0.5 * static_cast<double>(p[0]) +
             1.5 * static_cast<double>(p[1]) - 1.5 * static_cast<double>(p[2]) +
             0.5 * static_cast<double>(p[3]);
  double b = static_cast<double>(p[0]) - 2.5 * static_cast<double>(p[1]) +
             2.0 * static_cast<double>(p[2]) - 0.5 * static_cast<double>(p[3]);
  double c = -0.5 * static_cast<double>(p[0]) + 0.5 * static_cast<double>(p[2]);
  auto d = static_cast<double>(p[1]);
  return static_cast<T>(a * t * t * t + b * t * t + c * t + d);
}

template <typename T>
T linearInterpolate(T value, T inMin, T inMax, T outMin, T outMax) {
  return (value - inMin) * (outMax - outMin) / (inMax - inMin) + outMin;
}

} // namespace

template <typename Link> class LinkAudioSinkRenderer {
public:
  LinkAudioSinkRenderer(Link &link, std::string name, size_t numChannels,
                        double &sampleRate)
      : mLink(link), mNumChannels(numChannels),
        mSink(mLink, std::move(name), 4096 * numChannels),
        mSampleRate(sampleRate) {}

  void send(const float *const *ppChannels, size_t numFrames,
            typename Link::SessionState sessionState, double sampleRate,
            const std::chrono::microseconds hostTime, double quantum) {
    auto buffer = LinkAudioSink::BufferHandle(mSink);
    if (buffer) {
      for (size_t frame = 0; frame < numFrames; ++frame)
        for (size_t ch = 0; ch < mNumChannels; ++ch)
          buffer.samples[frame * mNumChannels + ch] =
              ableton::util::floatToInt16(ppChannels[ch][frame]);

      const auto beatsAtBufferBegin =
          sessionState.beatAtTime(hostTime, quantum);
      buffer.commit(sessionState, beatsAtBufferBegin, quantum, numFrames,
                    mNumChannels, static_cast<uint32_t>(sampleRate));
    }
  }

private:
  Link &mLink;
  size_t mNumChannels;
  LinkAudioSink mSink;
  double &mSampleRate;
};

template <typename Link> class LinkAudioSourceRenderer {
  struct Buffer {
    std::vector<double> mSamples;
    LinkAudioSource::BufferHandle::Info mInfo;
  };
  using Queue = link_audio::Queue<Buffer>;

public:
  LinkAudioSourceRenderer(Link &link, size_t numChannels, double &sampleRate)
      : mLink(link), mNumChannels(numChannels), mSampleRate(sampleRate) {
    Buffer proto;
    proto.mSamples.resize(1024 * 8);
    auto queue = Queue(2048, proto);
    mpQueueWriter =
        std::make_shared<typename Queue::Writer>(std::move(queue.writer()));
    mpQueueReader =
        std::make_shared<typename Queue::Reader>(std::move(queue.reader()));
  }

  ~LinkAudioSourceRenderer() { mpSource.reset(); }

  void receive(float *const *ppChannels, size_t numFrames,
               typename Link::SessionState sessionState, double sampleRate,
               const std::chrono::microseconds hostTime, double quantum,
               double latencyMs) {
    auto silenceOutputs = [&]() {
      for (size_t ch = 0; ch < mNumChannels; ++ch)
        std::fill_n(ppChannels[ch], numFrames, 0.0);
      // Every path that silences also clears moStartReadPos, so the *next* block is treated as
      // pre-roll again and mDropoutCount stops advancing. That makes the dropout count useless
      // for telling "healthy" apart from "never produced a sample" — e.g. a playout buffer too
      // small for the network, where the target beat can never be satisfied. mRendering is the
      // signal that distinguishes them: false here, true only after a block actually renders.
      mRendering.store(false, std::memory_order_relaxed);
    };

    // Make every buffer published by the Link callback visible to this render
    // pass. Retained slots stay readable until releaseSlot() advances the head
    // of the queue.
    while (mpQueueReader->retainSlot()) {
    }

    // We were mid-stream if a read position is already established; producing
    // silence from here then means the queue starved (a real dropout), as
    // opposed to normal pre-roll silence.
    const bool wasRendering = moStartReadPos.has_value();

    // Playout buffer expressed in milliseconds, converted to beats at the
    // current tempo — the same scheme Ableton Live and Max use (latency_beats =
    // (ms/1000) * (bpm/60)). Expressing it in ms keeps the real-time buffer
    // depth constant regardless of tempo (a fixed beat count would shrink in
    // real time as the tempo rises).
    const double kLatencyInBeats =
        (latencyMs / 1000.0) * (sessionState.tempo() / 60.0);
    const auto targetBeatsAtBufferBegin =
        sessionState.beatAtTime(hostTime, quantum) - kLatencyInBeats;
    const auto targetBeatsAtBufferEnd =
        sessionState.beatAtTime(
            hostTime + std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::duration<double>(double(numFrames) /
                                                         sampleRate)),
            quantum) -
        kLatencyInBeats;

    // Before rendering starts, discard complete network buffers that precede
    // the target window. Once moStartReadPos exists, normal consumption below
    // owns all queue advancement so the fractional read position remains
    // relative to the first retained slot.
    while (!moStartReadPos && mpQueueReader->numRetainedSlots() > 0) {
      const auto headEnd =
          (*mpQueueReader)[0]->mInfo.endBeats(sessionState, quantum);
      if (!headEnd) {
        // beginBeats/endBeats return nullopt when the buffer was stamped in a
        // *different Link session*, so its beat time cannot be placed on our
        // timeline at all. Dropping it is the only option, but it must be
        // counted: this is otherwise indistinguishable from "nothing is
        // arriving", and no latency value can fix it.
        //
        // Comparing the optional directly (as this used to) silently swallowed
        // the case — `std::optional` mixed comparison defines `nullopt < v` as
        // *true* for every v, so such a buffer always looked "too old" and was
        // discarded no matter how large the playout buffer was.
        mUnmappableCount.fetch_add(1, std::memory_order_relaxed);
        mpQueueReader->releaseSlot();
        continue;
      }
      if (*headEnd < targetBeatsAtBufferBegin) {
        mpQueueReader->releaseSlot();
        continue;
      }
      break;
    }

    if (mpQueueReader->numRetainedSlots() == 0) {
      // An empty queue during pre-roll is expected; it only becomes a dropout
      // after playback has established a read position.
      if (wasRendering)
        mDropoutCount.fetch_add(1, std::memory_order_relaxed);
      silenceOutputs();
      moStartReadPos = std::nullopt;
      mBuffered = 0;
      return;
    }

    if (!moStartReadPos) {
      // The loop above released every unmappable head, so the head is mappable here.
      const auto &info = (*mpQueueReader)[0]->mInfo;
      const auto startBufferBegin = info.beginBeats(sessionState, quantum);
      const auto startBufferEnd = info.endBeats(sessionState, quantum);

      if (!startBufferBegin || !startBufferEnd) {
        mUnmappableCount.fetch_add(1, std::memory_order_relaxed);
        silenceOutputs();
        mBuffered = 0;
        return;
      }

      if (*startBufferBegin > targetBeatsAtBufferBegin) {
        // The first received buffer begins in the future relative to the delayed
        // playout cursor. Preserve it and output pre-roll silence until the
        // target window catches up.
        silenceOutputs();
        mBuffered = 0;
        return;
      }

      // Convert the target beat into a fractional frame offset within the first
      // usable buffer. Keeping the fraction allows the resampler to align
      // playout more precisely than a whole-frame seek.
      moStartReadPos =
          linearInterpolate(targetBeatsAtBufferBegin, *startBufferBegin,
                            *startBufferEnd, 0.0, double(info.numFrames));
    }

    const auto startFramePos = *moStartReadPos;

    auto totalFrames = 0.0;
    auto foundEnd = false;

    // Locate the end of this JACK block on the incoming beat timeline.
    // totalFrames is the fractional amount of source audio spanning from the
    // queue head to that endpoint, possibly crossing several network buffers.
    for (auto i = 0u; i < mpQueueReader->numRetainedSlots(); ++i) {
      const auto &info = (*mpQueueReader)[i]->mInfo;
      const auto bufferBegin = info.beginBeats(sessionState, quantum);
      const auto bufferEnd = info.endBeats(sessionState, quantum);

      // A buffer from a different Link session has no place on our timeline, so it can't
      // extend the span. Stop here and let the !foundEnd path below report the starve.
      // (Dereferencing these unconditionally was undefined behaviour: once moStartReadPos
      // is established the release loop above no longer runs, so an unmappable buffer
      // reaches this point intact.)
      if (!bufferBegin || !bufferEnd) {
        mUnmappableCount.fetch_add(1, std::memory_order_relaxed);
        break;
      }

      if (targetBeatsAtBufferEnd >= *bufferBegin &&
          targetBeatsAtBufferEnd < *bufferEnd) {
        totalFrames +=
            linearInterpolate(targetBeatsAtBufferEnd, *bufferBegin, *bufferEnd,
                              0.0, double(info.numFrames));
        foundEnd = true;
        break;
      } else {
        totalFrames += double(info.numFrames);
      }
    }

    if (!foundEnd) {
      if (wasRendering)
        mDropoutCount.fetch_add(1, std::memory_order_relaxed);
      silenceOutputs();
      moStartReadPos = std::nullopt;
      mBuffered = 0;
      return;
    }

    totalFrames -= startFramePos;

    if (totalFrames <= 0.0) {
      if (wasRendering)
        mDropoutCount.fetch_add(1, std::memory_order_relaxed);
      silenceOutputs();
      moStartReadPos = std::nullopt;
      mBuffered = 0;
      return;
    }

    const auto frameIncrement = totalFrames / double(numFrames);
    auto readPos = startFramePos;

    const size_t srcChannels = (*mpQueueReader)[0]->mInfo.numChannels;

    // Address the retained buffers as one contiguous, interleaved source
    // stream. This hides network-buffer boundaries from the interpolation loop
    // and supplies silence for a missing destination channel or an out-of-range
    // look-up.
    auto getSample = [&](size_t idx, size_t ch) -> double {
      size_t bufferIdx = 0;
      size_t currentIdx = idx;
      while (bufferIdx < mpQueueReader->numRetainedSlots()) {
        auto &currentBuffer = *((*mpQueueReader)[bufferIdx]);
        if (currentIdx < currentBuffer.mInfo.numFrames) {
          const size_t bufSrcCh = currentBuffer.mInfo.numChannels;
          return (ch < bufSrcCh)
                     ? currentBuffer.mSamples[currentIdx * bufSrcCh + ch]
                     : 0.0;
        }
        currentIdx -= currentBuffer.mInfo.numFrames;
        ++bufferIdx;
      }
      return 0.0;
    };

    for (auto frame = 0u; frame < numFrames; ++frame) {
      const auto framePos = readPos + frame * frameIncrement;
      const auto frameIdx = static_cast<size_t>(std::floor(framePos));
      const auto t = framePos - std::floor(framePos);

      // Catmull-Rom interpolation expects chronological samples, with t moving
      // from p[1] (frameIdx) toward p[2] (frameIdx + 1). Clamp the look-behind
      // at stream start; getSample supplies silence beyond retained look-ahead.
      for (size_t ch = 0; ch < mNumChannels; ++ch) {
        if (ch < srcChannels) {
          const auto previousIdx = frameIdx > 0 ? frameIdx - 1 : 0;
          const std::array<double, 4> samples{
              getSample(previousIdx, ch), getSample(frameIdx, ch),
              getSample(frameIdx + 1, ch), getSample(frameIdx + 2, ch)};
          ppChannels[ch][frame] =
              static_cast<float>(cubicInterpolate(samples, t));
        } else {
          ppChannels[ch][frame] = 0.0;
        }
      }

      // Release every complete slot passed by this sample and rebase readPos
      // onto the new head. Keep the slot containing the target endpoint.
      auto consumedFrameIdx = frameIdx;
      while (mpQueueReader->numRetainedSlots() > 1) {
        const auto headFrames = (*mpQueueReader)[0]->mInfo.numFrames;
        if (consumedFrameIdx < headFrames)
          break;
        readPos -= double(headFrames);
        consumedFrameIdx -= headFrames;
        mpQueueReader->releaseSlot();
      }
    }

    *moStartReadPos = readPos + double(numFrames) * frameIncrement;

    // Publish the unread queue duration: the unconsumed portion of the head
    // plus every remaining complete slot.
    const auto &headInfo = (*mpQueueReader)[0]->mInfo;
    auto buffered = static_cast<float>(
        std::max(0.0, double(headInfo.numFrames) - *moStartReadPos) /
        double(headInfo.sampleRate));
    for (auto i = 1u; i < mpQueueReader->numRetainedSlots(); ++i) {
      const auto &info = (*mpQueueReader)[i]->mInfo;
      buffered += float(info.numFrames) / float(info.sampleRate);
    }
    mBuffered = buffered;
    mRendering.store(true, std::memory_order_relaxed);
  }

  bool hasSource() const { return mpSource != nullptr; }

  void createSource(const ChannelId &channelId) {
    mpSource = std::make_unique<LinkAudioSource>(
        mLink, channelId,
        [this](ableton::LinkAudioSource::BufferHandle bufferHandle) {
          onSourceBuffer(bufferHandle);
        });
  }

  void removeSource() {
    if (mpSource) {
      mpSource.reset();

      while (mpQueueReader->retainSlot()) {
      }
      while (mpQueueReader->numRetainedSlots() > 0) {
        mpQueueReader->releaseSlot();
      }

      moStartReadPos = std::nullopt;

      // Reset health to a clean per-connection slate. Safe here: mpSource is
      // destroyed above, so no onSourceBuffer callback can be running. Without
      // clearing mHasLastArrival, the first arrival of the next source would
      // measure its gap against this source's last arrival (seconds stale on a
      // switch/reconnect) and report a huge spurious jitter spike.
      mHasLastArrival = false;
      mJitterMs.store(0.0f, std::memory_order_relaxed);
      mDropoutCount.store(0, std::memory_order_relaxed);
      mUnmappableCount.store(0, std::memory_order_relaxed);
      mBuffered.store(0.0f, std::memory_order_relaxed);
      mRendering.store(false, std::memory_order_relaxed);
    }
  }

  float buffered() const { return mBuffered; }
  // True while blocks are actually being filled with received audio. A connected source that
  // reads false is subscribed but producing pure silence — most often because the playout
  // buffer is too small to cover the network's arrival delay, which the dropout count cannot
  // report (it only counts starves *after* playback has started).
  bool receiving() const { return mRendering.load(std::memory_order_relaxed); }
  uint32_t dropoutCount() const {
    return mDropoutCount.load(std::memory_order_relaxed);
  }
  // Buffers that arrived but were stamped in a *different Link session*, so their beat time
  // can't be mapped onto ours. Nonzero means audio is reaching us and being thrown away —
  // a state no latency setting can fix, and one that otherwise looks exactly like silence.
  uint32_t unmappableCount() const {
    return mUnmappableCount.load(std::memory_order_relaxed);
  }
  // Zero the cumulative dropout count without disturbing the stream, so a count can be read
  // as "dropouts since I last changed a setting". Just a relaxed store on the same atomic the
  // receive path increments, so it's safe to call from a control thread while audio runs.
  void resetDropoutCount() {
    mDropoutCount.store(0, std::memory_order_relaxed);
    mUnmappableCount.store(0, std::memory_order_relaxed);
  }
  float jitterMs() const { return mJitterMs.load(std::memory_order_relaxed); }

  void onSourceBuffer(const LinkAudioSource::BufferHandle bufferHandle) {
    // Network jitter estimate (RFC 3550 style): compare the actual gap between
    // arriving buffers to the gap implied by their audio duration; the smoothed
    // absolute deviation is the jitter. Runs on the Link callback thread only,
    // so the arrival-time state needs no synchronization.
    const auto now = std::chrono::steady_clock::now();
    if (mHasLastArrival && bufferHandle.info.sampleRate > 0) {
      const double actualMs =
          std::chrono::duration<double, std::milli>(now - mLastArrival).count();
      const double expectedMs = 1000.0 * double(bufferHandle.info.numFrames) /
                                double(bufferHandle.info.sampleRate);
      const double d = std::abs(actualMs - expectedMs);
      float j = mJitterMs.load(std::memory_order_relaxed);
      j += (static_cast<float>(d) - j) / 16.0f;
      mJitterMs.store(j, std::memory_order_relaxed);
    }
    mLastArrival = now;
    mHasLastArrival = true;

    if (mpQueueWriter->retainSlot()) {
      auto &buffer = *((*mpQueueWriter)[0]);
      buffer.mInfo = bufferHandle.info;
      const auto totalSamples =
          bufferHandle.info.numFrames * bufferHandle.info.numChannels;
      if (buffer.mSamples.size() < totalSamples)
        buffer.mSamples.resize(totalSamples);
      for (size_t i = 0; i < totalSamples; ++i)
        buffer.mSamples[i] =
            util::int16ToFloat<double>(bufferHandle.samples[i]);
      mpQueueWriter->releaseSlot();
    }
  }

private:
  Link &mLink;
  size_t mNumChannels;
  std::unique_ptr<LinkAudioSource> mpSource;
  double &mSampleRate;

  std::optional<double> moStartReadPos;
  std::atomic<float> mBuffered = 0;
  // Written only by the render (RT) thread, read by the control thread.
  std::atomic<bool> mRendering{false};
  std::atomic<uint32_t> mUnmappableCount{0};

  // Health metrics: dropouts (starvation underruns) counted in receive() on the
  // RT thread; jitter updated in onSourceBuffer on the Link thread; both read
  // non-RT for publishing.
  std::atomic<uint32_t> mDropoutCount{0};
  std::atomic<float> mJitterMs{0.0f};
  std::chrono::steady_clock::time_point mLastArrival{};
  bool mHasLastArrival = false;

  std::shared_ptr<typename Queue::Writer> mpQueueWriter;
  std::shared_ptr<typename Queue::Reader> mpQueueReader;
};

} // namespace linkaudio
} // namespace ableton

#else

namespace ableton {
namespace linkaudio {

template <typename Link> class LinkAudioSinkRenderer {
public:
  LinkAudioSinkRenderer(Link &, std::string, size_t, double &) {}

  void send(const float *const *, size_t, typename Link::SessionState, double,
            const std::chrono::microseconds, double) {}
};

template <typename Link> class LinkAudioSourceRenderer {
public:
  LinkAudioSourceRenderer(Link &, size_t, double &) {}

  void receive(float *const *, size_t numFrames, typename Link::SessionState,
               double, const std::chrono::microseconds, double, double) {}

  bool hasSource() const { return false; }
  template <typename ChannelId> void createSource(const ChannelId &) {}
  void removeSource() {}
  float buffered() const { return 0.0f; }
  bool receiving() const { return false; }
  uint32_t dropoutCount() const { return 0; }
  uint32_t unmappableCount() const { return 0; }
  void resetDropoutCount() {}
  float jitterMs() const { return 0.0f; }
};

} // namespace linkaudio
} // namespace ableton

#endif
