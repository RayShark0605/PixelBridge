// No raster/capture claims: this seam replaces the demodulation edge with
// canonical blocks from the production builder-> FEC, Receiver, durability,
// identity/policy, publication and post-completion admission are production.
namespace
{
void RunFeedbackTransferProbe(const std::wstring& outputDirectory, const FeedbackTransferProbeOptions& options,
    FeedbackTransferProbeSnapshot& output)
{
    Require(options.segmentCount <= 13 && (options.visualProfile == VisualProfile::UnifiedLc4 ||
        options.visualProfile == VisualProfile::UnifiedGrayFast || IsExperimentalPam4Family(options.visualProfile)), "Feedback probe dimensions/profile invalid");
    Require(!options.spatialInterleave || options.visualProfile == VisualProfile::UnifiedGrayFast, "Feedback spatial probe requires GrayFast");
    const auto profile = GetProfileBinding(options.visualProfile);
    auto fixture = BuildUnifiedStripingFixture(64U * 1024U, profile.profile, (std::max)(options.segmentCount, 1U));
    auto& description = fixture.description;
    const auto sessionTag = pbprotocol::DeriveSessionTag(description.session.sessionId);
    if (options.segmentCount == 0)
    {
        description.segments.clear();
        description.session.originalFileSize = 0;
        description.session.segmentCount = 0;
        description.manifest.originalFileSize = 0;
        description.manifest.segmentCount = 0;
        description.manifest.wholeFileDigest = pbprotocol::GetEmptyBlake3WholeFileDigest();
    }
    else if (options.mixedCompression)
    {
        for (std::size_t ordinal = 0; ordinal < description.segments.size(); ordinal += 2)
        {
            const auto offset = ordinal * 64U * 1024U;
            description.segments[ordinal] = PrepareSegmentDescription(description.session, sessionTag, ordinal, offset,
                std::span(fixture.source).subspan(offset, 64U * 1024U), true, 3, profile.blockBytes);
        }
    }
    FinalizeTransferControls(description);
    const auto policy = MakeReceiverResourcePolicyForVisualProfile(profile.profile);
    FeedbackIdentity identity;
    Require(static_cast<bool>(BuildFeedbackIdentity(description.session, description.manifest, profile.layoutVersion, policy, identity)), "Feedback probe identity failed");
    FeedbackSenderPolicy feedback;
    Require(static_cast<bool>(feedback.Reset(identity, 1)), "Feedback probe policy reset failed");
    FeedbackSenderPolicy unused;
    Require(static_cast<bool>(unused.Reset(identity, 2)), "Feedback disabled-equivalence policy reset failed");
    EncoderSessionStoreCreateConfig stateConfig;
    std::unique_ptr<EncoderSessionStore> sessionStore;
    SenderFrameBuilder::RepairIdStartProvider repairStart;
    SenderFrameBuilder::RepairLeaseCallback repairLease;
    if (options.durableRepairRestart)
    {
        Require(options.segmentCount >= 6 && !options.mixedCompression, "Durable restart probe requires multiple Wirehair segments");
        stateConfig.rootDirectory = std::filesystem::path(outputDirectory) / L"sender-feedback-state";
        stateConfig.sourceIdentity = {1, description.session.sessionId.bytes, description.session.originalFileSize, 1};
        stateConfig.sourcePathUtf8 = "feedback-probe.bin";
        stateConfig.buildIdentity = "FeedbackDurableProbe.1";
        stateConfig.compressionIdentity = "raw";
        stateConfig.outerFecIdentity = "wirehair-v2";
        stateConfig.sessionId = description.session.sessionId;
        stateConfig.segmentCount = description.session.segmentCount;
        stateConfig.descriptorBundle = BuildDescriptorBundle(description);
        const auto created = EncoderSessionStore::Create(stateConfig, sessionStore);
        Require(static_cast<bool>(created), "Durable feedback probe store creation failed: " + created.message);
        repairStart = [&](const std::uint64_t ordinal, const std::uint32_t systematicCount)
        {
            return sessionStore->GetRepairIdStart(ordinal, systematicCount);
        };
        repairLease = [&](const std::uint64_t ordinal, const std::uint64_t requiredExclusive)
        {
            const auto leased = sessionStore->EnsureRepairIdLease(ordinal, requiredExclusive);
            Require(static_cast<bool>(leased), "Durable feedback repair lease failed: " + leased.message);
        };
    }
    const auto MakeBuilder = [&]()
    {
        return std::make_unique<SenderFrameBuilder>(profile, description, 4, SenderFrameBuilder::SegmentLoader{}, repairStart, repairLease,
            sessionStore ? sessionStore->GetCarouselPass() : 0, sessionStore ? sessionStore->GetSegmentOrdinal() : 0, 30, options.spatialInterleave);
    };
    auto builder = MakeBuilder();
    {
        SenderFrameBuilder baseline(profile, description, 4, {}, {}, {}, 0, 0, 30, options.spatialInterleave);
        SenderFrameBuilder disabled(profile, description, 4, {}, {}, {}, 0, 0, 30, options.spatialInterleave);
        for (std::uint64_t tick = 0; tick < 24; tick++)
        {
            const auto expected = baseline.PrepareHeadlessFrame(tick);
            const auto actual = disabled.PrepareHeadlessFrame(tick);
            output.disabledTraceEquivalent &= expected == actual;
            for (std::uint32_t slot = 0; slot < expected.slotCount; slot++)
            {
                if (expected.slotAssignments[slot].assignment.kind == pbmodulation::UnifiedSlotKind::Control)
                {
                    output.disabledTraceEquivalent &= std::ranges::equal(baseline.GetControlBlockForSlot(slot), disabled.GetControlBlockForSlot(slot));
                }
                else if (!description.segments.empty() && expected.slotAssignments[slot].transportDisposition != SenderUnifiedTransportSlotDisposition::InactiveControlPrelude)
                {
                    std::array<std::byte, pbmodulation::kUnifiedMaximumInformationBytes> expectedBytes{};
                    std::array<std::byte, pbmodulation::kUnifiedMaximumInformationBytes> actualBytes{};
                    std::uint32_t expectedPayload = 0;
                    std::uint32_t actualPayload = 0;
                    const auto expectedSize = baseline.BuildTransportBlockForSlot(slot, expectedBytes, expectedPayload);
                    const auto actualSize = disabled.BuildTransportBlockForSlot(slot, actualBytes, actualPayload);
                    output.disabledTraceEquivalent &= expectedSize == actualSize && expectedPayload == actualPayload &&
                        std::equal(expectedBytes.begin(), expectedBytes.begin() + expectedSize, actualBytes.begin());
                }
            }
            baseline.Advance();
            disabled.Advance();
            disabled.ApplyFeedbackPolicy(unused);
        }
    }
    auto ingress = pbreceiver::ReceiverIngress::Create(policy, profile.blockBytes);
    RequireResult(ingress, "Feedback probe ingress failed");
    auto receiver = std::move(ingress).Value();
    SnapshotStore<DecoderSnapshot> snapshot;
    snapshot.Update([&](DecoderSnapshot& value)
    {
        value.runGeneration = 1;
        value.visualProfile = profile.profile;
        value.visualProfileId = profile.visualProfileId;
        value.visualLayoutVersion = profile.layoutVersion;
    });
    AuthoritativeCompletion completion;
    ReceiverPipeline pipeline(receiver, outputDirectory, policy, snapshot, completion, 1, std::chrono::steady_clock::now(), profile.profile,
        false, false, true, nullptr, {}, false, {}, true);
    std::vector<std::vector<std::uint32_t>> repairIds(options.segmentCount);
    std::vector<bool> covered(options.segmentCount, false);
    std::uint64_t feedbackSequence = 0;
    std::uint64_t noTerminalFrames = 0;
    bool expiryStarted = false;
    bool previouslyEffective = false;
    std::uint64_t runToken = 1;
    std::uint64_t nextFrameSequence = 0;
    bool restarted = false;
    std::uint64_t repairsAfterRestart = 0;
    std::vector<std::uint32_t> retiredLeaseEnds(options.segmentCount, 0);
    for (std::uint64_t tick = 0; tick < 3000; tick++)
    {
        const auto frameSequence = nextFrameSequence++;
        if (sessionStore)
        {
            Require(static_cast<bool>(sessionStore->EnsureFrameSequenceLease(frameSequence + 1)), "Feedback probe frame lease failed");
        }
        const auto frame = builder->PrepareHeadlessFrame(tick);
        output.preparedRetriesFrozen &= frame == builder->PrepareHeadlessFrame(tick);
        const auto resultOwner = std::make_unique<pbdemodd3d11::CaptureDemodulatorResult>();
        auto& result = *resultOwner;
        const pbprotocol::BootstrapRecord bootstrap{pbprotocol::kBootstrapVersion, pbprotocol::GetProtocolVersion(), profile.layoutVersion,
            profile.visualProfileId, sessionTag, frameSequence, 0, 0};
        RequireResult(pbprotocol::SerializeBootstrapRecord(bootstrap, result.bootstrapRecord), "Feedback projection Bootstrap failed");
        result.bootstrap.erasure = pbmodulation::LocalDesktopErasureReason::None;
        result.bootstrap.canonical44 = result.bootstrapRecord;
        result.bootstrap.geometry.scaleX = 1;
        result.bootstrap.geometry.scaleY = 1;
        result.metadata.domain.captureEpoch = 1;
        result.metadata.domain.sourceId.fill(std::byte{1});
        result.metadata.captureObservation = tick + 1;
        result.metadata.timestamp.monotonic100ns = static_cast<std::int64_t>((tick + 1) * 10'000'000ULL / 30U);
        result.demodulation.metadata = result.metadata;
        result.demodulation.visualProfileId = profile.visualProfileId;
        std::array<pbmodulation::UnifiedAcceptedBlock, maximumMixedFrameSlotCount> blocks{};
        std::uint32_t blockCount = 0;
        std::uint32_t controlCount = 0;
        std::uint32_t transportCount = 0;
        for (std::uint32_t slot = 0; slot < frame.slotCount; slot++)
        {
            pbmodulation::UnifiedAcceptedBlock block;
            block.codewordSlot = static_cast<std::uint8_t>(slot);
            block.kind = frame.slotAssignments[slot].assignment.kind;
            if (block.kind == pbmodulation::UnifiedSlotKind::Control)
            {
                const auto bytes = builder->GetControlBlockForSlot(slot);
                block.size = static_cast<std::uint32_t>(bytes.size());
                std::copy(bytes.begin(), bytes.end(), block.bytes.begin());
                controlCount++;
            }
            else
            {
                if (description.segments.empty() || frame.slotAssignments[slot].transportDisposition == SenderUnifiedTransportSlotDisposition::InactiveControlPrelude)
                {
                    continue;
                }
                std::uint32_t payloadBytes = 0;
                block.size = static_cast<std::uint32_t>(builder->BuildTransportBlockForSlot(slot, block.bytes, payloadBytes));
                const auto parsed = pbprotocol::ParseTransportBlock(std::span(block.bytes).first(block.size));
                RequireResult(parsed, "Feedback probe Transport parse failed");
                const auto ordinal = static_cast<std::size_t>(parsed.Value().header.segmentOrdinal);
                if (expiryStarted)
                {
                    covered[ordinal] = true;
                }
                if (tick >= 60 && tick < 100 && !options.expireFeedback && feedback.IsSegmentConfirmed(ordinal))
                {
                    output.acknowledgedServiceAfterSettling++;
                }
                if (frame.slotAssignments[slot].repairEquation &&
                    frame.slotAssignments[slot].transportDisposition == SenderUnifiedTransportSlotDisposition::ScheduledEquation)
                {
                    auto& ids = repairIds[ordinal];
                    const auto outerId = parsed.Value().header.outerBlockId;
                    output.repairIdentitiesUnique &= std::find(ids.begin(), ids.end(), outerId) == ids.end();
                    ids.push_back(outerId);
                    if (restarted)
                    {
                        output.repairIdentitiesUnique &= outerId >= retiredLeaseEnds[ordinal];
                        repairsAfterRestart++;
                    }
                }
                // The first Segment is withheld until frame 120. Other durable
                // segments drive actual bitmap feedback; no fake ACK is used.
                const auto withheldOrdinal = options.withholdTail ? options.segmentCount - 1U : 0U;
                if (options.segmentCount >= 6 && ordinal == withheldOrdinal && tick < 120)
                {
                    continue;
                }
                transportCount++;
            }
            blocks[blockCount++] = block;
        }
        const auto SetObservation = [&](auto& observation)
        {
            observation.inputValid = true;
            observation.frameErasure = decltype(observation.frameErasure)::None;
            observation.bootstrap = result.bootstrap;
            observation.bootstrapRecord = bootstrap;
            observation.frameSlotCount = profile.codewords;
            observation.acceptedBlocks = blockCount;
            observation.acceptedControlRecords = controlCount;
            observation.acceptedTransportBlocks = transportCount;
        };
        if (profile.profile == VisualProfile::ExperimentalPam4Wide)
        {
            result.kind = pbdemodd3d11::CaptureDemodulatorResultKind::ExperimentalPam4WideFrame;
            SetObservation(result.pam4WideObservation);
            result.pam4WideBlockCount = blockCount;
            std::copy_n(blocks.begin(), blockCount, result.pam4WideBlocks.begin());
        }
        else if (profile.profile == VisualProfile::ExperimentalPam4)
        {
            result.kind = pbdemodd3d11::CaptureDemodulatorResultKind::ExperimentalPam4Frame;
            SetObservation(result.pam4Observation);
            result.pam4BlockCount = blockCount;
            std::copy_n(blocks.begin(), blockCount, result.pam4Blocks.begin());
        }
        else
        {
            result.kind = pbdemodd3d11::CaptureDemodulatorResultKind::UnifiedFrame;
            SetObservation(result.demodulation.unifiedObservation);
            result.demodulation.acceptedUnifiedBlockCount = blockCount;
            std::copy_n(blocks.begin(), blockCount, result.demodulation.acceptedUnifiedBlocks.begin());
        }
        Require(static_cast<bool>(feedback.RecordSubmittedFrame(frameSequence)), "Feedback probe actual Submit ledger failed");
        static_cast<void>(pipeline.Process(result));
        const auto now = 1'000'000'000ULL + tick * 1'000'000'000ULL / 30U + (expiryStarted ? feedbackFreshnessNanoseconds + 1 : 0);
        if (options.expireFeedback && previouslyEffective && tick >= 35 && !expiryStarted)
        {
            output.feedbackExpired = feedback.Expire(now + feedbackFreshnessNanoseconds + 1);
            expiryStarted = true;
        }
        if (!expiryStarted && tick % 7 == 0)
        {
            FeedbackSnapshot receipt;
            receipt.receiverId.fill(std::byte{1});
            receipt.receiverEpoch.fill(std::byte{2});
            receipt.snapshotSequence = ++feedbackSequence;
            Require(static_cast<bool>(pipeline.ExportFeedbackSnapshot(receipt)), "Feedback probe snapshot export failed");
            if (receipt.phase == FeedbackPhase::Completed)
            {
                receipt.phase = FeedbackPhase::Receiving;
                receipt.wholeDigestVerified = false;
                receipt.finalPublishSucceeded = false;
                receipt.finalReopenVerified = false;
                receipt.reportedCompletionMilliseconds = 0;
            }
            static_cast<void>(feedback.Accept(receipt, now));
        }
        previouslyEffective |= feedback.GetTelemetry().effective && feedback.GetTelemetry().confirmedSegments != 0;
        builder->Advance();
        builder->ApplyFeedbackPolicy(feedback);
        output.peakEncodedSegments = builder->GetPeakResidentEncodedSegmentCount();
        output.acknowledgedRetirements = builder->GetFeedbackSkippedService();
        output.senderFrames = tick + 1;
        if (options.durableRepairRestart && !restarted && tick >= 40 && output.acknowledgedRetirements != 0 &&
            std::ranges::any_of(repairIds, [](const auto& ids) { return !ids.empty(); }))
        {
            Require(static_cast<bool>(sessionStore->UpdateCarouselPosition(builder->GetCarouselSnapshot().cycleCount, builder->GetCheckpointSegmentOrdinal())), "Feedback probe position checkpoint failed");
            for (std::size_t ordinal = 0; ordinal < retiredLeaseEnds.size(); ordinal++)
            {
                retiredLeaseEnds[ordinal] = sessionStore->GetRepairIdLeaseEnd(ordinal);
            }
            const auto oldFrameLeaseEnd = sessionStore->GetFrameSequenceLeaseEnd();
            builder.reset();
            sessionStore.reset();
            bool found = false;
            const auto loaded = EncoderSessionStore::FindMatching(stateConfig.rootDirectory, stateConfig.sourceIdentity,
                stateConfig.buildIdentity, stateConfig.compressionIdentity, stateConfig.outerFecIdentity, sessionStore, found);
            Require(static_cast<bool>(loaded) && found && sessionStore->WasResumed(), "Feedback probe durable restart did not load its original store");
            Require(sessionStore->GetSessionId() == stateConfig.sessionId && sessionStore->MatchesDescriptorBundle(stateConfig.descriptorBundle),
                "Feedback probe restart changed immutable Session/descriptor bytes");
            nextFrameSequence = sessionStore->GetFrameSequenceStart();
            Require(nextFrameSequence >= oldFrameLeaseEnd, "Feedback probe restart reused a leased frame identity");
            runToken++;
            Require(static_cast<bool>(feedback.Reset(identity, runToken)), "Feedback probe new Start reset failed");
            builder = MakeBuilder();
            restarted = true;
        }
        if (pipeline.IsCompleted() && (!options.expireFeedback || (expiryStarted && std::ranges::all_of(covered, [](const bool value) { return value; }))))
        {
            if (noTerminalFrames++ < 20)
            {
                Require(!feedback.CanStop(runToken), "All-segment hints incorrectly stopped without final receipt");
                continue;
            }
            output.allConfirmedWithoutTerminalContinued = true;
            FeedbackSnapshot terminal;
            terminal.receiverId.fill(std::byte{1});
            terminal.receiverEpoch.fill(std::byte{2});
            terminal.snapshotSequence = ++feedbackSequence;
            Require(static_cast<bool>(pipeline.ExportFeedbackSnapshot(terminal)), "Feedback probe terminal export failed");
            Require(static_cast<bool>(feedback.Accept(terminal, now)), "Feedback probe final receipt rejected");
            output.terminalAccepted = feedback.CanStop(runToken);
            output.staleTokenCanStop = feedback.CanStop(runToken + 1);
            output.durableRestartVerified = restarted && repairsAfterRestart != 0;
            output.expiredCoveredAllSegments = std::ranges::all_of(covered, [](const bool value) { return value; });
            output.decoder = snapshot.Get();
            return;
        }
    }
    throw RuntimeFailure("Feedback transfer probe exceeded bounded frame budget");
}
}

