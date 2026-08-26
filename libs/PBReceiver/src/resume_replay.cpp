#include "pbreceiver/resume_replay.h"

#include "pbprotocol/checked_integer.h"
#include "pbprotocol/descriptor_codec.h"

#include <algorithm>
#include <fstream>
#include <new>
#include <stdexcept>
#include <system_error>

namespace pbreceiver {

namespace {

[[nodiscard]] ReceiverResult<ResumeReplayOutcome> ReplayFailed(
    const pbouterfec::OuterFecError& error) noexcept
{
    return ReceiverResult<ResumeReplayOutcome>::Failure(error);
}

} // namespace

ReceiverResult<ResumeReplayOutcome> ReplayActiveWirehairCache(
    const pbprotocol::ResumeActiveWirehairCacheRecord& cacheRecord,
    pbouterfec::WirehairV2Decoder& decoder)
{
    ResumeReplayOutcome outcome;
    // An empty (moved-from) decoder exposes the zero-filled sentinel profile:
    // it has no bound state, so no record can be valid against it and replay
    // fails closed with the root-cause code before any metadata comparison.
    const pbprotocol::WirehairV2SerializedProfile boundProfile =
        decoder.GetBoundSerializedProfile();
    if (std::all_of(
            boundProfile.bytes.begin(),
            boundProfile.bytes.end(),
            [](const std::byte value) noexcept { return value == std::byte{0}; }))
    {
        return ReplayFailed(pbouterfec::OuterFecError{
            pbouterfec::OuterFecErrorCode::InvalidState, 0});
    }
    // Trust boundary: resume.state is untrusted persistent input, so the
    // persisted profile snapshot must match the decoder's descriptor-bound
    // profile before any block is re-injected (design doc section 31.2).
    // A mismatch fails closed without consuming the decoder.
    if (cacheRecord.wirehairProfile != boundProfile)
    {
        return ReplayFailed(pbouterfec::OuterFecError{
            pbouterfec::OuterFecErrorCode::UnsupportedProfile, 0});
    }

    for (const auto& entry : cacheRecord.entries)
    {
        const auto decodeResult = decoder.DecodeBlock(
            entry.outerBlockId,
            std::span<const std::byte>(entry.payload));
        if (!decodeResult)
        {
            return ReplayFailed(decodeResult.Error());
        }
        outcome.replayedEntryCount++;
        if (decodeResult.Value() == pbouterfec::DecodeDisposition::Ready)
        {
            // Ready is sticky in the decoder: once recovered it never reverts.
            outcome.decoderReady = true;
        }
    }
    return ReceiverResult<ResumeReplayOutcome>::Success(std::move(outcome));
}

ReceiverResult<ResumeReplayOutcome> ReplayDirectRepeatBlocks(
    const pbprotocol::ResumeActiveDirectRepeatRecord& directRepeatRecord,
    pbouterfec::DirectRepeatDecoder& decoder)
{
    ResumeReplayOutcome outcome;
    // An empty (moved-from) decoder reports a zero bound block count: it has
    // no bound state, so no record can be valid against it and replay fails
    // closed with the root-cause code before any metadata comparison.
    const std::uint64_t boundBlockCount = decoder.GetBoundBlockCount();
    if (boundBlockCount == 0)
    {
        return ReplayFailed(pbouterfec::OuterFecError{
            pbouterfec::OuterFecErrorCode::InvalidState, 0});
    }
    // Trust boundary: the persisted block count must equal the
    // descriptor-derived bound count before any block is re-injected (design
    // doc section 31.3); per-entry revalidation alone cannot catch an
    // under-reported count.
    if (static_cast<std::uint64_t>(directRepeatRecord.directBlockCount) !=
        boundBlockCount)
    {
        return ReplayFailed(pbouterfec::OuterFecError{
            pbouterfec::OuterFecErrorCode::InvalidInput,
            static_cast<std::uint64_t>(directRepeatRecord.directBlockCount)});
    }

    for (const auto& entry : directRepeatRecord.entries)
    {
        const auto decodeResult = decoder.DecodeBlock(
            entry.blockOrdinal,
            entry.realPayloadBytes,
            std::span<const std::byte>(entry.paddedPayload));
        if (!decodeResult)
        {
            return ReplayFailed(decodeResult.Error());
        }
        outcome.replayedEntryCount++;
        if (decodeResult.Value() == pbouterfec::DecodeDisposition::Ready)
        {
            outcome.decoderReady = true;
        }
    }
    return ReceiverResult<ResumeReplayOutcome>::Success(std::move(outcome));
}

ReceiverResult<std::vector<std::byte>> ReadResumeStateFile(
    const std::filesystem::path& filePath,
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy)
{
    using DocumentResult = ReceiverResult<std::vector<std::byte>>;

    const auto policyStatus =
        pbprotocol::ValidateReceiverResourcePolicy(resourcePolicy);
    if (!policyStatus)
    {
        return DocumentResult::Failure(
            pbprotocol::ProtocolError{
                policyStatus.Error().code,
                policyStatus.Error().offset});
    }

    // Stat before any allocation or read: an over-budget file must be rejected
    // on size alone so a hostile or stale resume.state cannot force memory.
    std::error_code errorCode;
    const bool isRegularFile =
        std::filesystem::is_regular_file(filePath, errorCode);
    if (errorCode || !isRegularFile)
    {
        return DocumentResult::Failure(
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::ResumeStateIoFailure, 0});
    }

