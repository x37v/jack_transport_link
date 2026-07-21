#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>

#if defined(LINK_AUDIO)

#include <ableton/LinkAudio.hpp>
#include <ableton/link_audio/Buffer.hpp>
#include <ableton/link_audio/Queue.hpp>
#include <ableton/util/FloatIntConversion.hpp>

namespace ableton
{
namespace linkaudio
{

namespace
{

template <typename T>
T cubicInterpolate(const std::array<T, 4>& p, double t)
{
  double a = -0.5 * static_cast<double>(p[0]) + 1.5 * static_cast<double>(p[1])
             - 1.5 * static_cast<double>(p[2]) + 0.5 * static_cast<double>(p[3]);
  double b = static_cast<double>(p[0]) - 2.5 * static_cast<double>(p[1])
             + 2.0 * static_cast<double>(p[2]) - 0.5 * static_cast<double>(p[3]);
  double c = -0.5 * static_cast<double>(p[0]) + 0.5 * static_cast<double>(p[2]);
  auto d = static_cast<double>(p[1]);
  return static_cast<T>(a * t * t * t + b * t * t + c * t + d);
}

template <typename T>
T linearInterpolate(T value, T inMin, T inMax, T outMin, T outMax)
{
  return (value - inMin) * (outMax - outMin) / (inMax - inMin) + outMin;
}

} // namespace

template <typename Link>
class LinkAudioSinkRenderer
{
public:
  LinkAudioSinkRenderer(Link& link,
                        std::string name,
                        size_t numChannels,
                        double& sampleRate)
    : mLink(link)
    , mNumChannels(numChannels)
    , mSink(mLink, std::move(name), 4096 * numChannels)
    , mSampleRate(sampleRate)
  {}

  void send(double* const* ppChannels,
            size_t numFrames,
            typename Link::SessionState sessionState,
            double sampleRate,
            const std::chrono::microseconds hostTime,
            double quantum)
  {
    auto buffer = LinkAudioSink::BufferHandle(mSink);
    if (buffer)
    {
      for (size_t frame = 0; frame < numFrames; ++frame)
        for (size_t ch = 0; ch < mNumChannels; ++ch)
          buffer.samples[frame * mNumChannels + ch] =
              ableton::util::floatToInt16(ppChannels[ch][frame]);

      const auto beatsAtBufferBegin = sessionState.beatAtTime(hostTime, quantum);
      buffer.commit(sessionState,
                    beatsAtBufferBegin,
                    quantum,
                    numFrames,
                    mNumChannels,
                    static_cast<uint32_t>(sampleRate));
    }
  }

private:
  Link& mLink;
  size_t mNumChannels;
  LinkAudioSink mSink;
  double& mSampleRate;
};

template <typename Link>
class LinkAudioSourceRenderer
{
  struct Buffer
  {
    std::vector<double> mSamples;
    LinkAudioSource::BufferHandle::Info mInfo;
  };
  using Queue = link_audio::Queue<Buffer>;

public:
  LinkAudioSourceRenderer(Link& link,
                          size_t numChannels,
                          double& sampleRate)
    : mLink(link)
    , mNumChannels(numChannels)
    , mSampleRate(sampleRate)
    , mReceiverSampleCaches(numChannels, {0.0, 0.0, 0.0, 0.0})
  {
    Buffer proto;
    proto.mSamples.resize(1024 * 8);
    auto queue = Queue(2048, proto);
    mpQueueWriter = std::make_shared<typename Queue::Writer>(std::move(queue.writer()));
    mpQueueReader = std::make_shared<typename Queue::Reader>(std::move(queue.reader()));
  }

  ~LinkAudioSourceRenderer() { mpSource.reset(); }

