// Internal no-window correctness seam, included inside pbapp so it exercises
// the same anonymous-namespace source, sender and ReceiverPipeline owners.
namespace
{

class ExperimentalPam4NativeProbe final
{
public:
    ExperimentalPam4NativeProbe(const std::uint64_t visualProfileId, const std::uint32_t width, const std::uint32_t height)
        : rasterWidth_(width), rasterHeight_(height), device_(CreateWarpReplayD3dDevice()),
        results_(std::make_unique<std::array<pbdemodd3d11::CaptureDemodulatorResult, 2>>())
    {
        pbdemodd3d11::CaptureDemodulatorConfig config;
        config.visualProfileId = visualProfileId;
        config.slotCount = 2;
        config.resultQueueCapacity = 2;
        config.maximumRoiWidth = rasterWidth_;
        config.maximumRoiHeight = rasterHeight_;
        config.maximumFrameAgeMilliseconds = 60000;
        config.offlinePixelsOnly = true;
        const auto status = pbdemodd3d11::CaptureDemodulator::Create(config, demodulator_);
        Require(static_cast<bool>(status), "PAM4 native readback creation failed: " + DescribeCaptureStatus(status));
        domain_.sourceId[0] = std::byte{0x4D};
        domain_.captureEpoch = 1;
        environment_.region.physicalRect = {0, 0, static_cast<LONG>(rasterWidth_), static_cast<LONG>(rasterHeight_)};
        environment_.region.monitorPhysicalRect = environment_.region.physicalRect;
        environment_.region.dpiX = 96;
        environment_.region.dpiY = 96;
        environment_.region.rotation = DXGI_MODE_ROTATION_IDENTITY;
        environment_.contentSize = {static_cast<std::int32_t>(rasterWidth_), static_cast<std::int32_t>(rasterHeight_)};
        environment_.sourceSize = environment_.contentSize;
        environment_.sourceRotation = DXGI_MODE_ROTATION_IDENTITY;
        environment_.pixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        environment_.adapterLuid = device_.adapterLuid;
        environment_.bitsPerColor = 8;
        environment_.backendKind = pbcapturenormalize::CaptureBackendKind::OfflinePixels;
        Start();
    }
    ~ExperimentalPam4NativeProbe()
    {
        demodulator_->DomainInvalidated(domain_);
    }
    void Recreate()
    {
        demodulator_->DomainInvalidated(domain_);
        domain_.captureEpoch++;
        Start();
    }
    [[nodiscard]] pbdemodd3d11::CaptureDemodulatorSnapshot Stop()
    {
        demodulator_->DomainInvalidated(domain_);
        return demodulator_->GetSnapshot();
    }
    [[nodiscard]] const pbdemodd3d11::CaptureDemodulatorResult& Decode(const std::span<const std::byte> pixels,
        const std::uint64_t observationOrdinal)
    {
        pbcapturenormalize::ScreenCaptureFrameMetadata metadata;
        metadata.domain = domain_;
        metadata.backend = pbcapturenormalize::CaptureBackendKind::OfflinePixels;
        metadata.captureObservation = observationOrdinal;
        metadata.sourceGeneration = domain_.captureEpoch;
        metadata.slotGeneration = observationOrdinal;
        metadata.slotIndex = static_cast<std::uint32_t>((observationOrdinal - 1) % 2);
        metadata.physicalRoi = environment_.region.physicalRect;
        metadata.sourceContentSize = environment_.contentSize;
        metadata.sourceExtent = environment_.sourceSize;
        metadata.roiSize = environment_.contentSize;
        metadata.displayRotation = DXGI_MODE_ROTATION_IDENTITY;
        metadata.sourceTransform = DXGI_MODE_ROTATION_IDENTITY;
        metadata.sourcePixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        metadata.pixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        metadata.adapterLuid = device_.adapterLuid;
        metadata.bitsPerColor = 8;
        metadata.signalEncoding = pbcapturenormalize::CaptureSignalEncoding::SdrRgb;
        metadata.timestamp.domain = pbcapturenormalize::CaptureTimestampDomain::DxgiQpcTicks;
        metadata.timestamp.rawValue = CurrentQpc100ns();
        metadata.timestamp.rawFrequency = 10000000;
        metadata.timestamp.monotonic100ns = metadata.timestamp.rawValue;
        metadata.timestamp.arrivalQpc100ns = metadata.timestamp.rawValue;
        metadata.isCursorExcluded = false;
        metadata.sourceCursorState = pbcapturenormalize::CursorState::Unknown;
        const pbrealcapturereplay::ReplayRasterView raster{rasterWidth_, rasterHeight_,
            rasterWidth_ * 4, DXGI_FORMAT_B8G8R8A8_UNORM, pixels};
        const auto texture = CreateReplayTexture(device_.device.Get(), raster);
        RunReplayProductionDemod(demodulator_, device_.device.Get(), device_.context.Get(), metadata, texture, *results_);
        return (*results_)[0];
    }
private:
    void Start()
    {
        const auto status = demodulator_->DomainStarted(domain_, environment_, device_.device.Get());
        Require(static_cast<bool>(status), "PAM4 native readback domain start failed: " + DescribeCaptureStatus(status));
    }
    const std::uint32_t rasterWidth_;
    const std::uint32_t rasterHeight_;
    const ReplayD3dDevice device_;
    std::shared_ptr<pbdemodd3d11::CaptureDemodulator> demodulator_;
    pbcapturenormalize::ScreenCaptureDomain domain_;
    pbcapturenormalize::CaptureEnvironment environment_;
    std::unique_ptr<std::array<pbdemodd3d11::CaptureDemodulatorResult, 2>> results_;
};

}