    const auto fileSize = std::filesystem::file_size(filePath, errorCode);
    if (errorCode)
    {
        return DocumentResult::Failure(
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::ResumeStateIoFailure, 0});
    }

    const auto fileSizeBytesResult =
        pbprotocol::CheckedNarrowUnsigned<std::uint64_t>(fileSize);
    if (!fileSizeBytesResult)
    {
        return DocumentResult::Failure(
            pbprotocol::ProtocolError{
                fileSizeBytesResult.Error().code, 0});
    }
    if (fileSizeBytesResult.Value() > resourcePolicy.maxResumeBytes)
    {
        // Rejected before the file is opened or any buffer allocated.
        return DocumentResult::Failure(
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::ResourceLimitExceeded, 0});
    }

    const auto fileSizeInBytesResult =
        pbprotocol::CheckedUint64ToSize(fileSizeBytesResult.Value());
    if (!fileSizeInBytesResult)
    {
        return DocumentResult::Failure(
            pbprotocol::ProtocolError{
                fileSizeInBytesResult.Error().code, 0});
    }

    std::vector<std::byte> document;
    try
    {
        document.resize(fileSizeInBytesResult.Value());
    }
    catch (const std::bad_alloc&)
    {
        return DocumentResult::Failure(
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::ResourceExhausted, 0});
    }
    catch (const std::length_error&)
    {
        return DocumentResult::Failure(
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::ResourceExhausted, 0});
    }

    if (!document.empty())
    {
        std::ifstream fileStream(filePath, std::ios::binary);
        if (!fileStream.is_open())
        {
            return DocumentResult::Failure(
                pbprotocol::ProtocolError{
                    pbprotocol::ProtocolErrorCode::ResumeStateIoFailure, 0});
        }
        // Exact-size binary read: a short read is torn or racing state and
        // fails closed instead of being treated as valid content.
        fileStream.read(
            reinterpret_cast<char*>(document.data()),
            static_cast<std::streamsize>(fileSizeInBytesResult.Value()));
        // A negative or short gcount means torn/racing state; fail closed.
        const std::streamsize bytesRead = fileStream.gcount();
        if (bytesRead < 0 || static_cast<std::uint64_t>(bytesRead) != fileSizeBytesResult.Value())
        {
            return DocumentResult::Failure(
                pbprotocol::ProtocolError{
                    pbprotocol::ProtocolErrorCode::TruncatedInput, 0});
        }
    }

    return DocumentResult::Success(std::move(document));
}

ReceiverResult<std::size_t> WriteResumeStateFile(
    const std::filesystem::path& filePath,
    const std::span<const std::byte> documentBytes)
{
    using WriteResult = ReceiverResult<std::size_t>;

    // Plain FastResume-phase write: no flush ordering or atomic replace. The
    // caller is responsible for passing a budget-validated document; torn tail
    // writes are the documented failure mode that LoadResumeState tolerates at
    // the file end and flags via hasTruncatedTail.
    std::ofstream fileStream(
        filePath, std::ios::binary | std::ios::trunc);
    if (!fileStream.is_open())
    {
        return WriteResult::Failure(
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::ResumeStateIoFailure, 0});
    }

    const auto documentSizeResult =
        pbprotocol::CheckedNarrowUnsigned<std::uint64_t>(documentBytes.size());
    if (!documentSizeResult)
    {
        return WriteResult::Failure(
            pbprotocol::ProtocolError{
                documentSizeResult.Error().code, 0});
    }

    fileStream.write(
        reinterpret_cast<const char*>(documentBytes.data()),
        static_cast<std::streamsize>(documentSizeResult.Value()));
    // The buffered tail is committed only by flush/close; success is
    // reported only after the data is flushed and the stream is closed, so a
    // torn write never masquerades as persisted state.
    fileStream.flush();
    if (fileStream.fail())
    {
        return WriteResult::Failure(
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::ResumeStateIoFailure, 0});
    }
    fileStream.close();
    if (fileStream.fail())
    {
        return WriteResult::Failure(
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::ResumeStateIoFailure, 0});
    }

    return WriteResult::Success(documentBytes.size());
}

} // namespace pbreceiver