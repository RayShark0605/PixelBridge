#include "local_desktop_runtime.h"
#include "run_report.h"
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

namespace pbstep2
{
namespace
{
void Check(const bool value, const char* message)
{
    if (!value)
    {
        throw std::runtime_error(message);
    }
}

// Producer-only helper. Receiver has no reference to this object or source.
class FixtureWriter
{
public:
    explicit FixtureWriter(const std::filesystem::path& path)
    {
        try
        {
            const auto utf8 = path.u8string();
            const std::string filename(utf8.begin(), utf8.end());
            Check(avformat_alloc_output_context2(&format_, nullptr, "matroska", filename.c_str()) >= 0, "Fixture muxer allocation failed");
            const auto* encoder = avcodec_find_encoder(AV_CODEC_ID_FFV1);
            Check(encoder != nullptr, "FFV1 encoder unavailable");
            codec_ = avcodec_alloc_context3(encoder);
            Check(codec_ != nullptr, "Fixture codec allocation failed");
            codec_->width = 1920;
            codec_->height = 1080;
            codec_->pix_fmt = AV_PIX_FMT_BGRA;
            codec_->time_base = {1, 60};
            codec_->framerate = {60, 1};
            codec_->thread_count = 1;
            if (format_->oformat->flags & AVFMT_GLOBALHEADER)
            {
                codec_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            }
            Check(avcodec_open2(codec_, encoder, nullptr) >= 0, "Fixture FFV1 open failed");
            stream_ = avformat_new_stream(format_, nullptr);
            Check(stream_ != nullptr, "Fixture stream allocation failed");
            stream_->time_base = codec_->time_base;
            Check(avcodec_parameters_from_context(stream_->codecpar, codec_) >= 0, "Fixture codec parameters failed");
            Check(avio_open(&format_->pb, filename.c_str(), AVIO_FLAG_WRITE) >= 0, "Fixture output open failed");
            Check(avformat_write_header(format_, nullptr) >= 0, "Fixture header failed");
            frame_ = av_frame_alloc();
            packet_ = av_packet_alloc();
            Check(frame_ != nullptr && packet_ != nullptr, "Fixture frame allocation failed");
            frame_->format = codec_->pix_fmt;
            frame_->width = codec_->width;
            frame_->height = codec_->height;
            Check(av_frame_get_buffer(frame_, 32) >= 0, "Fixture frame buffer failed");
        }
        catch (...)
        {
            Release();
            throw;
        }
    }
    ~FixtureWriter()
    {
        Release();
    }
    void Write(const pbrenderd3d::CanonicalBgraFrameView& input)
    {
        Check(count_ < 3 && input.pixels.size() == 1920ULL * 1080 * 4, "Fixture frame limit or geometry");
        Check(av_frame_make_writable(frame_) >= 0, "Fixture frame not writable");
        for (std::size_t row = 0; row < 1080; row++)
        {
            std::memcpy(frame_->data[0] + row * static_cast<std::size_t>(frame_->linesize[0]), input.pixels.data() + row * 7680, 7680);
        }
        frame_->pts = count_++;
        Check(avcodec_send_frame(codec_, frame_) >= 0, "Fixture encoding failed");
        Drain();
    }
    void Finish()
    {
        Check(avcodec_send_frame(codec_, nullptr) >= 0, "Fixture flush failed");
        Drain();
        Check(av_write_trailer(format_) >= 0 && avio_closep(&format_->pb) >= 0, "Fixture trailer/close failed");
    }

private:
    void Release() noexcept
    {
        av_packet_free(&packet_);
        av_frame_free(&frame_);
        avcodec_free_context(&codec_);
        if (format_ != nullptr)
        {
            avio_closep(&format_->pb);
            avformat_free_context(format_);
        }
    }