  void receive(double* const* ppChannels,
               size_t numFrames,
               typename Link::SessionState sessionState,
               double sampleRate,
               const std::chrono::microseconds hostTime,
               double quantum,
               double latencyMs)
  {
    auto silenceOutputs = [&]() {
      for (size_t ch = 0; ch < mNumChannels; ++ch)
        std::fill_n(ppChannels[ch], numFrames, 0.0);
    };

    while (mpQueueReader->retainSlot())
    {
    }

    // We were mid-stream if a read position is already established; producing silence from here
    // then means the queue starved (a real dropout), as opposed to normal pre-roll silence.
    const bool wasRendering = moStartReadPos.has_value();

    // Playout buffer expressed in milliseconds, converted to beats at the current tempo — the
    // same scheme Ableton Live and Max use (latency_beats = (ms/1000) * (bpm/60)). Expressing it
    // in ms keeps the real-time buffer depth constant regardless of tempo (a fixed beat count
    // would shrink in real time as the tempo rises).
    const double kLatencyInBeats = (latencyMs / 1000.0) * (sessionState.tempo() / 60.0);
    const auto targetBeatsAtBufferBegin =
      sessionState.beatAtTime(hostTime, quantum) - kLatencyInBeats;
    const auto targetBeatsAtBufferEnd =
      sessionState.beatAtTime(
        hostTime
          + std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::duration<double>(double(numFrames) / sampleRate)),
        quantum)
      - kLatencyInBeats;

    while (!moStartReadPos && mpQueueReader->numRetainedSlots() > 0)
    {
      if ((*mpQueueReader)[0]->mInfo.endBeats(sessionState, quantum)
          < targetBeatsAtBufferBegin)
      {
        mpQueueReader->releaseSlot();
      }
      else
      {
        break;
      }
    }

    if (mpQueueReader->numRetainedSlots() == 0)
    {
      if (wasRendering) mDropoutCount.fetch_add(1, std::memory_order_relaxed);
      silenceOutputs();
      moLastFrameIdx = std::nullopt;
      moStartReadPos = std::nullopt;
      mBuffered = 0;
      return;
    }

    if (!moStartReadPos
        && (*mpQueueReader)[0]->mInfo.beginBeats(sessionState, quantum)
             > targetBeatsAtBufferBegin)
    {
      silenceOutputs();
      moLastFrameIdx = std::nullopt;
      moStartReadPos = std::nullopt;
      mBuffered = 0;
      return;
    }

    if (!moStartReadPos)
    {
      const auto& info = (*mpQueueReader)[0]->mInfo;
      const auto startBufferBegin = *info.beginBeats(sessionState, quantum);
      const auto startBufferEnd = *info.endBeats(sessionState, quantum);

      moStartReadPos = linearInterpolate(targetBeatsAtBufferBegin,
                                         startBufferBegin,
                                         startBufferEnd,
                                         0.0,
                                         double(info.numFrames));
    }

    const auto startFramePos = *moStartReadPos;

    auto totalFrames = 0.0;
    auto foundEnd = false;

    for (auto i = 0u; i < mpQueueReader->numRetainedSlots(); ++i)
    {
      const auto& info = (*mpQueueReader)[i]->mInfo;
      const auto bufferBegin = *info.beginBeats(sessionState, quantum);
      const auto bufferEnd = *info.endBeats(sessionState, quantum);

      if (targetBeatsAtBufferEnd >= bufferBegin && targetBeatsAtBufferEnd < bufferEnd)
      {
        totalFrames += linearInterpolate(
          targetBeatsAtBufferEnd, bufferBegin, bufferEnd, 0.0, double(info.numFrames));
        foundEnd = true;
        break;
      }
      else
      {
        totalFrames += double(info.numFrames);
      }
    }

    if (!foundEnd)
    {
      if (wasRendering) mDropoutCount.fetch_add(1, std::memory_order_relaxed);
      silenceOutputs();
      moLastFrameIdx = std::nullopt;
      moStartReadPos = std::nullopt;
      mBuffered = 0;
      return;
    }

    totalFrames -= startFramePos;

    if (totalFrames <= 0.0)
    {
      if (wasRendering) mDropoutCount.fetch_add(1, std::memory_order_relaxed);
      silenceOutputs();
      moLastFrameIdx = std::nullopt;
      moStartReadPos = std::nullopt;
      mBuffered = 0;
      return;
    }

