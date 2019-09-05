/*
 *  Copyright (c) 2019 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "test/pc/e2e/analyzer/video/video_quality_analyzer_injection_helper.h"

#include <utility>

#include "absl/memory/memory.h"
#include "test/pc/e2e/analyzer/video/quality_analyzing_video_decoder.h"
#include "test/pc/e2e/analyzer/video/quality_analyzing_video_encoder.h"
#include "test/pc/e2e/analyzer/video/simulcast_dummy_buffer_helper.h"

namespace webrtc {
namespace webrtc_pc_e2e {

namespace {

class VideoFrameInterceptor {
 public:
  virtual ~VideoFrameInterceptor() = default;

  // Performs desired actions with video frame. It may change video frame.
  virtual void OnVideoFrame(VideoFrame* frame) = 0;
};

class VideoAnalyzerCapturingInterceptor : public VideoFrameInterceptor {
 public:
  VideoAnalyzerCapturingInterceptor(std::string stream_label,
                                    VideoQualityAnalyzerInterface* analyzer)
      : stream_label_(std::move(stream_label)), analyzer_(analyzer) {
    RTC_DCHECK(analyzer_);
  }
  ~VideoAnalyzerCapturingInterceptor() override = default;

  void OnVideoFrame(VideoFrame* frame) override {
    uint16_t frame_id = analyzer_->OnFrameCaptured(stream_label_, *frame);
    frame->set_id(frame_id);
  }

 private:
  const std::string stream_label_;
  VideoQualityAnalyzerInterface* analyzer_;
};

class VideoWriterInterceptor : public VideoFrameInterceptor {
 public:
  VideoWriterInterceptor(test::VideoFrameWriter* video_writer)
      : video_writer_(video_writer) {}
  ~VideoWriterInterceptor() override = default;

  void OnVideoFrame(VideoFrame* frame) override {
    bool result = video_writer_->WriteFrame(*frame);
    RTC_CHECK(result) << "Failed to write frame";
  }

 private:
  test::VideoFrameWriter* video_writer_;
};

// Intercepts generated frames and passes them also to video quality analyzer
// and into video frame writer, if the last one is provided.
class InterceptingFrameGenerator : public test::FrameGenerator {
 public:
  InterceptingFrameGenerator(
      std::unique_ptr<test::FrameGenerator> delegate,
      std::vector<std::unique_ptr<VideoFrameInterceptor>> interceptors)
      : delegate_(std::move(delegate)),
        interceptors_(std::move(interceptors)) {}
  ~InterceptingFrameGenerator() override = default;

  VideoFrame* NextFrame() override {
    VideoFrame* frame = delegate_->NextFrame();
    for (auto& interceptor : interceptors_) {
      interceptor->OnVideoFrame(frame);
    }
    return frame;
  }

  void ChangeResolution(size_t width, size_t height) override {
    delegate_->ChangeResolution(width, height);
  }

 private:
  std::unique_ptr<test::FrameGenerator> delegate_;
  std::vector<std::unique_ptr<VideoFrameInterceptor>> interceptors_;
};

// Implements the video sink, that forwards rendered frames to the video quality
// analyzer and to the video frame writer, if the last one is provided.
class AnalyzingVideoSink : public rtc::VideoSinkInterface<VideoFrame> {
 public:
  AnalyzingVideoSink(VideoQualityAnalyzerInterface* analyzer,
                     test::VideoFrameWriter* video_writer)
      : analyzer_(analyzer), video_writer_(video_writer) {
    RTC_DCHECK(analyzer_);
  }
  ~AnalyzingVideoSink() override = default;

  void OnFrame(const VideoFrame& frame) override {
    if (IsDummyFrameBuffer(frame.video_frame_buffer()->ToI420())) {
      // This is dummy frame, so we  don't need to process it further.
      return;
    }
    analyzer_->OnFrameRendered(frame);
    if (video_writer_) {
      bool result = video_writer_->WriteFrame(frame);
      RTC_CHECK(result) << "Failed to write frame";
    }
  }
  void OnDiscardedFrame() override {}

 private:
  VideoQualityAnalyzerInterface* analyzer_;
  test::VideoFrameWriter* video_writer_;
};

}  // namespace

VideoQualityAnalyzerInjectionHelper::VideoQualityAnalyzerInjectionHelper(
    std::unique_ptr<VideoQualityAnalyzerInterface> analyzer,
    EncodedImageDataInjector* injector,
    EncodedImageDataExtractor* extractor)
    : analyzer_(std::move(analyzer)),
      injector_(injector),
      extractor_(extractor),
      encoding_entities_id_generator_(absl::make_unique<IntIdGenerator>(1)) {
  RTC_DCHECK(injector_);
  RTC_DCHECK(extractor_);
}
VideoQualityAnalyzerInjectionHelper::~VideoQualityAnalyzerInjectionHelper() =
    default;

std::unique_ptr<VideoEncoderFactory>
VideoQualityAnalyzerInjectionHelper::WrapVideoEncoderFactory(
    std::unique_ptr<VideoEncoderFactory> delegate,
    double bitrate_multiplier,
    std::map<std::string, absl::optional<int>> stream_required_spatial_index)
    const {
  return absl::make_unique<QualityAnalyzingVideoEncoderFactory>(
      std::move(delegate), bitrate_multiplier,
      std::move(stream_required_spatial_index),
      encoding_entities_id_generator_.get(), injector_, analyzer_.get());
}

std::unique_ptr<VideoDecoderFactory>
VideoQualityAnalyzerInjectionHelper::WrapVideoDecoderFactory(
    std::unique_ptr<VideoDecoderFactory> delegate) const {
  return absl::make_unique<QualityAnalyzingVideoDecoderFactory>(
      std::move(delegate), encoding_entities_id_generator_.get(), extractor_,
      analyzer_.get());
}

std::unique_ptr<test::FrameGenerator>
VideoQualityAnalyzerInjectionHelper::WrapFrameGenerator(
    std::string stream_label,
    std::unique_ptr<test::FrameGenerator> delegate,
    test::VideoFrameWriter* writer) const {
  std::vector<std::unique_ptr<VideoFrameInterceptor>> interceptors;
  interceptors.push_back(absl::make_unique<VideoAnalyzerCapturingInterceptor>(
      std::move(stream_label), analyzer_.get()));
  if (writer) {
    interceptors.push_back(absl::make_unique<VideoWriterInterceptor>(writer));
  }
  return absl::make_unique<InterceptingFrameGenerator>(std::move(delegate),
                                                       std::move(interceptors));
}

std::unique_ptr<rtc::VideoSinkInterface<VideoFrame>>
VideoQualityAnalyzerInjectionHelper::CreateVideoSink(
    test::VideoFrameWriter* writer) const {
  return absl::make_unique<AnalyzingVideoSink>(analyzer_.get(), writer);
}

void VideoQualityAnalyzerInjectionHelper::Start(std::string test_case_name,
                                                int max_threads_count) {
  analyzer_->Start(std::move(test_case_name), max_threads_count);
}

void VideoQualityAnalyzerInjectionHelper::OnStatsReports(
    const std::string& pc_label,
    const StatsReports& stats_reports) {
  analyzer_->OnStatsReports(pc_label, stats_reports);
}

void VideoQualityAnalyzerInjectionHelper::Stop() {
  analyzer_->Stop();
}

}  // namespace webrtc_pc_e2e
}  // namespace webrtc
