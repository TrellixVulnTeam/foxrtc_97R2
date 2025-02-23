// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/media_source_state.h"

#include <vector>

#include "base/bind.h"
#include "base/memory/ptr_util.h"
#include "base/strings/string_number_conversions.h"
#include "media/base/gmock_callback_support.h"
#include "media/base/media_util.h"
#include "media/base/mock_filters.h"
#include "media/base/mock_media_log.h"
#include "media/base/test_helpers.h"
#include "media/filters/frame_processor.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace media {

using testing::_;
using testing::SaveArg;

namespace {

AudioDecoderConfig CreateAudioConfig(AudioCodec codec) {
  return AudioDecoderConfig(codec, kSampleFormatPlanarF32,
                            CHANNEL_LAYOUT_STEREO, 1000, EmptyExtraData(),
                            Unencrypted());
}

VideoDecoderConfig CreateVideoConfig(VideoCodec codec, int w, int h) {
  gfx::Size size(w, h);
  gfx::Rect visible_rect(size);
  return VideoDecoderConfig(codec, VIDEO_CODEC_PROFILE_UNKNOWN,
                            PIXEL_FORMAT_YV12, COLOR_SPACE_HD_REC709, size,
                            visible_rect, size, EmptyExtraData(),
                            Unencrypted());
}

void AddAudioTrack(std::unique_ptr<MediaTracks>& t, AudioCodec codec, int id) {
  t->AddAudioTrack(CreateAudioConfig(codec), id, "", "", "");
}

void AddVideoTrack(std::unique_ptr<MediaTracks>& t, VideoCodec codec, int id) {
  t->AddVideoTrack(CreateVideoConfig(codec, 16, 16), id, "", "", "");
}

void InvokeCbAndSaveResult(const base::Callback<bool()>& cb, bool* result) {
  DCHECK(result);
  *result = cb.Run();
}
}

class MediaSourceStateTest : public ::testing::Test {
 public:
  MediaSourceStateTest()
      : media_log_(new testing::StrictMock<MockMediaLog>()),
        mock_stream_parser_(nullptr) {}

  std::unique_ptr<MediaSourceState> CreateMediaSourceState() {
    std::unique_ptr<FrameProcessor> frame_processor = base::WrapUnique(
        new FrameProcessor(base::Bind(&MediaSourceStateTest::OnUpdateDuration,
                                      base::Unretained(this)),
                           media_log_));
    mock_stream_parser_ = new testing::StrictMock<MockStreamParser>();
    return base::WrapUnique(new MediaSourceState(
        base::WrapUnique(mock_stream_parser_), std::move(frame_processor),
        base::Bind(&MediaSourceStateTest::CreateDemuxerStream,
                   base::Unretained(this)),
        media_log_));
  }

  std::unique_ptr<MediaSourceState> CreateAndInitMediaSourceState(
      const std::string& expected_codecs) {
    std::unique_ptr<MediaSourceState> mss = CreateMediaSourceState();
    EXPECT_CALL(*mock_stream_parser_, Init(_, _, _, _, _, _, _, _))
        .WillOnce(SaveArg<1>(&new_config_cb_));
    mss->Init(base::Bind(&MediaSourceStateTest::SourceInitDone,
                         base::Unretained(this)),
              expected_codecs,
              base::Bind(&MediaSourceStateTest::StreamParserEncryptedInitData,
                         base::Unretained(this)),
              base::Bind(&MediaSourceStateTest::StreamParserNewTextTrack,
                         base::Unretained(this)));

    mss->SetTracksWatcher(base::Bind(
        &MediaSourceStateTest::OnMediaTracksUpdated, base::Unretained(this)));
    return mss;
  }

