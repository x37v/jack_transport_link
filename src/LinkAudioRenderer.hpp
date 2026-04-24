#pragma once

#include <chrono>
#include <cstddef>

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
class LinkAudioRenderer
{
  struct Buffer
  {
    std::vector<double> mSamples; // pre-sized; covers max network buffer * max channels
    LinkAudioSource::BufferHandle::Info mInfo;
  };
  using Queue = link_audio::Queue<Buffer>;

public:
  LinkAudioRenderer(Link& link,
                    size_t numInChannels,
                    size_t numOutChannels,
                    double& sampleRate)
    : mLink(link)
    , mNumInChannels(numInChannels)
    , mNumOutChannels(numOutChannels)
    , mSink(mLink, "Link Audio", 4096 * numInChannels)
    , mSampleRate(sampleRate)
    , mReceiverSampleCaches(numOutChannels, {0.0, 0.0, 0.0, 0.0})
  {
    Buffer proto;
    proto.mSamples.resize(1024 * 8); // generous: 1024 frames * 8 channels
    auto queue = Queue(2048, proto);
    mpQueueWriter = std::make_shared<typename Queue::Writer>(std::move(queue.writer()));
    mpQueueReader = std::make_shared<typename Queue::Reader>(std::move(queue.reader()));
  }

  ~LinkAudioRenderer() { mpSource.reset(); }

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
        for (size_t ch = 0; ch < mNumInChannels; ++ch)
          buffer.samples[frame * mNumInChannels + ch] =
              ableton::util::floatToInt16(ppChannels[ch][frame]);

      const auto beatsAtBufferBegin = sessionState.beatAtTime(hostTime, quantum);
      buffer.commit(sessionState,
                    beatsAtBufferBegin,
                    quantum,
                    numFrames,
                    mNumInChannels,
                    static_cast<uint32_t>(sampleRate));
    }
  }

  void receive(double* const* ppChannels,
               size_t numFrames,
               typename Link::SessionState sessionState,
               double sampleRate,
               const std::chrono::microseconds hostTime,
               double quantum)
  {
    auto silenceOutputs = [&]() {
      for (size_t ch = 0; ch < mNumOutChannels; ++ch)
        std::fill_n(ppChannels[ch], numFrames, 0.0);
    };

    while (mpQueueReader->retainSlot())
    {
    }

    constexpr auto kLatencyInBeats = 4;
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
      silenceOutputs();
      moLastFrameIdx = std::nullopt;
      moStartReadPos = std::nullopt;
      mBuffered = 0;
      return;
    }

    totalFrames -= startFramePos;

    if (totalFrames <= 0.0)
    {
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

      // Advance all per-channel caches together until moLastFrameIdx reaches frameIdx
      while (!moLastFrameIdx || (moLastFrameIdx && frameIdx > *moLastFrameIdx))
      {
        for (size_t ch = 0; ch < mNumOutChannels; ++ch)
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

      for (size_t ch = 0; ch < mNumOutChannels; ++ch)
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

  void operator()(double* const* ppSendChannels,
                  double* const* ppRecvChannels,
                  size_t numFrames,
                  typename Link::SessionState sessionState,
                  double sampleRate,
                  const std::chrono::microseconds hostTime,
                  double quantum)
  {
    send(ppSendChannels, numFrames, sessionState, sampleRate, hostTime, quantum);
    receive(ppRecvChannels, numFrames, sessionState, sampleRate, hostTime, quantum);
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
    }
  }

  float buffered() const { return mBuffered; }

  void onSourceBuffer(const LinkAudioSource::BufferHandle bufferHandle)
  {
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

  Link& mLink;
  size_t mNumInChannels;
  size_t mNumOutChannels;
  LinkAudioSink mSink;
  std::unique_ptr<LinkAudioSource> mpSource;
  double& mSampleRate;

  std::optional<double> moStartReadPos;
  std::atomic<float> mBuffered = 0;

private:
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
class LinkAudioRenderer
{
public:
  LinkAudioRenderer(Link&, size_t, size_t, double&) {}

  void operator()(double* const*,
                  double* const*,
                  size_t numFrames,
                  typename Link::SessionState,
                  double,
                  const std::chrono::microseconds,
                  double) {}

  bool hasSource() const { return false; }
  template <typename ChannelId>
  void createSource(const ChannelId&) {}
  void removeSource() {}
  float buffered() const { return 0.0f; }
};

} // namespace linkaudio
} // namespace ableton

#endif
