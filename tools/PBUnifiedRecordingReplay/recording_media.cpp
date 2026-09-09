#include "recording_media.h"
#include "pbprotocol/blake3_digest.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstdarg>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace pbstep2
{
namespace
{
// Tool-only FFmpeg callback: decoder thread_count=1. Error logging can signal
// truncated Matroska even when av_read_frame subsequently returns EOF.
thread_local bool mediaLoggedError = false;
void ObserveMediaLog(void*, const int level, const char*, va_list)
{
    if (level <= AV_LOG_ERROR)
    {
        mediaLoggedError = true;
    }
}

constexpr int canvasWidth = 1920;
constexpr int canvasHeight = 1080;
constexpr std::uint64_t maximumPackets = 50000;
constexpr std::uint64_t maximumFrames = 7200;
constexpr std::uint64_t maximumIoBytes = 4ULL * 1024 * 1024 * 1024;
constexpr int maximumPacketBytes = 16 * 1024 * 1024;

void Check(const int result, const char* const stage)
{
    if (result < 0)
    {
        std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
        av_strerror(result, buffer.data(), buffer.size());
        throw std::runtime_error(std::string(stage) + ": " + buffer.data());
    }
}

void Require(const bool condition, const char* const message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}
} // namespace

struct RecordingMedia::Implementation
{
    HANDLE file = INVALID_HANDLE_VALUE;
    AVFormatContext* format = nullptr;
    AVIOContext* io = nullptr;
    AVCodecContext* codec = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    SwsContext* scaler = nullptr;
    std::vector<std::byte> pixels;
    std::uint64_t inputBytes = 0;
    std::uint64_t ioBytes = 0;
    std::uint64_t packets = 0;
    std::uint64_t frames = 0;
    std::uint64_t skippedPackets = 0;
    int videoStream = -1;
    AVRational timeBase{};
    bool flushed = false;
    bool failed = false;
    bool eof = false;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    std::string sourceBlake3;
    std::string pixelContract;

    ~Implementation()
    {
        sws_freeContext(scaler);
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&codec);
        avformat_close_input(&format);
        if (io != nullptr)
        {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
        if (file != INVALID_HANDLE_VALUE)
        {
            CloseHandle(file);
        }
    }

    static int Interrupted(void* const opaque)
    {
        const auto& state = *static_cast<Implementation*>(opaque);
        return std::chrono::steady_clock::now() - state.started > std::chrono::seconds(900) ? 1 : 0;
    }

    static int Read(void* const opaque, std::uint8_t* const bytes, const int requested)
    {
        auto& state = *static_cast<Implementation*>(opaque);
        if (requested <= 0 || Interrupted(opaque) || state.ioBytes >= maximumIoBytes)
        {
            return AVERROR(EIO);
        }
        const auto permitted = static_cast<DWORD>(std::min<std::uint64_t>(static_cast<std::uint64_t>(requested), maximumIoBytes - state.ioBytes));
        DWORD read = 0;
        if (!ReadFile(state.file, bytes, permitted, &read, nullptr))
        {
            return AVERROR(EIO);
        }
        state.ioBytes += read;
        return read == 0 ? AVERROR_EOF : static_cast<int>(read);
    }

    static std::int64_t Seek(void* const opaque, const std::int64_t offset, const int whence)
    {
        auto& state = *static_cast<Implementation*>(opaque);
        if (whence == AVSEEK_SIZE)
        {
            return static_cast<std::int64_t>(state.inputBytes);
        }
        if (Interrupted(opaque) || (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END))
        {
            return AVERROR(EINVAL);
        }
        LARGE_INTEGER current{};
        LARGE_INTEGER zero{};
        if (!SetFilePointerEx(state.file, zero, &current, FILE_CURRENT))
        {
            return AVERROR(EIO);
        }
        const std::int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? current.QuadPart : static_cast<std::int64_t>(state.inputBytes);
        if (offset < -base || offset > static_cast<std::int64_t>(state.inputBytes) - base)
        {
            return AVERROR(EINVAL);
        }
        LARGE_INTEGER target{};
        target.QuadPart = base + offset;
        return SetFilePointerEx(state.file, target, nullptr, FILE_BEGIN) ? target.QuadPart : AVERROR(EIO);
    }

    static int DenyOpen(AVFormatContext*, AVIOContext**, const char*, int, AVDictionary**)
    {
        return AVERROR(EPERM);
    }

    void Convert()
    {
        Require(frame->width == canvasWidth && frame->height == canvasHeight, "Only exact 1920x1080 recording pixels are supported");
        Require(frame->pts != AV_NOPTS_VALUE, "Missing original frame PTS; no nominal-FPS reconstruction permitted");
        Require((frame->flags & AV_FRAME_FLAG_CORRUPT) == 0 && frame->decode_error_flags == 0, "Corrupt decoded frame");
        Require(frame->crop_top == 0 && frame->crop_bottom == 0 && frame->crop_left == 0 && frame->crop_right == 0, "Implicit decoder crop is not permitted");
        const auto pixelFormat = static_cast<AVPixelFormat>(frame->format);
        const bool yuv = pixelFormat == AV_PIX_FMT_YUV420P;
        const bool rgb = pixelFormat == AV_PIX_FMT_BGRA || pixelFormat == AV_PIX_FMT_BGR0;
        Require(yuv || rgb, "Unsupported pixel format; explicit color contract required");
        if (yuv)
        {
            Require(frame->color_range == AVCOL_RANGE_MPEG && frame->colorspace == AVCOL_SPC_BT709 && frame->color_primaries == AVCOL_PRI_BT709 &&
                        frame->color_trc == AVCOL_TRC_BT709 && frame->chroma_location == AVCHROMA_LOC_LEFT,
                "Unsupported or unspecified YUV color contract");
        }
        const std::string contract = yuv ? "YUV420P-Limited-BT709-Left-To-BGRA-Full" : pixelFormat == AV_PIX_FMT_BGRA ? "BGRA-Identity" : "BGR0-To-BGRA-Opaque";
        Require(pixelContract.empty() || pixelContract == contract, "Midstream color/pixel-format change");
        pixelContract = contract;
        if (rgb)
        {
            Require(frame->linesize[0] >= canvasWidth * 4, "Invalid RGB row pitch");
            for (int row = 0; row < canvasHeight; row++)
            {
                std::copy_n(reinterpret_cast<const std::byte*>(frame->data[0] + static_cast<std::size_t>(row) * frame->linesize[0]), canvasWidth * 4,
                    pixels.begin() + static_cast<std::ptrdiff_t>(row) * canvasWidth * 4);
            }
            for (std::size_t offset = 3; offset < pixels.size(); offset += 4)
            {
                pixels[offset] = std::byte{255};
            }
            return;
        }
        if (scaler == nullptr)
        {
            scaler = sws_alloc_context();
            Require(scaler != nullptr, "sws_alloc_context failed");
            Check(av_opt_set_int(scaler, "srcw", canvasWidth, 0), "srcw");
            Check(av_opt_set_int(scaler, "srch", canvasHeight, 0), "srch");
            Check(av_opt_set_int(scaler, "dstw", canvasWidth, 0), "dstw");
            Check(av_opt_set_int(scaler, "dsth", canvasHeight, 0), "dsth");
            Check(av_opt_set_int(scaler, "src_format", pixelFormat, 0), "src_format");
            Check(av_opt_set_int(scaler, "dst_format", AV_PIX_FMT_BGRA, 0), "dst_format");
            Check(av_opt_set_int(scaler, "sws_flags", SWS_BILINEAR | SWS_ACCURATE_RND | SWS_BITEXACT | SWS_FULL_CHR_H_INT, 0), "sws_flags");
            Check(av_opt_set_int(scaler, "src_h_chr_pos", 0, 0), "left chroma x");
            Check(av_opt_set_int(scaler, "src_v_chr_pos", 128, 0), "left chroma y");
            Check(sws_init_context(scaler, nullptr, nullptr), "sws_init_context");
            const int* const coefficients = sws_getCoefficients(SWS_CS_ITU709);
            Check(sws_setColorspaceDetails(scaler, coefficients, 0, coefficients, 1, 0, 1 << 16, 1 << 16), "BT709 range conversion");
        }
        std::array<std::uint8_t*, 4> destination{reinterpret_cast<std::uint8_t*>(pixels.data()), nullptr, nullptr, nullptr};
        const std::array<int, 4> strides{canvasWidth * 4, 0, 0, 0};
        Require(sws_scale(scaler, frame->data, frame->linesize, 0, canvasHeight, destination.data(), strides.data()) == canvasHeight, "Incomplete BGRA conversion");
    }
};

RecordingMedia::RecordingMedia(std::unique_ptr<Implementation> implementation) : implementation_(std::move(implementation))
{
}
RecordingMedia::~RecordingMedia() = default;

std::unique_ptr<RecordingMedia> RecordingMedia::Open(const std::filesystem::path& path, std::string& error)
{
    mediaLoggedError = false;
    av_log_set_callback(ObserveMediaLog);
    try
    {
        const auto absolute = std::filesystem::absolute(path).lexically_normal();
        Require(absolute.has_root_name() && absolute.root_name().wstring().size() == 2 && GetDriveTypeW(absolute.root_path().c_str()) == DRIVE_FIXED,
            "Recording must be an explicit local fixed-drive file");
        auto state = std::make_unique<Implementation>();
        state->file = CreateFileW(absolute.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Require(state->file != INVALID_HANDLE_VALUE, "Cannot lock recording against mutation");
        LARGE_INTEGER size{};
        Require(GetFileType(state->file) == FILE_TYPE_DISK && GetFileSizeEx(state->file, &size) && size.QuadPart > 0 &&
                    static_cast<std::uint64_t>(size.QuadPart) <= maximumInputBytes,
            "Recording input byte limit");
        state->inputBytes = static_cast<std::uint64_t>(size.QuadPart);
        pbprotocol::Blake3Hasher hasher;
        std::array<std::byte, 65536> buffer{};
        DWORD read = 0;
        do
        {
            Require(ReadFile(state->file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) != FALSE, "Input hash read failed");
            hasher.Update(std::span(buffer).first(read));
        } while (read != 0);
        const auto digest = hasher.Finalize();
        constexpr char hex[] = "0123456789abcdef";
        for (const auto value : digest)
        {
            const auto byte = std::to_integer<unsigned int>(value);
            state->sourceBlake3 += hex[byte >> 4];
            state->sourceBlake3 += hex[byte & 15];
        }
        Check(static_cast<int>(Implementation::Seek(state.get(), 0, SEEK_SET)), "Input rewind");
        auto* const ioBuffer = static_cast<std::uint8_t*>(av_malloc(32768));
        Require(ioBuffer != nullptr, "AVIO allocation failed");
        state->io = avio_alloc_context(ioBuffer, 32768, 0, state.get(), Implementation::Read, nullptr, Implementation::Seek);
        if (state->io == nullptr)
        {
            av_free(ioBuffer);
            throw std::runtime_error("AVIO context failed");
        }
        state->format = avformat_alloc_context();
        Require(state->format != nullptr, "AVFormat allocation failed");
        state->format->pb = state->io;
        state->format->flags |= AVFMT_FLAG_CUSTOM_IO;
        state->format->io_open = Implementation::DenyOpen;
        state->format->interrupt_callback = {Implementation::Interrupted, state.get()};
        state->format->max_streams = 4;
        state->format->max_probe_packets = 128;
        state->format->probesize = 4 * 1024 * 1024;
        state->format->max_analyze_duration = AV_TIME_BASE;
        state->format->error_recognition = AV_EF_EXPLODE;
        const auto* const inputFormat = av_find_input_format("matroska");
        Require(inputFormat != nullptr, "Pinned Matroska demuxer unavailable");
        Check(avformat_open_input(&state->format, nullptr, inputFormat, nullptr), "Open Matroska");
        Check(avformat_find_stream_info(state->format, nullptr), "Read stream information");
        Require(!mediaLoggedError, "FFmpeg reported a demux/header error");
        for (unsigned int index = 0; index < state->format->nb_streams; index++)
        {
            if (state->format->streams[index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            {
                Require(state->videoStream == -1, "Ambiguous multiple video streams");
                state->videoStream = static_cast<int>(index);
            }
        }
        Require(state->videoStream >= 0, "No video stream");
        const auto* const stream = state->format->streams[state->videoStream];
        const auto* const parameters = stream->codecpar;
        Require(parameters->width == canvasWidth && parameters->height == canvasHeight, "Recording dimensions outside fixed canvas");
        Require(parameters->codec_id == AV_CODEC_ID_H264 || parameters->codec_id == AV_CODEC_ID_FFV1, "Only H264 recording and FFV1 lossless fixture supported");
        Require(parameters->extradata_size >= 0 && parameters->extradata_size <= 1024 * 1024, "Codec extradata limit");
        state->timeBase = stream->time_base;
        Require(state->timeBase.num > 0 && state->timeBase.den > 0, "Invalid time base");
        const auto* const decoder = avcodec_find_decoder(parameters->codec_id);
        Require(decoder != nullptr, "Pinned software decoder unavailable");
        state->codec = avcodec_alloc_context3(decoder);
        Require(state->codec != nullptr, "Codec allocation failed");
        Check(avcodec_parameters_to_context(state->codec, parameters), "Codec parameters");
        state->codec->thread_count = 1;
        state->codec->max_pixels = static_cast<std::int64_t>(canvasWidth) * (canvasHeight + 16);
        state->codec->err_recognition = AV_EF_CRCCHECK | AV_EF_EXPLODE;
        state->codec->pkt_timebase = state->timeBase;
        Check(avcodec_open2(state->codec, decoder, nullptr), "Open software decoder");
        state->packet = av_packet_alloc();
        state->frame = av_frame_alloc();
        Require(state->packet != nullptr && state->frame != nullptr, "Packet/frame allocation failed");
        state->pixels.resize(static_cast<std::size_t>(canvasWidth) * canvasHeight * 4);
        return std::unique_ptr<RecordingMedia>(new RecordingMedia(std::move(state)));
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return {};
    }
}

pbapp::RecordedPixelRead RecordingMedia::ReadNext(pbapp::RecordedPixelFrame& output, std::string& error)
{
    auto& state = *implementation_;
    try
    {
        Require(!state.failed, "Media reader already failed");
        if (state.eof)
        {
            return pbapp::RecordedPixelRead::EndOfFile;
        }
        Require(!Implementation::Interrupted(&state), "Media time limit");
        for (;;)
        {
            av_frame_unref(state.frame);
            const int received = avcodec_receive_frame(state.codec, state.frame);
            Require(!mediaLoggedError, "FFmpeg reported a decode error");
            if (received == 0)
            {
                Require(state.frames < maximumFrames, "Media frame limit");
                state.Convert();
                state.frames++;
                output = {state.pixels, canvasWidth, canvasHeight, canvasWidth * 4, state.frame->pts, state.timeBase.num, state.timeBase.den, state.frame->duration};
                return pbapp::RecordedPixelRead::Frame;
            }
            if (received == AVERROR_EOF)
            {
                state.eof = true;
                return pbapp::RecordedPixelRead::EndOfFile;
            }
            Check(received == AVERROR(EAGAIN) ? 0 : received, "Receive frame");
            Require(!state.flushed, "Decoder requested packets after flush");
            av_packet_unref(state.packet);
            const int read = av_read_frame(state.format, state.packet);
            Require(!mediaLoggedError, "FFmpeg reported a demux error or truncated input");
            if (read == AVERROR_EOF)
            {
                Check(avcodec_send_packet(state.codec, nullptr), "Flush decoder");
                state.flushed = true;
                continue;
            }
            Check(read, "Read packet");
            Require(state.packets < maximumPackets && state.packet->size >= 0 && state.packet->size <= maximumPacketBytes, "Packet count/size limit");
            state.packets++;
            if (state.packet->stream_index != state.videoStream)
            {
                state.skippedPackets++;
                continue;
            }
            Require((state.packet->flags & AV_PKT_FLAG_CORRUPT) == 0, "Corrupt packet");
            Check(avcodec_send_packet(state.codec, state.packet), "Send packet");
        }
    }
    catch (const std::exception& exception)
    {
        state.failed = true;
        error = exception.what();
        return pbapp::RecordedPixelRead::Error;
    }
}

std::string RecordingMedia::GetLibraryIdentityJson()
{
    std::ostringstream stream;
    stream << "{\"ffmpegVersion\":\"" << av_version_info() << "\",\"avformat\":" << avformat_version() << ",\"avcodec\":" << avcodec_version()
           << ",\"avutil\":" << avutil_version() << ",\"swscale\":" << swscale_version() << ",\"decoderThreads\":1,\"networkInput\":false}";
    return stream.str();
}

std::string RecordingMedia::GetIdentityJson() const
{
    const auto& state = *implementation_;
    std::ostringstream stream;
    stream << "{\"sourceBlake3\":\"" << state.sourceBlake3 << "\",\"inputBytes\":" << state.inputBytes << ",\"decodedFrames\":" << state.frames
           << ",\"packets\":" << state.packets << ",\"skippedNonVideoPackets\":" << state.skippedPackets << ",\"ioBytes\":" << state.ioBytes << ",\"pixelContract\":\""
           << state.pixelContract << "\",\"codecId\":" << state.codec->codec_id << ",\"timeBaseNumerator\":" << state.timeBase.num
           << ",\"timeBaseDenominator\":" << state.timeBase.den << ",\"decoderEof\":" << (state.eof ? "true" : "false") << ",\"library\":" << GetLibraryIdentityJson()
           << '}';
    return stream.str();
}

} // namespace pbstep2