  // Emulates appending some data to the MediaSourceState, since OnNewConfigs
  // can only be invoked when append is in progress.
  bool AppendDataAndReportTracks(const std::unique_ptr<MediaSourceState>& mss,
                                 std::unique_ptr<MediaTracks> tracks) {
    const uint8_t stream_data[] = "stream_data";
    const int data_size = sizeof(stream_data);
    base::TimeDelta t;
    StreamParser::TextTrackConfigMap text_track_config_map;

    bool new_configs_result = false;
    base::Closure new_configs_closure =
        base::Bind(InvokeCbAndSaveResult,
                   base::Bind(new_config_cb_, base::Passed(std::move(tracks)),
                              text_track_config_map),
                   &new_configs_result);
    EXPECT_CALL(*mock_stream_parser_, Parse(stream_data, data_size))
        .WillOnce(testing::DoAll(RunClosure(new_configs_closure),
                                 testing::Return(true)));
    mss->Append(stream_data, data_size, t, t, &t);
    return new_configs_result;
  }

  MOCK_METHOD1(OnUpdateDuration, void(base::TimeDelta));

  MOCK_METHOD1(SourceInitDone, void(const StreamParser::InitParameters&));
  MOCK_METHOD2(StreamParserEncryptedInitData,
               void(EmeInitDataType, const std::vector<uint8_t>&));
  MOCK_METHOD2(StreamParserNewTextTrack,
               void(ChunkDemuxerStream*, const TextTrackConfig&));

  MOCK_METHOD1(MediaTracksUpdatedMock, void(std::unique_ptr<MediaTracks>&));
  void OnMediaTracksUpdated(std::unique_ptr<MediaTracks> tracks) {
    MediaTracksUpdatedMock(tracks);
  }

  ChunkDemuxerStream* CreateDemuxerStream(DemuxerStream::Type type) {
    static unsigned track_id = 0;
    demuxer_streams_.push_back(base::WrapUnique(
        new ChunkDemuxerStream(type, false, base::UintToString(++track_id))));
    return demuxer_streams_.back().get();
  }

  scoped_refptr<testing::StrictMock<MockMediaLog>> media_log_;
  std::vector<std::unique_ptr<ChunkDemuxerStream>> demuxer_streams_;
  MockStreamParser* mock_stream_parser_;
  StreamParser::NewConfigCB new_config_cb_;
};

TEST_F(MediaSourceStateTest, InitSingleAudioTrack) {
  std::unique_ptr<MediaSourceState> mss =
      CreateAndInitMediaSourceState("vorbis");

  std::unique_ptr<MediaTracks> tracks(new MediaTracks());
  AddAudioTrack(tracks, kCodecVorbis, 1);

  EXPECT_MEDIA_LOG(FoundStream("audio"));
  EXPECT_MEDIA_LOG(CodecName("audio", "vorbis"));
  EXPECT_CALL(*this, MediaTracksUpdatedMock(_));
  EXPECT_TRUE(AppendDataAndReportTracks(mss, std::move(tracks)));
}

TEST_F(MediaSourceStateTest, InitSingleVideoTrack) {
  std::unique_ptr<MediaSourceState> mss = CreateAndInitMediaSourceState("vp8");

  std::unique_ptr<MediaTracks> tracks(new MediaTracks());
  AddVideoTrack(tracks, kCodecVP8, 1);

  EXPECT_MEDIA_LOG(FoundStream("video"));
  EXPECT_MEDIA_LOG(CodecName("video", "vp8"));
  EXPECT_CALL(*this, MediaTracksUpdatedMock(_));
  EXPECT_TRUE(AppendDataAndReportTracks(mss, std::move(tracks)));
}

TEST_F(MediaSourceStateTest, InitMultipleTracks) {
  std::unique_ptr<MediaSourceState> mss =
      CreateAndInitMediaSourceState("vorbis,vp8,opus,vp9");

  std::unique_ptr<MediaTracks> tracks(new MediaTracks());
  AddAudioTrack(tracks, kCodecVorbis, 1);
  AddAudioTrack(tracks, kCodecOpus, 2);
  AddVideoTrack(tracks, kCodecVP8, 3);
  AddVideoTrack(tracks, kCodecVP9, 4);

  EXPECT_MEDIA_LOG(FoundStream("audio")).Times(2);
  EXPECT_MEDIA_LOG(CodecName("audio", "vorbis"));
  EXPECT_MEDIA_LOG(CodecName("audio", "opus"));
  EXPECT_MEDIA_LOG(FoundStream("video")).Times(2);
  EXPECT_MEDIA_LOG(CodecName("video", "vp8"));
  EXPECT_MEDIA_LOG(CodecName("video", "vp9"));
  EXPECT_CALL(*this, MediaTracksUpdatedMock(_));
  EXPECT_TRUE(AppendDataAndReportTracks(mss, std::move(tracks)));
}

