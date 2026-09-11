// Included inside the runtime's private namespace to exercise the actual
// durable sender, mixed-slot builder and storage/journal ReceiverPipeline.
void RunUnifiedWindowTransitionProbe(const std::wstring& sourcePath,
    const std::filesystem::path& sessionStateRoot, const std::wstring& outputDirectory,
    const std::span<const std::uint32_t> erasedBands, const std::uint32_t biasedPhase,
    UnifiedWindowTransitionProbeSnapshot& result)
{
    constexpr std::uint64_t segmentCount = 16;
    constexpr std::uint64_t windowSize = senderUnifiedActiveSegmentWindowSize;
    // Incremental fountain repair passes deliver fewer equations per Carousel
    // wrap, so recovery under the loss projection spans more (cheaper) wraps
    // than the historical K+20% FullRepairPass budget did.
    constexpr std::uint64_t maximumFrames = 24000;
    constexpr std::uint64_t lossBandCount = 516;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(360);
    Require(biasedPhase < windowSize && erasedBands.size() <= lossBandCount &&
        std::ranges::all_of(erasedBands, [](const std::uint32_t band) { return band < lossBandCount; }),
        "Pass-0 loss projection exceeds its fixed bounds");
    SourceFile source = ReadSourceFile(sourcePath);
    Require(source.fileBytes == segmentCount * pbprotocol::kDefaultSourceSegmentTargetBytes,
        "Pass-0 transition probe requires exactly 16 full-size Segments");
    const ProfileBinding profile = GetProfileBinding(VisualProfile::UnifiedLc4);
    const pbprotocol::ReceiverResourcePolicy policy = MakeReceiverResourcePolicyForVisualProfile(VisualProfile::UnifiedLc4);
    Require(policy.maxActiveOuterFecDecoders == senderUnifiedReceiverActiveDecoderLimit &&
        windowSize < policy.maxActiveOuterFecDecoders,
        "Pass-0 probe must retain the decoder policy strictly above the sender window");
    std::atomic<bool> stopRequested = false;
    DurableSenderPreparation preparation = PrepareDurableSender(source, false, 3, profile.visualProfileId,
        sessionStateRoot, stopRequested);
    Require(!preparation.resumed, "Pass-0 probe requires fresh durable sender state");
    const TransferDescription& description = preparation.description;
    std::unique_ptr<EncoderSessionStore>& sessionStore = preparation.sessionStore;
    Require(description.segments.size() == segmentCount, "Pass-0 probe descriptor count changed");
    result.sourceBytes = source.fileBytes;
    result.acceptedSymbols.resize(segmentCount);
    result.completedAtFrame.resize(segmentCount);
    SenderFrameBuilder builder(profile, description, 4,
        [&source, &description](const std::uint64_t ordinal)
        {
            return LoadEncodedSegment(source, description.segments.at(static_cast<std::size_t>(ordinal)), false, 3);
        },
        [&sessionStore](const std::uint64_t ordinal, const std::uint32_t blockCount)
        {
            return sessionStore->GetRepairIdStart(ordinal, blockCount);
        },
        [&sessionStore](const std::uint64_t ordinal, const std::uint64_t requiredExclusive)
        {
            const auto status = sessionStore->EnsureRepairIdLease(ordinal, requiredExclusive);
            Require(static_cast<bool>(status), "Pass-0 durable repair lease failed: " + status.message);
        }, 0, 0, 15);
    auto createdReceiver = pbreceiver::ReceiverIngress::Create(policy, outerBlockBytes);
    RequireResult(createdReceiver, "Pass-0 Receiver creation failed");
    pbreceiver::ReceiverIngress receiver = std::move(createdReceiver).Value();
    SnapshotStore<DecoderSnapshot> snapshot;
    snapshot.Update([&profile](DecoderSnapshot& value)
    {
        value.state = DecoderState::WaitingForBootstrap;
        value.runGeneration = 1;
        value.visualProfile = VisualProfile::UnifiedLc4;
        value.visualProfileId = profile.visualProfileId;
        value.visualLayoutVersion = profile.layoutVersion;
        value.codedDataBytesPerFrame = profile.dataBytes;
        value.codewordsPerFrame = profile.codewords;
    });
    AuthoritativeCompletion completion;
    ReceiverPipeline pipeline(receiver, outputDirectory, policy, snapshot, completion, 1,
        std::chrono::steady_clock::now(), VisualProfile::UnifiedLc4, false, false, true);
    const auto RecordResources = [&]()
    {
        const auto telemetry = receiver.GetTelemetry();
        const DecoderSnapshot currentSnapshot = snapshot.Get();
        result.peakActiveDecoders = pipeline.GetPeakActiveOuterFecDecoderCount();
        result.peakReservedDecoderBytes = pipeline.GetPeakReservedOuterFecDecoderBytes();
        result.resourceRejections = (std::max)(telemetry.totalResourcePolicyRejectedCount, currentSnapshot.outerResourceRejections);
        result.deferredCount = telemetry.deferredResourceBusyCount;
        result.quotaCount = telemetry.outerFecQuotaExceededCount;
        result.orphanCount = telemetry.orphanAdmittedBlockCount;
        result.conflictRejections = (std::max)(telemetry.orphanConflictRejectionCount, currentSnapshot.outerConflictRejections);
        result.completedSegments = currentSnapshot.verifiedSegmentCount;
    };
    while (!pipeline.IsCompleted())
    {
        Require(std::chrono::steady_clock::now() < deadline, "Pass-0 probe reached its bounded execution deadline");
        Require(result.senderFrames < maximumFrames, "Pass-0 probe exceeded bounded frame budget");
        Require(builder.GetCarouselSnapshot().cycleCount == 0, "Pass-0 probe cannot repair holes in a later Carousel pass");
        const std::uint64_t ordinal = builder.GetCurrentSegmentOrdinal();
        Require(ordinal < segmentCount, "Pass-0 probe ordinal outside descriptor table");
        if (ordinal >= windowSize && result.firstWindowFrames == 0)
        {
            RecordResources();
            result.firstWindowFrames = result.senderFrames;
            result.firstWindowCompletedSegments = result.completedSegments;
            result.activeDecodersBeforeTransition = receiver.GetTelemetry().activeOuterFecDecoderCount;
            result.firstWindowFullCaptureBytesPerFrame = static_cast<double>(windowSize * pbprotocol::kDefaultSourceSegmentTargetBytes) /
                static_cast<double>(result.firstWindowFrames);
            result.transitionSafe = result.completedSegments == windowSize && result.activeDecodersBeforeTransition == 0 &&
                result.resourceRejections == 0 && result.conflictRejections == 0 && result.deferredCount == 0 &&
                result.quotaCount == 0 && result.orphanCount == 0;
            Require(result.transitionSafe, "First window not digest/stored-complete and released before next-window Transport");
        }
        const std::uint64_t tick = result.senderFrames;
        const SenderUnifiedScheduledFrame& frame = builder.PrepareHeadlessFrame(tick);
        const auto sequenceLease = sessionStore->EnsureFrameSequenceLease(tick + 1);
        Require(static_cast<bool>(sequenceLease), "Pass-0 FrameSequence lease failed: " + sequenceLease.message);
        const std::uint64_t band = (tick / windowSize) % lossBandCount;
        // The journal supplies only the biased phase. Other-phase 1/16 loss is
        // an explicit synthetic sensitivity case, never claimed as captured data.
        const bool erased = !erasedBands.empty() && (tick % windowSize == biasedPhase ?
            std::ranges::find(erasedBands, band) != erasedBands.end() : band % 16 == 15);
        if (!erased)
        {
            result.observedFrames++;
            const std::int64_t timestamp100ns = static_cast<std::int64_t>(tick * 666667ULL);
            // Match live Session -> other Control -> Transport processing order.
            for (std::uint32_t pass = 0; pass < 2; pass++)
            {
                for (const auto& slot : frame.slots)
                {
                    if (slot.assignment.kind != pbmodulation::UnifiedSlotKind::Control)
                    {
                        continue;
                    }
                    const bool sessionControl = slot.assignment.controlPriority == pbmodulation::UnifiedControlPriority::SessionDescriptor;
                    if (sessionControl != (pass == 0))
                    {
                        continue;
                    }
                    const std::span<const std::byte> bytes = sessionControl ? std::span<const std::byte>(description.sessionControl) :
                        slot.assignment.controlPriority == pbmodulation::UnifiedControlPriority::FinalManifest ?
                            std::span<const std::byte>(description.manifestControl) :
                            std::span<const std::byte>(description.segments.at(static_cast<std::size_t>(ordinal)).control);
                    Require(pipeline.ProcessHeadlessControlRecord(bytes, timestamp100ns), "Pass-0 scheduled Control rejected");
                }
            }
            for (std::uint32_t slot = 0; slot < frame.slots.size() && !pipeline.IsCompleted(); slot++)
            {
                if (frame.slots[slot].assignment.kind != pbmodulation::UnifiedSlotKind::Transport)
                {
                    continue;
                }
                const std::uint32_t blockId = builder.GetOuterBlockIdForSlot(slot);
                result.durableLeaseAheadOfUse &= blockId < builder.GetBlockCount() ||
                    blockId < sessionStore->GetRepairIdLeaseEnd(ordinal);
                Require(result.durableLeaseAheadOfUse, "Pass-0 repair equation preceded its durable lease");
                std::vector<std::byte> bytes(informationBytes);
                std::uint32_t payloadBytes = 0;
                const std::size_t serializedBytes = builder.BuildTransportBlockForSlot(slot, bytes, payloadBytes);
                Require(payloadBytes > 0 && serializedBytes <= bytes.size(), "Pass-0 Transport serialization bounds failed");
                bytes.resize(serializedBytes);
                const ReceiverProcessResult admitted = pipeline.ProcessHeadlessTransportBlock(bytes, tick + 1, timestamp100ns);
                result.acceptedSymbols.at(static_cast<std::size_t>(ordinal)) += static_cast<std::uint64_t>(admitted.uniqueAdmission);
                if (admitted.completedSegment)
                {
                    result.completedAtFrame.at(static_cast<std::size_t>(ordinal)) = tick + 1;
                }
            }
        }
        RecordResources();
        Require(result.resourceRejections == 0 && result.conflictRejections == 0 && result.deferredCount == 0 &&
            result.quotaCount == 0 && result.orphanCount == 0, "Pass-0 resource/conflict/orphan invariant failed");
        result.senderFrames++;
        if (!pipeline.IsCompleted())
        {
            const std::uint64_t previousCheckpoint = builder.GetCheckpointSegmentOrdinal();
            builder.Advance();
            if (previousCheckpoint != builder.GetCheckpointSegmentOrdinal() || builder.GetCarouselSnapshot().cycleCount != 0)
            {
                const auto status = sessionStore->UpdateCarouselPosition(builder.GetCarouselSnapshot().cycleCount,
                    builder.GetCheckpointSegmentOrdinal());
                Require(static_cast<bool>(status), "Pass-0 window checkpoint failed: " + status.message);
                result.checkpointUpdates++;
            }
        }
    }
    const DecoderSnapshot finalSnapshot = snapshot.Get();
    result.wholeDigestVerified = finalSnapshot.wholeFileDigestVerified;
    result.published = finalSnapshot.finalPublishSucceeded;
    result.finalReopenVerified = finalSnapshot.finalReopenVerified.value_or(false);
    result.publishedPath = completion.outputPathWide;
    result.sourceStable = IsSourceStable(source);
    Require(result.sourceStable && result.wholeDigestVerified && result.published && result.finalReopenVerified &&
        result.completedSegments == segmentCount, "Pass-0 final publication or source identity failed");
    Require(result.observedFrames > 0, "Pass-0 probe observed no frames");
    result.noRasterEncodedBytesPerObservedFrame = static_cast<double>(result.sourceBytes) / static_cast<double>(result.observedFrames);
}
