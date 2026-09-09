#pragma once

#include "recorded_pixel_source.h"
#include <filesystem>
#include <memory>

namespace pbstep2
{

class RecordingMedia final : public pbapp::RecordedPixelSource
{
public:
    static constexpr std::uint64_t maximumInputBytes = 2ULL * 1024 * 1024 * 1024;
    [[nodiscard]] static std::unique_ptr<RecordingMedia> Open(const std::filesystem::path& path, std::string& error);
    ~RecordingMedia();
    [[nodiscard]] pbapp::RecordedPixelRead ReadNext(pbapp::RecordedPixelFrame& output, std::string& error) override;
    [[nodiscard]] std::string GetIdentityJson() const;
    [[nodiscard]] static std::string GetLibraryIdentityJson();

private:
    struct Implementation;
    explicit RecordingMedia(std::unique_ptr<Implementation> implementation);
    std::unique_ptr<Implementation> implementation_;
};

} // namespace pbstep2
