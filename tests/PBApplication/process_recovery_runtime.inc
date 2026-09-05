// Included in local_desktop_runtime.cpp only by the opt-in recovery test
// libraries. All scheduling, packet serialization, FEC, resume and publication
// below reuse the production objects in that translation unit.
int test::RunProcessRecoveryWorker(const std::filesystem::path& sourcePath,
    const std::filesystem::path& sessionRoot, const std::filesystem::path& outputDirectory,
    const ProcessRecoveryMode mode) noexcept
{
    try
    {
        const bool largeFile = mode == ProcessRecoveryMode::LargeFile;
        const std::uint64_t fixtureBytes = largeFile ? 20ULL * 1024ULL * 1024ULL * 1024ULL + 65536 : 256ULL * 1024ULL * 1024ULL;
        const std::uint64_t expectedSegments = largeFile ? 2561 : 32;
        GetProcessRecoveryObservations() = {};
        const auto started = std::chrono::steady_clock::now();
        SourceFile source = ReadSourceFile(sourcePath.wstring());
        Require(source.fileBytes == fixtureBytes, "Recovery worker source length does not match its selected fixture");
        Require(std::filesystem::is_directory(outputDirectory), "G18 output directory is absent");
        const ProfileBinding profile = GetProfileBinding(VisualProfile::UnifiedLc4);
        const std::atomic<bool> stopRequested = false;
        DurableSenderPreparation preparation = PrepareDurableSender(source, largeFile, 3,
            profile.visualProfileId, sessionRoot, stopRequested);
        const std::uint64_t preparationMilliseconds = ElapsedMilliseconds(started);
        TransferDescription& description = preparation.description;
        EncoderSessionStore& sessionStore = *preparation.sessionStore;
        Require(description.segments.size() == expectedSegments, "Recovery fixture Segment count mismatch");
        std::uint64_t compressedSegments = 0;
        std::uint64_t rawSegments = 0;
        std::uint64_t encodedTotalBytes = 0;
        const std::uint64_t initialFrameSequence = sessionStore.GetFrameSequenceStart();
        std::ostringstream initial;
        initial << "{\"baseGitCommit\":\"" << PB_PROCESS_TEST_BASE_COMMIT << "\",\"sessionId\":\""
            << SessionIdHex(description.session.sessionId) << "\",\"resumed\":" << (preparation.resumed ? "true" : "false")
            << ",\"sourceBytes\":" << source.fileBytes << ",\"segmentCount\":" << description.segments.size()
            << ",\"profileId\":" << profile.visualProfileId << ",\"wholeFileBlake3\":\""
            << DigestHex(description.manifest.wholeFileDigest.bytes) << "\",\"frameStart\":" << initialFrameSequence
            << ",\"preparationMilliseconds\":" << preparationMilliseconds << ",\"repairStarts\":[";
        for (std::size_t index = 0; index < description.segments.size(); index++)
        {
            const auto& descriptor = description.segments[index].descriptor;
            Require(largeFile || (descriptor.compressionCodec == pbprotocol::CompressionCodec::Raw &&
                descriptor.outerFecMode == pbprotocol::OuterFecMode::WirehairV2 &&
                descriptor.rawSize == pbprotocol::kDefaultSourceSegmentTargetBytes), "G18 did not retain a RAW/Wirehair source");
            Require(description.segments[index].inMemoryEncodedBytes.empty(), "Recovery descriptor table retained encoded payload");
            const auto offset = pbprotocol::CheckedMultiplyUint64(index, pbprotocol::kDefaultSourceSegmentTargetBytes);
            RequireResult(offset, "Recovery Segment offset overflow");
            Require(descriptor.segmentOrdinal == index && descriptor.rawOffset == offset.Value() &&
                descriptor.rawSize == (std::min)(fixtureBytes - offset.Value(),
                    static_cast<std::uint64_t>(pbprotocol::kDefaultSourceSegmentTargetBytes)), "Recovery Segment range mismatch");
            compressedSegments += static_cast<std::uint64_t>(descriptor.compressionCodec == pbprotocol::CompressionCodec::Zstandard);
            rawSegments += static_cast<std::uint64_t>(descriptor.compressionCodec == pbprotocol::CompressionCodec::Raw);
            const auto encodedSum = pbprotocol::CheckedAddUint64(encodedTotalBytes, descriptor.encodedSize);
            RequireResult(encodedSum, "Recovery encoded byte count overflow");
            encodedTotalBytes = encodedSum.Value();
            const auto systematicBlocks = static_cast<std::uint32_t>((descriptor.encodedSize + outerBlockBytes - 1) / outerBlockBytes);
            initial << (index == 0 ? "" : ",") << sessionStore.GetRepairIdStart(index, systematicBlocks);
        }
        initial << "],\"prescanMilliseconds\":" << GetProcessRecoveryObservations().prescanMilliseconds
            << ",\"encodedTotalBytes\":" << encodedTotalBytes << "}\n";
        WriteEvidence(L"initial.json", initial.str());

        const pbprotocol::ReceiverResourcePolicy policy = pbprotocol::GetDefaultReceiverResourcePolicy();
        if (largeFile)
        {
            // Metadata only: exercise the real policy/serializers and checked
            // final offset without allocating a 500 GiB file or payload.
            constexpr std::uint64_t maximumBytes = 500ULL * 1024ULL * 1024ULL * 1024ULL;
            auto boundarySession = MakeSessionDescriptor(description.session.sessionId, maximumBytes, 64000,
                profile.visualProfileId, "metadata-only-500gib.bin");
            Require(policy.maxAcceptedFileBytes == maximumBytes && CalculateSegmentCount(maximumBytes) == 64000,
                "G19 500 GiB policy/count mismatch");
            const auto finalOffset = pbprotocol::CheckedMultiplyUint64(63999, pbprotocol::kDefaultSourceSegmentTargetBytes);
            RequireResult(finalOffset, "G19 500 GiB final offset overflow");
            const auto finalEnd = pbprotocol::CheckedAddUint64(finalOffset.Value(), pbprotocol::kDefaultSourceSegmentTargetBytes);
            RequireResult(finalEnd, "G19 500 GiB final end overflow");
            Require(finalEnd.Value() == maximumBytes && finalOffset.Value() > UINT32_MAX,
                "G19 500 GiB range narrowed");
            const auto sessionSize = pbprotocol::GetSerializedSize(boundarySession);
            RequireResult(sessionSize, "G19 boundary Session size failed");
            std::vector<std::byte> sessionBytes(sessionSize.Value());
            RequireResult(pbprotocol::SerializeSessionDescriptor(boundarySession, policy, sessionBytes), "G19 boundary Session serialization failed");
            const auto sessionRoundTrip = pbprotocol::ParseSessionDescriptor(sessionBytes, policy);
            RequireResult(sessionRoundTrip, "G19 boundary Session parse failed");
            Require(sessionRoundTrip.Value() == boundarySession, "G19 boundary Session round-trip mismatch");
            auto boundarySegment = description.segments.front().descriptor;
            boundarySegment.segmentOrdinal = 63999;
            boundarySegment.rawOffset = finalOffset.Value();
            const auto segmentSize = pbprotocol::GetSerializedSize(boundarySegment);
            RequireResult(segmentSize, "G19 boundary Segment size failed");
            std::vector<std::byte> segmentBytes(segmentSize.Value());
            RequireResult(pbprotocol::SerializeSegmentDescriptor(boundarySegment, boundarySession, policy, segmentBytes), "G19 boundary Segment serialization failed");
            const auto segmentRoundTrip = pbprotocol::ParseSegmentDescriptor(segmentBytes, boundarySession, policy);
            RequireResult(segmentRoundTrip, "G19 boundary Segment parse failed");
            Require(segmentRoundTrip.Value() == boundarySegment, "G19 boundary Segment round-trip mismatch");
            auto boundaryManifest = description.manifest;
            boundaryManifest.originalFileSize = maximumBytes;
            boundaryManifest.segmentCount = 64000;
            std::vector<std::byte> manifestBytes(pbprotocol::GetSerializedSize(boundaryManifest));
            RequireResult(pbprotocol::SerializeFinalManifest(boundaryManifest, boundarySession, policy, manifestBytes), "G19 boundary Manifest serialization failed");
            const auto manifestRoundTrip = pbprotocol::ParseFinalManifest(manifestBytes, boundarySession, policy);
            RequireResult(manifestRoundTrip, "G19 boundary Manifest parse failed");
            Require(manifestRoundTrip.Value() == boundaryManifest, "G19 boundary Manifest round-trip mismatch");
            boundarySession.originalFileSize++;
            boundarySession.segmentCount++;
            Require(!pbprotocol::ValidateSessionDescriptor(boundarySession, policy) &&
                !pbprotocol::CheckedAddUint64(UINT64_MAX, 1) && !pbprotocol::CheckedMultiplyUint64(UINT64_MAX, 2),
                "G19 oversized/overflow boundary was accepted");
            WriteEvidence(L"boundary-500gib.json", "{\"metadataOnly\":true,\"bytes\":" + std::to_string(maximumBytes) +
                ",\"segments\":64000,\"lastOrdinal\":63999,\"lastOffset\":" + std::to_string(finalOffset.Value()) +
                ",\"sessionSegmentManifestRoundTrip\":true,\"overLimitAndOverflowRejected\":true}\n");
        }
        if (mode == ProcessRecoveryMode::QuotaSeed)
        {
            // Persist four valid active caches and five canonical descriptors.
            // The supervisor appends the fifth cache to a derived copy, with
            // valid framing/CRC. Neither writer nor receiver policy is relaxed.
            std::unique_ptr<DecoderResumeStore> store;
            DecoderResumeLoadedState loaded;
            auto status = DecoderResumeStore::Open(outputDirectory, pbprotocol::DeriveSessionTag(description.session.sessionId),
                description.sessionControl, policy, store, loaded);
            Require(static_cast<bool>(status), "G18 quota fixture journal creation failed: " + status.message);
            for (std::size_t index = 0; index < 5; index++)
            {
                status = store->RecordSegmentControl(description.segments[index].control);
                Require(static_cast<bool>(status), "G18 quota fixture descriptor failed: " + status.message);
                if (index == 4)
                {
                    break;
                }
                const auto encoded = LoadEncodedSegment(source, description.segments[index], false, 3);
                DecoderResumeAcceptedBlock block;
                block.segmentOrdinal = index;
                block.outerBlockId = 0;
                block.declaredPayloadBytes = outerBlockBytes;
                block.paddedPayload.assign(encoded.begin(), encoded.begin() + outerBlockBytes);
                status = store->RecordAcceptedBlock(block);
                Require(static_cast<bool>(status), "G18 quota fixture active cache failed: " + status.message);
            }
            status = store->Checkpoint();
            Require(static_cast<bool>(status), "G18 quota fixture flush failed: " + status.message);
            CrashAt(L"quota-seed", 4);
            throw RuntimeFailure("G18 quota fixture must be followed by an abrupt process termination");
        }

        SenderFrameBuilder builder(profile, description, 4,
            [&source, &description, largeFile](const std::uint64_t ordinal)
            {
                return LoadEncodedSegment(source, description.segments.at(static_cast<std::size_t>(ordinal)), largeFile, 3);
            },
            [&sessionStore](const std::uint64_t ordinal, const std::uint32_t systematicBlocks)
            {
                return sessionStore.GetRepairIdStart(ordinal, systematicBlocks);
            },
            [&sessionStore](const std::uint64_t ordinal, const std::uint64_t exclusiveEnd)
            {
                const auto status = sessionStore.EnsureRepairIdLease(ordinal, exclusiveEnd);
                Require(static_cast<bool>(status), "G18 production repair lease failed: " + status.message);
            }, sessionStore.GetCarouselPass(), sessionStore.GetSegmentOrdinal(), 15);

        auto receiverResult = pbreceiver::ReceiverIngress::Create(policy, outerBlockBytes);
        RequireResult(receiverResult, "G18 ReceiverIngress creation failed");
        auto receiver = std::move(receiverResult).Value();
        SnapshotStore<DecoderSnapshot> snapshot;
        snapshot.Update([&](DecoderSnapshot& value)
        {
            value.runGeneration = 1;
            value.state = DecoderState::WaitingForBootstrap;
            value.visualProfile = VisualProfile::UnifiedLc4;
            value.visualProfileId = profile.visualProfileId;
            value.visualLayoutVersion = profile.layoutVersion;
            value.codedDataBytesPerFrame = profile.dataBytes;
            value.codewordsPerFrame = profile.codewords;
        });
        AuthoritativeCompletion completion;
        LargeOutputConfirmationController largeOutputConfirmation;
        std::unique_ptr<ReceiverPipeline> pipeline;
        const auto restoreStarted = std::chrono::steady_clock::now();
        if (mode == ProcessRecoveryMode::Combined || largeFile)
        {
            if (largeFile)
            {
                Require(largeOutputConfirmation.BeginRun(1) == TransitionResult::Applied,
                    "G19 large output confirmation controller failed to start");
            }
            pipeline = std::make_unique<ReceiverPipeline>(receiver, outputDirectory.wstring(), policy, snapshot,
                completion, 1, started, VisualProfile::UnifiedLc4, false, false, true,
                largeFile ? &largeOutputConfirmation : nullptr);
            Require(pipeline->ProcessHeadlessControlRecord(description.sessionControl, 1), "G18 initial Session admission failed");
            if (largeFile)
            {
                const auto request = largeOutputConfirmation.GetSnapshot();
                Require(pipeline->IsAwaitingLargeOutputConfirmation() && request.requestId != 0,
                    "G19 did not exercise the production large output confirmation gate");
                Require(largeOutputConfirmation.Resolve(1, request.requestId, true) == TransitionResult::Applied,
                    "G19 explicit harness confirmation failed");
                pipeline->ApplyLargeOutputConfirmation(2);
                Require(!pipeline->IsAwaitingLargeOutputConfirmation(), "G19 output remained unconfirmed");
                WriteEvidence(L"large-output-confirmation.json", "{\"runGeneration\":1,\"requestId\":" +
                    std::to_string(request.requestId) + ",\"acceptedByExplicitHarness\":true,\"policyUnchanged\":true}\n");
            }
        }
        const std::uint64_t restoreMilliseconds = ElapsedMilliseconds(restoreStarted);
        const std::uint64_t restoredVerifiedSegments = snapshot.Get().verifiedSegmentCount;
        WriteEvidence(L"frames.jsonl", "");
        WriteEvidence(L"progress.jsonl", "");
        const std::filesystem::path evidenceDirectory = ReadEnvironment(L"PB_G18_EVIDENCE_DIRECTORY");
        std::ofstream trace(evidenceDirectory / L"frames.jsonl", std::ios::binary | std::ios::app);
        std::ofstream progress(evidenceDirectory / L"progress.jsonl", std::ios::binary | std::ios::app);
        trace.exceptions(std::ios::badbit | std::ios::failbit);
        progress.exceptions(std::ios::badbit | std::ios::failbit);
        const std::uint64_t firstPass = builder.GetCarouselSnapshot().cycleCount;
        const auto lastAllowedPass = pbprotocol::CheckedAddUint64(firstPass, 2);
        RequireResult(lastAllowedPass, "G18 Carousel pass budget overflow");
        std::uint64_t frameSequence = initialFrameSequence;
        std::uint64_t transportCount = 0;
        std::uint64_t skippedSystematicBlocks = 0;
        std::uint64_t repairBlocks = 0;
        std::int64_t timestamp100ns = largeFile ? 2 : 1;
        std::uint64_t frameCount = 0;
        for (; frameCount < 100000 && (!pipeline || !pipeline->IsCompleted()); frameCount++)
        {
            Require(builder.GetCarouselSnapshot().cycleCount <= lastAllowedPass.Value(),
                "G18 recovery did not finish inside the bounded Carousel budget");
            if (!pipeline && builder.GetCarouselSnapshot().cycleCount != firstPass)
            {
                break;
            }
            const SenderUnifiedScheduledFrame& scheduled = builder.PrepareHeadlessFrame(frameCount);
            const std::uint64_t ordinal = builder.GetCurrentSegmentOrdinal();
            const std::uint32_t systematicBlocks = builder.GetBlockCount();
            const auto nextSequence = pbprotocol::CheckedAddUint64(frameSequence, 1);
            RequireResult(nextSequence, "G18 FrameSequence overflow");
            const auto leaseStatus = sessionStore.EnsureFrameSequenceLease(nextSequence.Value());
            Require(static_cast<bool>(leaseStatus), "G18 durable frame lease failed: " + leaseStatus.message);
            // This is a conservative pre-exposure ledger: even IDs in a frame
            // killed before delivery are considered consumed, never reusable.
            trace << "{\"sequence\":" << frameSequence << ",\"ordinal\":" << ordinal
                << ",\"frameLeaseEnd\":" << sessionStore.GetFrameSequenceLeaseEnd()
                << ",\"repairLeaseEnd\":" << sessionStore.GetRepairIdLeaseEnd(ordinal) << ",\"repairIds\":[";
            bool firstRepair = true;
            for (const auto& slot : scheduled.slots)
            {
                if (slot.transportDisposition == SenderUnifiedTransportSlotDisposition::ScheduledEquation)
                {
                    const auto blockId = builder.GetOuterBlockIdForSlot(slot.assignment.codewordSlot);
                    if (blockId >= systematicBlocks)
                    {
                        trace << (firstRepair ? "" : ",") << blockId;
                        firstRepair = false;
                    }
                }
            }
            trace << "]}\n";
            trace.flush();
            CrashAt(L"lease-persisted", sessionStore.GetFrameSequenceLeaseEnd());

            // The scheduler's canonical Control bytes are processed before
            // Transport, as in the existing Unified receiver admission path.
            for (const auto& slot : scheduled.slots)
            {
                if (pipeline && slot.assignment.kind == pbmodulation::UnifiedSlotKind::Control)
                {
                    const auto& control = slot.assignment.controlPriority == pbmodulation::UnifiedControlPriority::SessionDescriptor ?
                        description.sessionControl : slot.assignment.controlPriority == pbmodulation::UnifiedControlPriority::FinalManifest ?
                        description.manifestControl : description.segments.at(static_cast<std::size_t>(ordinal)).control;
                    timestamp100ns++;
                    Require(pipeline->ProcessHeadlessControlRecord(control, timestamp100ns), "G18 scheduled Control admission failed");
                }
            }
            for (const auto& slot : scheduled.slots)
            {
                if (slot.transportDisposition != SenderUnifiedTransportSlotDisposition::ScheduledEquation)
                {
                    continue;
                }
                const std::uint32_t blockId = builder.GetOuterBlockIdForSlot(slot.assignment.codewordSlot);
                if (blockId == 1 && description.segments.at(static_cast<std::size_t>(ordinal)).descriptor.outerFecMode ==
                    pbprotocol::OuterFecMode::WirehairV2)
                {
                    skippedSystematicBlocks++;
                    continue;
                }
                std::array<std::byte, informationBytes> transport{};
                std::uint32_t payloadBytes = 0;
                const std::size_t serializedBytes = builder.BuildTransportBlockForSlot(slot.assignment.codewordSlot, transport, payloadBytes);
                Require(payloadBytes != 0, "G18 production Encoder emitted an empty payload");
                if (ordinal == 2 && blockId == 128)
                {
                    CrashAt(L"encoder-mid-segment", ordinal);
                }
                if (pipeline)
                {
                    if (ordinal == 2 && blockId == 64 && SelectedCrashPoint() == L"decoder-active")
                    {
                        // Advance the real periodic checkpoint deadline, rather
                        // than calling a synthetic flush or modifying its policy.
                        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
                    }
                    timestamp100ns++;
                    const auto processing = pipeline->ProcessHeadlessTransportBlock(
                        std::span(transport).first(serializedBytes), transportCount + 1, timestamp100ns);
                    Require(processing.hasDataAdmission && processing.carrierAccepted, "G18 Transport admission failed");
                    if (processing.completedSegment)
                    {
                        const auto& completedDescriptor = description.segments.at(static_cast<std::size_t>(ordinal)).descriptor;
                        const auto memory = ReadProcessMemory();
                        progress << "{\"verifiedSegments\":" << snapshot.Get().verifiedSegmentCount
                            << ",\"ordinal\":" << ordinal << ",\"rawOffset\":" << completedDescriptor.rawOffset
                            << ",\"rawSize\":" << completedDescriptor.rawSize
                            << ",\"peakJournalFileBytes\":" << GetProcessRecoveryObservations().peakJournalFileBytes
                            << ",\"workingSetBytes\":" << memory.WorkingSetSize << ",\"peakWorkingSetBytes\":"
                            << memory.PeakWorkingSetSize << "}\n";
                        progress.flush();
                        if (largeFile && ordinal == 1024)
                        {
                            CrashAt(L"large-file-checkpoint", ordinal);
                        }
                    }
                    if (ordinal == 2 && blockId == 128 && SelectedCrashPoint() == L"decoder-active")
                    {
                        Require(receiver.GetTelemetry().activeOuterFecDecoderCount != 0 && !processing.completedSegment,
                            "G18 active FEC injection did not have an incomplete decoder");
                        CrashAt(L"decoder-active", ordinal);
                    }
                }
                transportCount++;
                repairBlocks += static_cast<std::uint64_t>(blockId >= systematicBlocks);
                if (pipeline && pipeline->IsCompleted())
                {
                    break;
                }
            }
            if (pipeline && pipeline->IsCompleted())
            {
                frameCount++;
                break;
            }
            const std::uint64_t previousCheckpointSegmentOrdinal = builder.GetCheckpointSegmentOrdinal();
            const std::uint64_t previousPass = builder.GetCarouselSnapshot().cycleCount;
            builder.Advance();
            if (builder.GetCheckpointSegmentOrdinal() != previousCheckpointSegmentOrdinal ||
                builder.GetCarouselSnapshot().cycleCount != previousPass)
            {
                const auto status = sessionStore.UpdateCarouselPosition(builder.GetCarouselSnapshot().cycleCount,
                    builder.GetCheckpointSegmentOrdinal());
                Require(static_cast<bool>(status), "G18 Carousel position persistence failed: " + status.message);
            }
            frameSequence = nextSequence.Value();
        }
        Require(frameCount < 100000 && IsSourceStable(source), "G18 deadline/frame budget or source identity failed");
        const DecoderSnapshot final = snapshot.Get();
        Require(!pipeline || (pipeline->IsCompleted() && completion.published && final.state == DecoderState::Completed &&
            final.wholeFileDigestVerified && final.finalPublishSucceeded && final.finalReopenVerified.value_or(false) &&
            final.verifiedRawBytes == fixtureBytes && final.verifiedSegmentCount == expectedSegments), "Recovery authoritative publish was not complete");
        Require(builder.GetPeakResidentEncodedSegmentCount() <= 2 &&
            builder.GetPeakResidentEncodedSegmentBytes() <= 2ULL * pbprotocol::kDefaultSourceSegmentTargetBytes,
            "G18 sender resident Segment bound failed");
        const auto memory = ReadProcessMemory();
        std::ostringstream result;
        result << "{\"schema\":\"" << (largeFile ? "PixelBridge.G19LargeFileWorker.1" : "PixelBridge.G18ProcessWorker.1")
            << "\",\"senderOnly\":" << (!pipeline ? "true" : "false")
            << ",\"visualChainCovered\":false,\"published\":" << (completion.published ? "true" : "false")
            << ",\"finalReopenVerified\":" << (final.finalReopenVerified.value_or(false) ? "true" : "false")
            << ",\"verifiedSegments\":" << final.verifiedSegmentCount << ",\"restoredVerifiedSegments\":" << restoredVerifiedSegments
            << ",\"verifiedBytes\":" << final.verifiedRawBytes << ",\"frameCount\":" << frameCount
            << ",\"transportCount\":" << transportCount << ",\"repairBlocks\":" << repairBlocks
            << ",\"skippedSystematicBlocks\":" << skippedSystematicBlocks << ",\"preparationMilliseconds\":" << preparationMilliseconds
            << ",\"prescanMilliseconds\":" << GetProcessRecoveryObservations().prescanMilliseconds
            << ",\"peakJournalFileBytes\":" << GetProcessRecoveryObservations().peakJournalFileBytes
            << ",\"compressedSegments\":" << compressedSegments << ",\"rawSegments\":" << rawSegments
            << ",\"encodedTotalBytes\":" << encodedTotalBytes << ",\"maxOrdinal\":" << expectedSegments - 1
            << ",\"maxRawOffset\":" << description.segments.back().descriptor.rawOffset
            << ",\"restoreMilliseconds\":" << restoreMilliseconds << ",\"totalMilliseconds\":" << ElapsedMilliseconds(started)
            << ",\"workingSetBytes\":" << memory.WorkingSetSize << ",\"peakWorkingSetBytes\":" << memory.PeakWorkingSetSize
            << ",\"senderResidentSegments\":" << builder.GetPeakResidentEncodedSegmentCount()
            << ",\"senderResidentEncodedBytes\":" << builder.GetPeakResidentEncodedSegmentBytes()
            << ",\"receiverActiveDecoders\":" << (pipeline ? pipeline->GetPeakActiveOuterFecDecoderCount() : 0)
            << ",\"receiverReservedDecoderBytes\":" << (pipeline ? pipeline->GetPeakReservedOuterFecDecoderBytes() : 0)
            << ",\"resumeResidentPayloadBytes\":" << (pipeline ? pipeline->GetPeakResumeResidentPayloadBytes() : 0)
            << "}\n";
        WriteEvidence(L"result.json", result.str());
        return 0;
    }
    catch (const std::exception& exception)
    {
        try
        {
            WriteEvidence(L"error.txt", exception.what());
        }
        catch (...)
        {
        }
        return 1;
    }
    catch (...)
    {
        return 1;
    }
}