RuntimeStatus ApplicationRuntimeTestAccess::ProbeFeedbackTransfer(const std::wstring& outputDirectory,
    const FeedbackTransferProbeOptions& options, FeedbackTransferProbeSnapshot& output) noexcept
{
    output = {};
    try
    {
        RunFeedbackTransferProbe(outputDirectory, options, output);
        return {};
    }
    catch (const std::exception& exception)
    {
        return RuntimeStatus::Failure(exception.what());
    }
    catch (...)
    {
        return RuntimeStatus::Failure("Feedback transfer probe failed");
    }
}

RuntimeStatus ApplicationRuntimeTestAccess::ProbeFeedbackReaderTimeout(const std::filesystem::path& trustedHelperPath,
    const std::filesystem::path& feedbackFile, FeedbackTelemetry& output) noexcept
{
    output = {};
    try
    {
        auto fixture = BuildUnifiedStripingFixture(64U * 1024U, VisualProfile::UnifiedLc4, 1);
        const auto profile = GetProfileBinding(VisualProfile::UnifiedLc4);
        SenderFrameBuilder builder(profile, fixture.description, 4, {}, {}, {}, 0, 0, 30);
        SnapshotStore<EncoderSnapshot> snapshot;
        snapshot.Update([](EncoderSnapshot& value) { value.runGeneration = 1; });
        FeedbackIoProcess process(trustedHelperPath);
        EncoderFeedbackReader reader(process, {feedbackFile, FeedbackLocationKind::File}, fixture.description, profile, snapshot, 1);
        const auto started = MeasurementNowNanoseconds();
        reader.Pump(started, builder);
        const auto future = started + 10'000'000'000ULL;
        reader.Pump(future, builder);
        Require(snapshot.Get().feedback.ioTimeouts == 1, "Timeout probe did not observe the watchdog event");
        for (std::uint32_t iteration = 0; iteration < 16; iteration++)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            reader.Pump(future + iteration, builder);
        }
        output = snapshot.Get().feedback;
        return {};
    }
    catch (const std::exception& exception)
    {
        return RuntimeStatus::Failure(exception.what());
    }
    catch (...)
    {
        return RuntimeStatus::Failure("Feedback timeout probe failed");
    }
}