TEST_F(MediaSourceStateTest, AudioStreamMismatchesExpectedCodecs) {
  std::unique_ptr<MediaSourceState> mss = CreateAndInitMediaSourceState("opus");
  std::unique_ptr<MediaTracks> tracks(new MediaTracks());
  AddAudioTrack(tracks, kCodecVorbis, 1);
  EXPECT_MEDIA_LOG(InitSegmentMismatchesMimeType("Audio", "vorbis"));
  EXPECT_FALSE(AppendDataAndReportTracks(mss, std::move(tracks)));
}

TEST_F(MediaSourceStateTest, VideoStreamMismatchesExpectedCodecs) {
  std::unique_ptr<MediaSourceState> mss = CreateAndInitMediaSourceState("vp9");
  std::unique_ptr<MediaTracks> tracks(new MediaTracks());
  AddVideoTrack(tracks, kCodecVP8, 1);
  EXPECT_MEDIA_LOG(InitSegmentMismatchesMimeType("Video", "vp8"));
  EXPECT_FALSE(AppendDataAndReportTracks(mss, std::move(tracks)));
}

TEST_F(MediaSourceStateTest, MissingExpectedAudioStream) {
  std::unique_ptr<MediaSourceState> mss =
      CreateAndInitMediaSourceState("opus,vp9");
  std::unique_ptr<MediaTracks> tracks(new MediaTracks());
  AddVideoTrack(tracks, kCodecVP9, 1);
  EXPECT_MEDIA_LOG(FoundStream("video"));
  EXPECT_MEDIA_LOG(CodecName("video", "vp9"));
  EXPECT_MEDIA_LOG(InitSegmentMissesExpectedTrack("opus"));
  EXPECT_FALSE(AppendDataAndReportTracks(mss, std::move(tracks)));
}

TEST_F(MediaSourceStateTest, MissingExpectedVideoStream) {
  std::unique_ptr<MediaSourceState> mss =
      CreateAndInitMediaSourceState("opus,vp9");
  std::unique_ptr<MediaTracks> tracks(new MediaTracks());
  tracks->AddAudioTrack(CreateAudioConfig(kCodecOpus), 1, "", "", "");
  EXPECT_MEDIA_LOG(FoundStream("audio"));
  EXPECT_MEDIA_LOG(CodecName("audio", "opus"));
  EXPECT_MEDIA_LOG(InitSegmentMissesExpectedTrack("vp9"));
  EXPECT_FALSE(AppendDataAndReportTracks(mss, std::move(tracks)));
}

TEST_F(MediaSourceStateTest, TrackIdsChangeInSecondInitSegment) {
  std::unique_ptr<MediaSourceState> mss =
      CreateAndInitMediaSourceState("opus,vp9");

  std::unique_ptr<MediaTracks> tracks(new MediaTracks());
  AddAudioTrack(tracks, kCodecOpus, 1);
  AddVideoTrack(tracks, kCodecVP9, 2);
  EXPECT_MEDIA_LOG(FoundStream("audio"));
  EXPECT_MEDIA_LOG(CodecName("audio", "opus"));
  EXPECT_MEDIA_LOG(FoundStream("video"));
  EXPECT_MEDIA_LOG(CodecName("video", "vp9"));
  EXPECT_CALL(*this, MediaTracksUpdatedMock(_));
  AppendDataAndReportTracks(mss, std::move(tracks));

  // This second set of tracks have bytestream track ids that differ from the
  // first init segment above (audio track id 1 -> 3, video track id 2 -> 4).
  // Bytestream track ids are allowed to change when there is only a single
  // track of each type.
  std::unique_ptr<MediaTracks> tracks2(new MediaTracks());
  AddAudioTrack(tracks2, kCodecOpus, 3);
  AddVideoTrack(tracks2, kCodecVP9, 4);
  EXPECT_CALL(*this, MediaTracksUpdatedMock(_));
  AppendDataAndReportTracks(mss, std::move(tracks2));
}