RuntimeStatus ApplicationRuntimeTestAccess::ProbeExperimentalPam4File(const std::wstring& sourcePath,
    const std::filesystem::path& sessionStateRoot, const std::wstring& outputDirectory,
    const ExperimentalPam4FileProbeOptions& options, ExperimentalPam4FileProbeSnapshot& output) noexcept
{
    output = {};
    if (sourcePath.empty() || sessionStateRoot.empty() || outputDirectory.empty() || options.maximumFrames == 0 ||
        options.maximumFrames > 2000 || options.joinAfterFrames >= options.maximumFrames ||
        options.dropEveryFrames == 1 || options.dropEveryFrames > 32 || options.duplicateEveryFrames == 1 || options.duplicateEveryFrames > 32 ||
        options.handoffFaultForTest > ExperimentalPam4ProbeFault::CorruptLastSlotTransport || !IsExperimentalPam4Family(options.visualProfile) ||
        (options.decodedFrameForTest && options.handoffFaultForTest != ExperimentalPam4ProbeFault::None) ||
        (options.recreateDomainAfterFrames != 0 && (!options.nativeReadback || options.recreateDomainAfterFrames >= options.maximumFrames)) ||
        (options.restartReceiverAfterFrames != 0 && (options.nativeReadback || options.restartReceiverAfterFrames >= options.maximumFrames)))
    {
        return RuntimeStatus::Failure("PAM4 offline probe input is outside its bounded contract");
    }
    try
    {
        std::error_code directoryError;
        Require(std::filesystem::is_directory(outputDirectory, directoryError) && !directoryError,
            "PAM4 offline probe output directory is unavailable");
        SourceFile source = ReadSourceFile(sourcePath);
        Require(source.fileBytes <= 8ULL * mebibyte, "PAM4 frequent offline fixture exceeds eight MiB");
        const ProfileBinding profile = GetProfileBinding(options.visualProfile);
        const bool wide = options.visualProfile == VisualProfile::ExperimentalPam4Wide;
        const std::uint32_t rasterWidth = wide ? pbmodulation::kExperimentalPam4WidePresentationWidth : phase1CanvasWidth;
        const std::uint32_t rasterHeight = wide ? 1600U : phase1CanvasHeight;
        std::vector<std::byte> composedPixels(wide ? static_cast<std::size_t>(rasterWidth) * rasterHeight * 4 : 0);
        const std::atomic<bool> stopRequested = false;
        auto preparation = PrepareDurableSender(source, options.compressionEnabled, 3, profile.visualProfileId,
            sessionStateRoot, stopRequested, {}, 1024U * 1024U);
        Require(!preparation.resumed, "PAM4 offline probe requires a clean Sender state root");
        const auto& description = preparation.description;
        auto& sessionStore = preparation.sessionStore;
        SenderFrameBuilder builder(profile, description, 4,
            [&source, &description, &options](const std::uint64_t segmentOrdinal)
            {
                Require(segmentOrdinal < description.segments.size(), "PAM4 Segment ordinal exceeds frozen source");
                return LoadEncodedSegment(source, description.segments[segmentOrdinal], options.compressionEnabled, 3,
                    description.session.sourceSegmentTargetBytes);
            },
            [&sessionStore](const std::uint64_t segmentOrdinal, const std::uint32_t systematicBlockCount)
            {
                return sessionStore->GetRepairIdStart(segmentOrdinal, systematicBlockCount);
            },
            [&sessionStore](const std::uint64_t segmentOrdinal, const std::uint64_t requiredExclusive)
            {
                const auto status = sessionStore->EnsureRepairIdLease(segmentOrdinal, requiredExclusive);
                Require(static_cast<bool>(status), "PAM4 durable repair lease failed: " + status.message);
            }, sessionStore->GetCarouselPass(), sessionStore->GetSegmentOrdinal(), 30);
        auto policy = options.budgetBoundDecoders ? MakeBudgetBoundUnifiedReceiverResourcePolicy() : MakeReceiverResourcePolicyForVisualProfile(profile.profile);
        if (options.memoryBudget)
        {
            Require(options.budgetBoundDecoders, "PAM4 custom memory requires budget-bound admission");
            const auto memoryStatus = ApplyDecoderMemoryBudget(*options.memoryBudget, policy);
            Require(memoryStatus == DecoderMemoryBudgetError::None, DescribeDecoderMemoryBudgetError(memoryStatus));
        }
        auto ingress = pbreceiver::ReceiverIngress::Create(policy, profile.blockBytes);
        RequireResult(ingress, "PAM4 ReceiverIngress creation failed");
        auto receiver = std::make_unique<pbreceiver::ReceiverIngress>(std::move(ingress).Value());
        SnapshotStore<DecoderSnapshot> snapshot;
        std::uint64_t runGeneration = 1;
        const auto initializeSnapshot = [&]()
        {
            snapshot.Update([&](DecoderSnapshot& value)
            {
                value = {};
                value.state = DecoderState::WaitingForBootstrap;
                value.runGeneration = runGeneration;
                value.visualProfile = profile.profile;
                value.visualProfileId = profile.visualProfileId;
                value.visualLayoutVersion = profile.layoutVersion;
                value.codedDataBytesPerFrame = profile.dataBytes;
                value.codewordsPerFrame = profile.codewords;
            });
        };
        initializeSnapshot();
        AuthoritativeCompletion completion;
        const auto started = std::chrono::steady_clock::now();
        auto pipeline = std::make_unique<ReceiverPipeline>(*receiver, outputDirectory, policy, snapshot, completion, runGeneration, started,
            profile.profile, false, false, true, nullptr, std::shared_ptr<RunMeasurementRecorder>{}, options.budgetBoundDecoders, options.memoryBudget);
        pbmodulation::ExperimentalPam4CpuDecoder decoder;
        pbmodulation::ExperimentalPam4WideCpuDecoder wideDecoder;
        if (wide)
        {
            auto decoderResult = pbmodulation::ExperimentalPam4WideCpuDecoder::Create(pbmodulation::ExperimentalPam4WideCpuDecoder::RequiredBytes());
            RequireResult(decoderResult, "PAM4 Wide CPU reference creation failed");
            wideDecoder = std::move(decoderResult).Value();
        }
        else
        {
            auto decoderResult = pbmodulation::ExperimentalPam4CpuDecoder::Create(pbmodulation::ExperimentalPam4CpuDecoder::RequiredBytes());
            RequireResult(decoderResult, "PAM4 CPU reference creation failed");
            decoder = std::move(decoderResult).Value();
        }
        const auto native = options.nativeReadback ? std::make_unique<ExperimentalPam4NativeProbe>(profile.visualProfileId, rasterWidth, rasterHeight) : nullptr;
        const auto handoff = std::make_unique<pbdemodd3d11::CaptureDemodulatorResult>();
        ExperimentalPam4FileProbeSnapshot result;
        result.segmentCount = description.segments.size();
        for (const auto& segment : description.segments)
        {
            Require(segment.inMemoryEncodedBytes.empty(), "PAM4 source prescan retained whole-file payload");
            if (segment.descriptor.outerFecMode == pbprotocol::OuterFecMode::WirehairV2)
            {
                result.wirehairSegments++;
            }
            else
            {
                result.directRepeatSegments++;
            }
        }
        const std::uint64_t firstSequence = sessionStore->GetFrameSequenceStart();
        std::uint64_t observationOrdinal = 0;
        for (std::uint32_t frameIndex = 0; frameIndex < options.maximumFrames && !pipeline->IsCompleted(); frameIndex++)
        {
            if (options.restartReceiverAfterFrames != 0 && frameIndex == options.restartReceiverAfterFrames)
            {
                Require(snapshot.Get().descriptorKnown && !completion.published, "PAM4 restart requires a bound incomplete Receiver");
                result.verifiedRawBytesBeforeRestart = snapshot.Get().verifiedRawBytes;
                result.peakReceiverDecoders = pipeline->GetPeakActiveOuterFecDecoderCount();
                result.peakReceiverOrphanBytes = pipeline->GetPeakOrphanCachedBytes();
                // Release all receive/FEC/storage owners. Only the validated local
                // journal and .part survive; no in-memory packet state is copied.
                pipeline.reset();
                receiver.reset();
                auto restartedIngress = pbreceiver::ReceiverIngress::Create(policy, profile.blockBytes);
                RequireResult(restartedIngress, "PAM4 restarted ReceiverIngress creation failed");
                receiver = std::make_unique<pbreceiver::ReceiverIngress>(std::move(restartedIngress).Value());
                runGeneration++;
                initializeSnapshot();
                completion = {};
                pipeline = std::make_unique<ReceiverPipeline>(*receiver, outputDirectory, policy, snapshot, completion, runGeneration,
                    std::chrono::steady_clock::now(), profile.profile, false, false, true, nullptr, std::shared_ptr<RunMeasurementRecorder>{}, options.budgetBoundDecoders, options.memoryBudget);
                decoder.Reset();
                wideDecoder.Reset();
                result.receiverRestarts++;
            }
            if (options.recreateDomainAfterFrames != 0 && frameIndex == options.recreateDomainAfterFrames)
            {
                native->Recreate();
                decoder.Reset();
                wideDecoder.Reset();
                pipeline->CaptureEpochReset(static_cast<std::uint64_t>(CurrentQpc100ns() / 10000));
            }
            const auto sequence = pbprotocol::CheckedAddUint64(firstSequence, frameIndex);
            const auto exclusiveSequence = sequence ? pbprotocol::CheckedAddUint64(sequence.Value(), 1) : sequence;
            RequireResult(exclusiveSequence, "PAM4 frame sequence exhausted");
            const auto lease = sessionStore->EnsureFrameSequenceLease(exclusiveSequence.Value());
            Require(static_cast<bool>(lease), "PAM4 durable frame lease failed: " + lease.message);
            const auto& canonicalPixels = builder.Build(sequence.Value(), frameIndex, static_cast<std::uint64_t>(frameIndex) * 33333333ULL);
            if (wide)
            {
                ComposeRemoteVisualFullscreenBgra(canonicalPixels, rasterWidth, rasterHeight, composedPixels, true, rasterWidth);
            }
            const auto pixels = wide ? std::span<const std::byte>(composedPixels) : std::span<const std::byte>(canonicalPixels);
            result.generatedFrames++;
            const bool skipped = frameIndex < options.joinAfterFrames ||
                (options.dropEveryFrames != 0 && frameIndex % options.dropEveryFrames == 0);
            if (skipped)
            {
                result.skippedFrames++;
            }
            else
            {
                // No sender plan/descriptor/truth is passed into the decoder.
                // Its only input is this full BGRA raster. The following fake
                // capture envelope supplies order/time, never pixel identity.
                *handoff = {};
                handoff->kind = wide ? pbdemodd3d11::CaptureDemodulatorResultKind::ExperimentalPam4WideFrame :
                    pbdemodd3d11::CaptureDemodulatorResultKind::ExperimentalPam4Frame;
                const auto DecodePixels = [&](auto& selectedDecoder, auto& observation, auto& blocks, std::uint32_t& blockCount)
                {
                    observation = selectedDecoder.Decode({pixels, rasterWidth, rasterHeight, rasterWidth * 4, pbmodulation::LumaPixelFormat::Bgra8});
                    handoff->bootstrap = observation.bootstrap;
                    blockCount = static_cast<std::uint32_t>(selectedDecoder.GetAcceptedBlocks().size());
                    std::copy(selectedDecoder.GetAcceptedBlocks().begin(), selectedDecoder.GetAcceptedBlocks().end(), blocks.begin());
                    Require(observation.IsFrameAvailable(), "clean PAM4 raster failed CPU decode");
                    Require(observation.acceptedControlRecords == 1, "clean PAM4 raster lost its fixed control slot");
                    result.maximumDataBlocksPerFrame = std::max(result.maximumDataBlocksPerFrame, observation.acceptedTransportBlocks);
                    if (observation.acceptedTransportBlocks == 0)
                    {
                        result.controlPreludeFrames++;
                    }
                    else
                    {
                        result.dataFrames++;
                    }
                };
                if (wide)
                {
                    DecodePixels(wideDecoder, handoff->pam4WideObservation, handoff->pam4WideBlocks, handoff->pam4WideBlockCount);
                }
                else
                {
                    DecodePixels(decoder, handoff->pam4Observation, handoff->pam4Blocks, handoff->pam4BlockCount);
                }
                handoff->bootstrapRecord = handoff->bootstrap.canonical44;
                handoff->geometryStatus = wide ? pbdemodd3d11::CaptureDemodulatorGeometryStatus::Letterboxed : pbdemodd3d11::CaptureDemodulatorGeometryStatus::ExactCanvas;
                result.decodedFrames++;
                handoff->metadata.domain.sourceId[0] = std::byte{0x4D};
                handoff->metadata.domain.captureEpoch = 1;
                observationOrdinal++;
                handoff->metadata.captureObservation = observationOrdinal;
                handoff->metadata.timestamp.monotonic100ns = static_cast<std::int64_t>(frameIndex + 1ULL) * 333333;
                if (native)
                {
                    const auto& actual = native->Decode(pixels, observationOrdinal);
                    Require(actual.kind == handoff->kind && actual.bootstrapRecord == handoff->bootstrapRecord &&
                        (wide ? actual.pam4WideObservation.IsFrameAvailable() && actual.pam4WideBlockCount == handoff->pam4WideBlockCount :
                            actual.pam4Observation.IsFrameAvailable() && actual.pam4BlockCount == handoff->pam4BlockCount),
                        "PAM4 native readback disagreed with CPU reference identity/count");
                    const auto actualBlocks = wide ? std::span(actual.pam4WideBlocks).first(actual.pam4WideBlockCount) : std::span(actual.pam4Blocks).first(actual.pam4BlockCount);
                    const auto expectedBlocks = wide ? std::span(handoff->pam4WideBlocks).first(handoff->pam4WideBlockCount) : std::span(handoff->pam4Blocks).first(handoff->pam4BlockCount);
                    for (std::uint32_t index = 0; index < actualBlocks.size(); index++)
                    {
                        const auto& expectedBlock = expectedBlocks[index];
                        const auto& actualBlock = actualBlocks[index];
                        Require(actualBlock.codewordSlot == expectedBlock.codewordSlot && actualBlock.kind == expectedBlock.kind &&
                            actualBlock.size == expectedBlock.size && std::equal(actualBlock.bytes.begin(), actualBlock.bytes.begin() + actualBlock.size,
                                expectedBlock.bytes.begin()), "PAM4 native readback changed accepted bytes");
                    }
                    *handoff = actual;
                }
                if (options.decodedFrameForTest)
                {
                    options.decodedFrameForTest(*handoff);
                }
                // Explicit application-boundary negative tests, never a live
                // CLI option or a substitute for pixel decode in normal runs.
                const auto InjectFault = [&](auto& observation, auto& blocks, std::uint32_t& blockCount)
                {
                    switch (options.handoffFaultForTest)
                    {
                    case ExperimentalPam4ProbeFault::None: break;
                    case ExperimentalPam4ProbeFault::ForeignObservationIdentity:
                        observation.bootstrapRecord.visualProfileId = pbprotocol::kUnifiedVisualProfileId;
                        break;
                    case ExperimentalPam4ProbeFault::DuplicateControlSlot:
                        Require(blockCount == 1, "duplicate-slot probe requires the initial Control-only raster");
                        blocks[1] = blocks[0];
                        blockCount = 2;
                        observation.acceptedBlocks = 2;
                        observation.acceptedControlRecords = 2;
                        break;
                    case ExperimentalPam4ProbeFault::OversizedBlockCount:
                        blockCount = static_cast<std::uint32_t>(blocks.size()) + 1;
                        observation.acceptedBlocks = blockCount;
                        break;
                    case ExperimentalPam4ProbeFault::ForeignSlotCount:
                        observation.frameSlotCount = 18;
                        break;
                    case ExperimentalPam4ProbeFault::ForeignResultKind:
                        handoff->kind = wide ? pbdemodd3d11::CaptureDemodulatorResultKind::ExperimentalPam4Frame :
                            pbdemodd3d11::CaptureDemodulatorResultKind::ExperimentalPam4WideFrame;
                        break;
                    case ExperimentalPam4ProbeFault::SlotOutOfRange:
                        blocks[0].codewordSlot = static_cast<std::uint8_t>(profile.codewords);
                        break;
                    case ExperimentalPam4ProbeFault::OtherProfileBlocks:
                        if (wide)
                        {
                            handoff->pam4BlockCount = 1;
                        }
                        else
                        {
                            handoff->pam4WideBlockCount = 1;
                        }
                        break;
                    case ExperimentalPam4ProbeFault::CorruptLastSlotTransport:
                        if (blockCount == profile.codewords)
                        {
                            blocks[blockCount - 1].bytes[blocks[blockCount - 1].size - 1] ^= std::byte{1};
                        }
                        break;
                    }
                };
                if (wide)
                {
                    InjectFault(handoff->pam4WideObservation, handoff->pam4WideBlocks, handoff->pam4WideBlockCount);
                }
                else
                {
                    InjectFault(handoff->pam4Observation, handoff->pam4Blocks, handoff->pam4BlockCount);
                }
                ReceiverDecisionTrace trace;
                static_cast<void>(pipeline->Process(*handoff, &trace));
                result.lastSlotReceiverCalled = result.lastSlotReceiverCalled || trace.slotDecisions[profile.codewords - 1].receiverCalled;
                if (!pipeline->IsCompleted() && options.duplicateEveryFrames != 0 && frameIndex % options.duplicateEveryFrames == 0)
                {
                    observationOrdinal++;
                    handoff->metadata.captureObservation = observationOrdinal;
                    handoff->metadata.timestamp.monotonic100ns++;
                    static_cast<void>(pipeline->Process(*handoff));
                    result.duplicateObservations++;
                }
            }
            const auto previousPass = builder.GetCarouselSnapshot().cycleCount;
            const auto previousSegment = builder.GetCheckpointSegmentOrdinal();
            builder.Advance();
            if (previousPass != builder.GetCarouselSnapshot().cycleCount || previousSegment != builder.GetCheckpointSegmentOrdinal())
            {
                const auto persisted = sessionStore->UpdateCarouselPosition(builder.GetCarouselSnapshot().cycleCount, builder.GetCheckpointSegmentOrdinal());
                Require(static_cast<bool>(persisted), "PAM4 Carousel checkpoint failed: " + persisted.message);
            }
        }
        Require(pipeline->IsCompleted() && completion.published, "PAM4 bounded pixel loop did not complete file publication");
        result.sourceStable = IsSourceStable(source);
        Require(result.sourceStable, "PAM4 source changed during the Session");
        result.peakSenderSegments = builder.GetPeakResidentEncodedSegmentCount();
        result.peakReceiverDecoders = std::max(result.peakReceiverDecoders, pipeline->GetPeakActiveOuterFecDecoderCount());
        result.peakReceiverOrphanBytes = std::max(result.peakReceiverOrphanBytes, pipeline->GetPeakOrphanCachedBytes());
        result.decoder = snapshot.Get();
        Require(result.decoder.wholeFileDigestVerified && result.decoder.finalPublishSucceeded && result.decoder.finalReopenVerified == true &&
            result.decoder.verifiedRawBytes == source.fileBytes, "PAM4 final file is not authoritative");
        // Independent full source/output byte comparison, separate from BLAKE3
        // and the storage owner's reopen. The fixture is explicitly <=8 MiB.
        SourceFile reopened = ReadSourceFile(completion.outputPathWide);
        Require(reopened.fileBytes == source.fileBytes, "PAM4 independently reopened file has a different size");
        std::array<std::byte, 65536> expected{};
        std::array<std::byte, 65536> actual{};
        LARGE_INTEGER origin{};
        Require(SetFilePointerEx(source.handle.Get(), origin, nullptr, FILE_BEGIN) != FALSE &&
            SetFilePointerEx(reopened.handle.Get(), origin, nullptr, FILE_BEGIN) != FALSE, "PAM4 independent file seek failed");
        std::uint64_t remaining = source.fileBytes;
        while (remaining != 0)
        {
            const DWORD wanted = static_cast<DWORD>((std::min)(remaining, static_cast<std::uint64_t>(expected.size())));
            DWORD expectedBytes = 0;
            DWORD actualBytes = 0;
            Require(ReadFile(source.handle.Get(), expected.data(), wanted, &expectedBytes, nullptr) != FALSE && expectedBytes == wanted &&
                ReadFile(reopened.handle.Get(), actual.data(), wanted, &actualBytes, nullptr) != FALSE && actualBytes == wanted &&
                std::equal(expected.begin(), expected.begin() + wanted, actual.begin()), "PAM4 independent full-file comparison failed");
            remaining -= wanted;
        }
        result.independentlyReopenedEqual = true;
        if (native)
        {
            result.nativeReadback = native->Stop();
            Require(pbdemodd3d11::AreCaptureDemodulatorResourcesRetired(result.nativeReadback) &&
                result.nativeReadback.stagedGpuSubmissions == 0 && result.nativeReadback.pam4DecodedObservations == result.decodedFrames,
                "PAM4 native readback retained work or claimed a GPU demodulation stage");
            result.nativeReadbackMatchedReference = true;
        }
        output = std::move(result);
        return {};
    }
    catch (const std::exception& exception)
    {
        return RuntimeStatus::Failure(exception.what());
    }
    catch (...)
    {
        return RuntimeStatus::Failure("PAM4 offline pixel-file probe failed with an unknown error");
    }
}