    const auto frameIncrement = totalFrames / double(numFrames);
    auto readPos = startFramePos;

    const size_t srcChannels = (*mpQueueReader)[0]->mInfo.numChannels;

    auto getSample = [&](size_t idx, size_t ch) -> double {
      size_t bufferIdx = 0;
      size_t currentIdx = idx;
      while (bufferIdx < mpQueueReader->numRetainedSlots())
      {
        auto& currentBuffer = *((*mpQueueReader)[bufferIdx]);
        if (currentIdx < currentBuffer.mInfo.numFrames)
        {
          const size_t bufSrcCh = currentBuffer.mInfo.numChannels;
          return (ch < bufSrcCh) ? currentBuffer.mSamples[currentIdx * bufSrcCh + ch] : 0.0;
        }
        currentIdx -= currentBuffer.mInfo.numFrames;
        ++bufferIdx;
      }
      return 0.0;
    };

    for (auto frame = 0u; frame < numFrames; ++frame)
    {
      const auto framePos = readPos + frame * frameIncrement;
      const auto frameIdx = static_cast<size_t>(std::floor(framePos));
      const auto t = framePos - std::floor(framePos);

      while (!moLastFrameIdx || (moLastFrameIdx && frameIdx > *moLastFrameIdx))
      {
        for (size_t ch = 0; ch < mNumChannels; ++ch)
        {
          auto& cache = mReceiverSampleCaches[ch];
          cache[3] = cache[2];
          cache[2] = cache[1];
          cache[1] = cache[0];
          cache[0] = (ch < srcChannels)
              ? ((frameIdx > 0) ? getSample(frameIdx - 1, ch) : getSample(0, ch))
              : 0.0;
        }
        moLastFrameIdx = moLastFrameIdx ? (*moLastFrameIdx + 1) : frameIdx;
      }

      for (size_t ch = 0; ch < mNumChannels; ++ch)
      {
        ppChannels[ch][frame] = (ch < srcChannels)
            ? cubicInterpolate(mReceiverSampleCaches[ch], t)
            : 0.0;
      }

      const auto& currentInfo = (*mpQueueReader)[0]->mInfo;
      if (frameIdx >= currentInfo.numFrames)
      {
        readPos -= double(currentInfo.numFrames);
        moLastFrameIdx = frameIdx - currentInfo.numFrames;
        mpQueueReader->releaseSlot();
      }
    }

    *moStartReadPos = readPos + double(numFrames) * frameIncrement;

    auto buffered =
      -static_cast<float>(*moStartReadPos) / float((*mpQueueReader)[0]->mInfo.sampleRate);
    for (auto i = 1u; i < mpQueueReader->numRetainedSlots(); ++i)
    {
      const auto& info = (*mpQueueReader)[i]->mInfo;
      buffered += float(info.numFrames) / float(info.sampleRate);
    }
    mBuffered = buffered;
  }

  bool hasSource() const { return mpSource != nullptr; }

  void createSource(const ChannelId& channelId)
  {
    mpSource = std::make_unique<LinkAudioSource>(
      mLink,
      channelId,
      [this](ableton::LinkAudioSource::BufferHandle bufferHandle) {
        onSourceBuffer(bufferHandle);
      });
  }

  void removeSource()
  {
    if (mpSource)
    {
      mpSource.reset();

      while (mpQueueReader->retainSlot())
      {
      }
      while (mpQueueReader->numRetainedSlots() > 0)
      {
        mpQueueReader->releaseSlot();
      }

      moLastFrameIdx = std::nullopt;
      moStartReadPos = std::nullopt;

      // Reset health to a clean per-connection slate. Safe here: mpSource is destroyed above,
      // so no onSourceBuffer callback can be running. Without clearing mHasLastArrival, the
      // first arrival of the next source would measure its gap against this source's last
      // arrival (seconds stale on a switch/reconnect) and report a huge spurious jitter spike.
      mHasLastArrival = false;
      mJitterMs.store(0.0f, std::memory_order_relaxed);
      mDropoutCount.store(0, std::memory_order_relaxed);
      mBuffered.store(0.0f, std::memory_order_relaxed);
    }
  }

