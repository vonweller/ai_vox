#include "audio_output_engine.h"

#ifndef CLOGGER_SEVERITY
#define CLOGGER_SEVERITY CLOGGER_SEVERITY_WARN
#endif
#include "clogger/clogger.h"

#ifdef ARDUINO
#include "libopus/opus.h"
#else
#include "opus.h"
#endif

namespace {
constexpr uint32_t kDefaultSampleRate = 24000;
constexpr uint32_t kDefaultChannels = 1;
constexpr uint32_t kDefaultDurationMs = 20;  // Duration in milliseconds
constexpr uint32_t kDefaultFrameSize = kDefaultSampleRate / 1000 * kDefaultChannels * kDefaultDurationMs;
}  // namespace

AudioOutputEngine::AudioOutputEngine(std::shared_ptr<ai_vox::AudioOutputDevice> audio_output_device, const uint32_t frame_duration)
    : audio_output_device_(std::move(audio_output_device)), samples_(kDefaultSampleRate / 1000 * kDefaultChannels * frame_duration) {
  int error = -1;
  opus_decoder_ = opus_decoder_create(kDefaultSampleRate, kDefaultChannels, &error);
  assert(opus_decoder_ != nullptr);
  audio_output_device_->Open(kDefaultSampleRate);

  uint32_t stack_size = 9 << 10;
  task_queue_ = new TaskQueue("AudioOutput", stack_size, tskIDLE_PRIORITY + 1);
  CLOGI("OK");
}

AudioOutputEngine::~AudioOutputEngine() {
  delete task_queue_;
  audio_output_device_->Close();
  opus_decoder_destroy(opus_decoder_);
}

void AudioOutputEngine::Write(FlexArray<uint8_t>&& data) {
  task_queue_->Enqueue([this, data = std::move(data)]() mutable { ProcessData(std::move(data)); });
}

void AudioOutputEngine::NotifyDataEnd(std::function<void()>&& callback) {
  task_queue_->Enqueue(std::move(callback));
}

void AudioOutputEngine::ProcessData(FlexArray<uint8_t>&& data) {
  auto pcm = new int16_t[samples_];
  const auto ret = opus_decode(opus_decoder_, data.data(), data.size(), pcm, samples_, 0);
  if (ret >= 0) {
    audio_output_device_->Write(pcm, samples_);
  }
  delete[] pcm;
}