    void Drain()
    {
        for (;;)
        {
            const int result = avcodec_receive_packet(codec_, packet_);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
            {
                break;
            }
            Check(result >= 0, "Fixture receive packet failed");
            av_packet_rescale_ts(packet_, codec_->time_base, stream_->time_base);
            packet_->stream_index = stream_->index;
            Check(av_interleaved_write_frame(format_, packet_) >= 0, "Fixture packet write failed");
            av_packet_unref(packet_);
        }
    }
    AVFormatContext* format_ = nullptr;
    AVCodecContext* codec_ = nullptr;
    AVStream* stream_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    std::int64_t count_ = 0;
};

struct FixtureState
{
    std::atomic<bool> emitted = false;
    std::atomic<bool> stopped = false;
    FixtureWriter writer;
    explicit FixtureState(const std::filesystem::path& path) : writer(path)
    {
    }
};

class FixturePresentation final : public pbapp::EncoderPresentation
{
public:
    explicit FixturePresentation(std::shared_ptr<FixtureState> state) : state_(std::move(state))
    {
    }
    pbrenderd3d::DataWindowSnapshot GetSnapshot() const override
    {
        pbrenderd3d::DataWindowSnapshot snapshot;
        snapshot.state = state_->stopped ? pbrenderd3d::WindowState::Stopped : pbrenderd3d::WindowState::Running;
        snapshot.environment.clientWidth = 1920;
        snapshot.environment.clientHeight = 1080;
        snapshot.contract = {1920, 1080, 2, 1, pbrenderd3d::FlipEffect::Discard, true, true, true, true, true, true, true, true, true, true, true, true};
        snapshot.candidateContractSatisfied = true;
        snapshot.viewport.disposition = pbrenderd3d::PresentationViewportDisposition::Active;
        snapshot.timing.presentationEpoch = 1;
        snapshot.pendingFrame = !state_->stopped && state_->emitted;
        return snapshot;
    }
    pbrenderd3d::PresentationStatus SubmitFrame(const pbrenderd3d::CanonicalBgraFrameView& frame) override
    {
        Check(!state_->emitted, "Fixture producer emitted more than one independent frame");
        // Three captured observations of exactly one visual identity test that
        // recording replay never suppresses duplicates before production gates.
        state_->writer.Write(frame);
        state_->writer.Write(frame);
        state_->writer.Write(frame);
        state_->emitted = true;
        return {};
    }
    void RequestStop() noexcept override
    {
        state_->stopped = true;
    }
    void Stop() noexcept override
    {
        RequestStop();
    }

private:
    std::shared_ptr<FixtureState> state_;
};
} // namespace

void MakeRecordingFixture(const std::filesystem::path& root)
{
    Check(std::filesystem::create_directory(root), "Fixture root must be new");
    const auto source = root / "fixture-source.bin";
    std::array<std::byte, 512> bytes{};
    for (std::size_t index = 0; index < bytes.size(); index++)
    {
        bytes[index] = static_cast<std::byte>((index * 73 + 19) & 255);
    }
    {
        std::ofstream file(source, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        Check(file.good(), "Fixture source write failed");
    }
    const auto state = std::make_shared<FixtureState>(root / "fixture.mkv");
    pbapp::EncoderRuntime encoder([state](const pbrenderd3d::DataWindowConfig&) { return std::make_unique<FixturePresentation>(state); });
    auto config = pbapp::MakeUnifiedEncoderConfig(source.wstring(), 60);
    config.sessionStateRoot = root / "encoder-state";
    config.diagnostics = std::make_shared<pbcore::StageDiagnostics>();
    const auto status = encoder.Start(config);
    Check(static_cast<bool>(status), status.message.c_str());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!state->emitted && encoder.GetSnapshot().state != pbapp::EncoderState::Failed && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    encoder.Stop();
    Check(state->emitted && encoder.GetSnapshot().state == pbapp::EncoderState::Stopped, encoder.GetSnapshot().errorDetail.c_str());
    state->writer.Finish();
    Check(std::filesystem::file_size(root / "fixture.mkv") < 64ULL * 1024 * 1024, "Fixture media size exceeded evidence bound");
    std::ofstream report(root / "sender-report.json", std::ios::binary);
    report << pbapp::BuildEncoderRunReportJson({"Step2Fixture", "experiment", "unsealed", "unknown"}, encoder.GetSnapshot());
    report.flush();
    Check(report.good(), "Fixture sender report write failed");
}
} // namespace pbstep2