  float buffered() const { return mBuffered; }
  uint32_t dropoutCount() const { return mDropoutCount.load(std::memory_order_relaxed); }
  float jitterMs() const { return mJitterMs.load(std::memory_order_relaxed); }

  void onSourceBuffer(const LinkAudioSource::BufferHandle bufferHandle)
  {
    // Network jitter estimate (RFC 3550 style): compare the actual gap between arriving buffers
    // to the gap implied by their audio duration; the smoothed absolute deviation is the jitter.
    // Runs on the Link callback thread only, so the arrival-time state needs no synchronization.
    const auto now = std::chrono::steady_clock::now();
    if (mHasLastArrival && bufferHandle.info.sampleRate > 0)
    {
      const double actualMs =
        std::chrono::duration<double, std::milli>(now - mLastArrival).count();
      const double expectedMs = 1000.0 * double(bufferHandle.info.numFrames)
                                / double(bufferHandle.info.sampleRate);
      const double d = std::abs(actualMs - expectedMs);
      float j = mJitterMs.load(std::memory_order_relaxed);
      j += (static_cast<float>(d) - j) / 16.0f;
      mJitterMs.store(j, std::memory_order_relaxed);
    }
    mLastArrival = now;
    mHasLastArrival = true;

    if (mpQueueWriter->retainSlot())
    {
      auto& buffer = *((*mpQueueWriter)[0]);
      buffer.mInfo = bufferHandle.info;
      const auto totalSamples = bufferHandle.info.numFrames * bufferHandle.info.numChannels;
      if (buffer.mSamples.size() < totalSamples)
        buffer.mSamples.resize(totalSamples);
      for (size_t i = 0; i < totalSamples; ++i)
        buffer.mSamples[i] = util::int16ToFloat<double>(bufferHandle.samples[i]);
      mpQueueWriter->releaseSlot();
    }
  }

private:
  Link& mLink;
  size_t mNumChannels;
  std::unique_ptr<LinkAudioSource> mpSource;
  double& mSampleRate;

  std::optional<double> moStartReadPos;
  std::atomic<float> mBuffered = 0;

  // Health metrics: dropouts (starvation underruns) counted in receive() on the RT thread;
  // jitter updated in onSourceBuffer on the Link thread; both read non-RT for publishing.
  std::atomic<uint32_t> mDropoutCount{0};
  std::atomic<float> mJitterMs{0.0f};
  std::chrono::steady_clock::time_point mLastArrival{};
  bool mHasLastArrival = false;

  std::shared_ptr<typename Queue::Writer> mpQueueWriter;
  std::shared_ptr<typename Queue::Reader> mpQueueReader;

  std::vector<std::array<double, 4>> mReceiverSampleCaches;
  std::optional<size_t> moLastFrameIdx = std::nullopt;
};

} // namespace linkaudio
} // namespace ableton

#else

namespace ableton
{
namespace linkaudio
{

template <typename Link>
class LinkAudioSinkRenderer
{
public:
  LinkAudioSinkRenderer(Link&, std::string, size_t, double&) {}

  void send(double* const*,
            size_t,
            typename Link::SessionState,
            double,
            const std::chrono::microseconds,
            double) {}
};

template <typename Link>
class LinkAudioSourceRenderer
{
public:
  LinkAudioSourceRenderer(Link&, size_t, double&) {}

  void receive(double* const*,
               size_t numFrames,
               typename Link::SessionState,
               double,
               const std::chrono::microseconds,
               double,
               double) {}

  bool hasSource() const { return false; }
  template <typename ChannelId>
  void createSource(const ChannelId&) {}
  void removeSource() {}
  float buffered() const { return 0.0f; }
  uint32_t dropoutCount() const { return 0; }
  float jitterMs() const { return 0.0f; }
};

} // namespace linkaudio
} // namespace ableton

#endif
