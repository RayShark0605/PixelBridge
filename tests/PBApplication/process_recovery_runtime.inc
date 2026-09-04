// Included in local_desktop_runtime.cpp only by the two opt-in G18 test
// libraries. All scheduling, packet serialization, FEC, resume and publication
// below reuse the production objects in that translation unit.
int test::RunProcessRecoveryWorker(const std::filesystem::path& sourcePath,
    const std::filesystem::path& sessionRoot, const std::filesystem::path& outputDirectory,
    const ProcessRecoveryMode mode) noexcept
{
    try
    {
        constexpr std::uint64_t fixtureBytes = 256ULL * 1024ULL * 1024ULL;
        const auto started = std::chrono::steady_clock::now();
        SourceFile source = ReadSourceFile(sourcePath.wstring());
        Require(source.fileBytes == fixtureBytes, "G18 requires an actual 256 MiB source file");
        Require(std::filesystem::is_directory(outputDirectory), "G18 output directory is absent");
        const ProfileBinding profile = GetProfileBinding(VisualProfile::UnifiedLc4);
        const std::atomic<bool> stopRequested = false;
        DurableSenderPreparation preparation = PrepareDurableSender(source, false, 3,
            profile.visualProfileId, sessionRoot, stopRequested);
        const std::uint64_t preparationMilliseconds = ElapsedMilliseconds(started);
        TransferDescription& description = preparation.description;
        EncoderSessionStore& sessionStore = *preparation.sessionStore;
        Require(description.segments.size() == 32, "G18 fixture did not yield 32 Segments");
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
            Require(descriptor.compressionCodec == pbprotocol::CompressionCodec::Raw &&
                descriptor.outerFecMode == pbprotocol::OuterFecMode::WirehairV2 &&
                descriptor.rawSize == pbprotocol::kDefaultSourceSegmentTargetBytes &&
                description.segments[index].inMemoryEncodedBytes.empty(), "G18 did not retain a streaming RAW/Wirehair source");
            const auto systematicBlocks = static_cast<std::uint32_t>((descriptor.encodedSize + outerBlockBytes - 1) / outerBlockBytes);
            initial << (index == 0 ? "" : ",") << sessionStore.GetRepairIdStart(index, systematicBlocks);
        }
        initial << "]}\n";
        WriteEvidence(L"initial.json", initial.str());

        const pbprotocol::ReceiverResourcePolicy policy = pbprotocol::GetDefaultReceiverResourcePolicy();
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
            [&source, &description](const std::uint64_t ordinal)
            {
                return LoadEncodedSegment(source, description.segments.at(static_cast<std::size_t>(ordinal)), false, 3);
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
        std::unique_ptr<ReceiverPipeline> pipeline;
        const auto restoreStarted = std::chrono::steady_clock::now();
        if (mode == ProcessRecoveryMode::Combined)
        {
            pipeline = std::make_unique<ReceiverPipeline>(receiver, outputDirectory.wstring(), policy, snapshot,
                completion, 1, started, VisualProfile::UnifiedLc4, false, false, true);
            Require(pipeline->ProcessHeadlessControlRecord(description.sessionControl, 1), "G18 initial Session admission failed");
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
        std::int64_t timestamp100ns = 1;
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
                if (blockId == 1)
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
                        const auto memory = ReadProcessMemory();
                        progress << "{\"verifiedSegments\":" << snapshot.Get().verifiedSegmentCount
                            << ",\"workingSetBytes\":" << memory.WorkingSetSize << ",\"peakWorkingSetBytes\":"
                            << memory.PeakWorkingSetSize << "}\n";
                        progress.flush();
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
            const std::uint64_t previousOrdinal = ordinal;
            const std::uint64_t previousPass = builder.GetCarouselSnapshot().cycleCount;
            builder.Advance();
            if (builder.GetCurrentSegmentOrdinal() != previousOrdinal || builder.GetCarouselSnapshot().cycleCount != previousPass)
            {
                const auto status = sessionStore.UpdateCarouselPosition(builder.GetCarouselSnapshot().cycleCount,
                    builder.GetCurrentSegmentOrdinal());
                Require(static_cast<bool>(status), "G18 Carousel position persistence failed: " + status.message);
            }
            frameSequence = nextSequence.Value();
        }
        Require(frameCount < 100000 && IsSourceStable(source), "G18 deadline/frame budget or source identity failed");
        const DecoderSnapshot final = snapshot.Get();
        Require(!pipeline || (pipeline->IsCompleted() && completion.published && final.state == DecoderState::Completed &&
            final.wholeFileDigestVerified && final.finalPublishSucceeded && final.finalReopenVerified.value_or(false) &&
            final.verifiedRawBytes == fixtureBytes && final.verifiedSegmentCount == 32), "G18 authoritative publish was not complete");
        Require(builder.GetPeakResidentEncodedSegmentCount() <= 2 &&
            builder.GetPeakResidentEncodedSegmentBytes() <= 2ULL * pbprotocol::kDefaultSourceSegmentTargetBytes,
            "G18 sender resident Segment bound failed");
        const auto memory = ReadProcessMemory();
        std::ostringstream result;
        result << "{\"schema\":\"PixelBridge.G18ProcessWorker.1\",\"senderOnly\":" << (!pipeline ? "true" : "false")
            << ",\"visualChainCovered\":false,\"published\":" << (completion.published ? "true" : "false")
            << ",\"finalReopenVerified\":" << (final.finalReopenVerified.value_or(false) ? "true" : "false")
            << ",\"verifiedSegments\":" << final.verifiedSegmentCount << ",\"restoredVerifiedSegments\":" << restoredVerifiedSegments
            << ",\"verifiedBytes\":" << final.verifiedRawBytes << ",\"frameCount\":" << frameCount
            << ",\"transportCount\":" << transportCount << ",\"repairBlocks\":" << repairBlocks
            << ",\"skippedSystematicBlocks\":" << skippedSystematicBlocks << ",\"preparationMilliseconds\":" << preparationMilliseconds
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