TEST_F(MediaSourceStateTest, TrackIdChangeWithTwoAudioTracks) {
  std::unique_ptr<MediaSourceState> mss =
      CreateAndInitMediaSourceState("vorbis,opus");

  std::unique_ptr<MediaTracks> tracks(new MediaTracks());
  AddAudioTrack(tracks, kCodecVorbis, 1);
  AddAudioTrack(tracks, kCodecOpus, 2);
  EXPECT_MEDIA_LOG(FoundStream("audio")).Times(2);
  EXPECT_MEDIA_LOG(CodecName("audio", "vorbis"));
  EXPECT_MEDIA_LOG(CodecName("audio", "opus"));
  EXPECT_CALL(*this, MediaTracksUpdatedMock(_));
  EXPECT_TRUE(AppendDataAndReportTracks(mss, std::move(tracks)));

  // Since we have two audio tracks, bytestream track ids must match the first
  // init segment.
  std::unique_ptr<MediaTracks> tracks2(new MediaTracks());
  AddAudioTrack(tracks2, kCodecVorbis, 1);
  AddAudioTrack(tracks2, kCodecOpus, 2);
  EXPECT_CALL(*this, MediaTracksUpdatedMock(_));
  EXPECT_TRUE(AppendDataAndReportTracks(mss, std::move(tracks2)));

  // Emulate the situation where bytestream track ids have changed in the third
  // init segment. This must cause failure in the OnNewConfigs.
  std::unique_ptr<MediaTracks> tracks3(new MediaTracks());
  AddAudioTrack(tracks3, kCodecVorbis, 1);
  AddAudioTrack(tracks3, kCodecOpus, 3);
  EXPECT_MEDIA_LOG(UnexpectedTrack("audio", "3"));
  EXPECT_FALSE(AppendDataAndReportTracks(mss, std::move(tracks3)));
}

TEST_F(MediaSourceStateTest, TrackIdChangeWithTwoVideoTracks) {
  std::unique_ptr<MediaSourceState> mss =
      CreateAndInitMediaSourceState("vp8,vp9");

  std::unique_ptr<MediaTracks> tracks(new MediaTracks());
  AddVideoTrack(tracks, kCodecVP8, 1);
  AddVideoTrack(tracks, kCodecVP9, 2);
  EXPECT_MEDIA_LOG(FoundStream("video")).Times(2);
  EXPECT_MEDIA_LOG(CodecName("video", "vp8"));
  EXPECT_MEDIA_LOG(CodecName("video", "vp9"));
  EXPECT_CALL(*this, MediaTracksUpdatedMock(_));
  EXPECT_TRUE(AppendDataAndReportTracks(mss, std::move(tracks)));

  // Since we have two video tracks, bytestream track ids must match the first
  // init segment.
  std::unique_ptr<MediaTracks> tracks2(new MediaTracks());
  AddVideoTrack(tracks2, kCodecVP8, 1);
  AddVideoTrack(tracks2, kCodecVP9, 2);
  EXPECT_CALL(*this, MediaTracksUpdatedMock(_));
  EXPECT_TRUE(AppendDataAndReportTracks(mss, std::move(tracks2)));

  // Emulate the situation where bytestream track ids have changed in the third
  // init segment. This must cause failure in the OnNewConfigs.
  std::unique_ptr<MediaTracks> tracks3(new MediaTracks());
  AddVideoTrack(tracks3, kCodecVP8, 1);
  AddVideoTrack(tracks3, kCodecVP9, 3);
  EXPECT_MEDIA_LOG(UnexpectedTrack("video", "3"));
  EXPECT_FALSE(AppendDataAndReportTracks(mss, std::move(tracks3)));
}

}  // namespace media
