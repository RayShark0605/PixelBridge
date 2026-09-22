// Included in the runtime TU to reuse its exact ReceiverPipeline and GPU
// retirement helper. No alternate recovery, publication or FEC implementation.
RecordedPixelReplayResult RunRecordedPixelReplay(RecordedPixelSource& source, const std::filesystem::path& newOutputDirectory, std::ostream& trace,
    std::shared_ptr<pbcore::StageDiagnostics> diagnostics, const std::uint32_t maximumObservations, const bool receiverDecisionDiagnostics)
{
    RecordedPixelReplayResult output;
    const auto started = std::chrono::steady_clock::now();
    SnapshotStore<DecoderSnapshot> snapshot;
    AuthoritativeCompletion completion;
    std::shared_ptr<pbdemodd3d11::CaptureDemodulator> demodulator;
    pbcapturenormalize::ScreenCaptureDomain domain;
    try
    {
        Require(maximumObservations > 0 && maximumObservations <= 7200, "Invalid recording observation budget");
        Require(std::filesystem::create_directory(newOutputDirectory), "Recording output directory must be new");
        const auto policy = MakeUnifiedReceiverResourcePolicy();
        auto receiverResult = pbreceiver::ReceiverIngress::Create(policy, outerBlockBytes);
        RequireResult(receiverResult, "Recording Receiver creation failed");
        auto receiver = std::move(receiverResult).Value();
        const auto profile = GetProfileBinding(VisualProfile::UnifiedLc4);
        snapshot.Update(
            [&](DecoderSnapshot& value)
            {
                value.runGeneration = 1;
                value.runId = GenerateRunId();
                value.remoteMetadata.channelType = ChannelType::Other;
                value.remoteMetadata.notes = "OfflinePixels; original capture metadata unknown; processing QPC is not recording PTS or live channel time";
                value.state = DecoderState::WaitingForBootstrap;
                value.visualProfile = VisualProfile::UnifiedLc4;
                value.visualProfileId = profile.visualProfileId;
                value.visualLayoutVersion = profile.layoutVersion;
                value.codedDataBytesPerFrame = profile.dataBytes;
                value.codewordsPerFrame = profile.codewords;
                value.diagnostics = diagnostics;
            });
        // Missing live capture telemetry intentionally prevents certified
        // goodput. Local processing time remains diagnostic only.
        ReceiverPipeline pipeline(receiver, newOutputDirectory.wstring(), policy, snapshot, completion, 1, started, VisualProfile::UnifiedLc4, false, false, true);
        pipeline.diagnostics = diagnostics.get();
        DecoderConfig config;
        config.visualProfile = VisualProfile::UnifiedLc4;
        config.diagnostics = diagnostics;
        auto demodConfig = MakeCaptureDemodulatorConfig(config, profile, true);
        demodConfig.offlinePixelsOnly = true;
        demodConfig.slotCount = 2;
        demodConfig.maximumRoiWidth = 1920;
        demodConfig.maximumRoiHeight = 1080;
        const auto createStatus = pbdemodd3d11::CaptureDemodulator::Create(demodConfig, demodulator);
        Require(static_cast<bool>(createStatus), "Recording CaptureDemodulator creation failed");
        const auto device = CreateWarpReplayD3dDevice();
        const auto sourceId = pbprotocol::GenerateRandomSessionId();
        RequireResult(sourceId, "Recording source identity generation failed");
        domain.sourceId = sourceId.Value().bytes;
        domain.captureEpoch = 1;
        pbcapturenormalize::CaptureEnvironment environment;
        environment.region.physicalRect = {0, 0, 1920, 1080};
        environment.region.monitorPhysicalRect = environment.region.physicalRect;
        environment.contentSize = {1920, 1080};
        environment.sourceSize = environment.contentSize;
        environment.pixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        environment.adapterLuid = device.adapterLuid;
        environment.bitsPerColor = 8;
        environment.backendKind = pbcapturenormalize::CaptureBackendKind::OfflinePixels;
        const auto domainStatus = demodulator->DomainStarted(domain, environment, device.device.Get());
        Require(static_cast<bool>(domainStatus), "Recording demod domain start failed: " + DescribeCaptureStatus(domainStatus));
        const auto results = std::make_unique<std::array<pbdemodd3d11::CaptureDemodulatorResult, 2>>();
        ProcessResourceSampler resources;
        std::uint64_t traceBytes = 0;
        for (;;)
        {
            if (maximumObservations < 7200 && output.frames == maximumObservations)
            {
                output.prefixLimitReached = true;
                break;
            }
            Require(ElapsedMilliseconds(started) < 900000, "Recording processing budget reached");
            RecordedPixelFrame pixels;
            std::string mediaError;
            const auto read = [&]()
            {
                const pbcore::DiagnosticScope timing(diagnostics.get(), pbcore::DiagnosticStage::MediaDecode);
                return source.ReadNext(pixels, mediaError);
            }();
            if (read == RecordedPixelRead::EndOfFile)
            {
                output.reachedEof = true;
                break;
            }
            Require(read == RecordedPixelRead::Frame, "Recording media failure: " + mediaError);
            Require(output.frames < 7200, "Recording frame budget reached");
            Require(pixels.width == 1920 && pixels.height == 1080 && pixels.rowPitch == 7680 && pixels.bgra.size() == 1920ULL * 1080 * 4,
                "Recording raster contract changed");
            Require(output.timeline.Observe(pixels), "Recording PTS invalid, reordered, changed timebase or exceeded 120 seconds");
            output.frames++;
            pbcapturenormalize::ScreenCaptureFrameMetadata metadata;
            metadata.domain = domain;
            metadata.backend = pbcapturenormalize::CaptureBackendKind::OfflinePixels;
            metadata.captureObservation = output.frames;
            metadata.sourceGeneration = 1;
            metadata.slotGeneration = output.frames;
            metadata.slotIndex = static_cast<std::uint32_t>((output.frames - 1) % 2);
            metadata.physicalRoi = environment.region.physicalRect;
            metadata.sourceContentSize = environment.contentSize;
            metadata.sourceExtent = environment.contentSize;
            metadata.roiSize = environment.contentSize;
            metadata.sourcePixelFormat = metadata.pixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
            metadata.adapterLuid = device.adapterLuid;
            metadata.bitsPerColor = 8;
            metadata.signalEncoding = pbcapturenormalize::CaptureSignalEncoding::SdrRgb;
            metadata.timestamp.domain = pbcapturenormalize::CaptureTimestampDomain::DxgiQpcTicks;
            metadata.timestamp.rawValue = metadata.timestamp.monotonic100ns = metadata.timestamp.arrivalQpc100ns = CurrentQpc100ns();
            auto texture = [&]()
            {
                const pbcore::DiagnosticScope timing(diagnostics.get(), pbcore::DiagnosticStage::ReplayUpload);
                return CreateReplayTexture(
                    device.device.Get(), pbrealcapturereplay::ReplayRasterView{pixels.width, pixels.height, pixels.rowPitch, DXGI_FORMAT_B8G8R8A8_UNORM, pixels.bgra});
            }();
            {
                const pbcore::DiagnosticScope timing(diagnostics.get(), pbcore::DiagnosticStage::ReplayDemod);
                RunReplayProductionDemod(demodulator, device.device.Get(), device.context.Get(), metadata, texture, *results);
            }
            const auto& result = (*results)[0];
            ReceiverDecisionTrace decisions;
            ReceiverProcessResult processing;
            std::exception_ptr processingFailure;
            try
            {
                processing = pipeline.Process(result, receiverDecisionDiagnostics ? &decisions : nullptr);
            }
            catch (...)
            {
                // Retain the decisive failed call, then propagate the original
                // failure. No subsequent frame or recovery retry is permitted.
                processingFailure = std::current_exception();
            }
            resources.Sample(ElapsedMilliseconds(started));
            snapshot.Update(
                [&](DecoderSnapshot& value)
                {
                    ApplyOfflineDemodSnapshot(demodulator->GetSnapshot(), value);
                    ApplyProcessResourceSample(resources.GetSnapshot(), value);
                });
            const auto FiniteMetric = [](const double value)
            {
                if (!std::isfinite(value))
                {
                    return std::string("null");
                }
                std::ostringstream number;
                number << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
                return number.str();
            };
            std::ostringstream row;
            row << "{\"observation\":" << output.frames << ",\"pts\":" << pixels.pts << ",\"timeBaseNumerator\":" << pixels.timeBaseNumerator
                << ",\"timeBaseDenominator\":" << pixels.timeBaseDenominator << ",\"duration\":" << pixels.duration
                << ",\"processingQpc100ns\":" << metadata.timestamp.monotonic100ns << ",\"bootstrapAccepted\":" << (result.bootstrap.IsAccepted() ? "true" : "false")
                << ",\"sessionTag\":" << result.demodulation.unifiedObservation.bootstrapRecord.sessionTag.value
                << ",\"frameSequence\":" << result.demodulation.unifiedObservation.bootstrapRecord.frameSequence << ",\"kind\":" << static_cast<unsigned>(result.kind)
                << ",\"geometry\":" << static_cast<unsigned>(result.geometryStatus)
                << ",\"frameErasure\":" << static_cast<unsigned>(result.demodulation.unifiedObservation.frameErasure)
                << ",\"acceptedBlocks\":" << result.demodulation.acceptedUnifiedBlockCount
                << ",\"receiverCarrierAccepted\":" << (processing.carrierAccepted ? "true" : "false")
                << ",\"receiverUniqueAdmission\":" << (processing.uniqueAdmission ? "true" : "false")
                << ",\"receiverDataDisposition\":" << static_cast<unsigned>(processing.dataDisposition) << ",\"slots\":[";
            bool first = true;
            for (const auto& slot : result.demodulation.unifiedObservation.slotObservations)
            {
                if (!first)
                {
                    row << ',';
                }
                first = false;
                row << '[' << static_cast<unsigned>(slot.lane) << ',' << static_cast<unsigned>(slot.rejection) << ',' << slot.iterationsUsed << ',' << slot.fecValid
                    << ',' << slot.crcValid << ',' << slot.accepted << ']';
            }
            row << "],\"bootstrapErasure\":" << static_cast<unsigned>(result.bootstrap.erasure) << ",\"bootstrapQuality\":" << FiniteMetric(result.bootstrap.quality)
                << ",\"timingResidual\":" << FiniteMetric(result.bootstrap.timingResidual)
                << ",\"timingBitErrorFraction\":" << FiniteMetric(result.bootstrap.timingBitErrorFraction)
                << ",\"midGrayFraction\":" << FiniteMetric(result.bootstrap.midGrayFraction)
                << ",\"sampleMidGrayFraction\":" << FiniteMetric(result.bootstrap.sampleMidGrayFraction) << ",\"geometryFit\":["
                << FiniteMetric(result.bootstrap.geometry.originX) << ',' << FiniteMetric(result.bootstrap.geometry.originY) << ','
                << FiniteMetric(result.bootstrap.geometry.scaleX) << ',' << FiniteMetric(result.bootstrap.geometry.scaleY) << "],\"bootstrapCopies\":[";
            first = true;
            for (const auto& copy : result.bootstrap.copies)
            {
                if (!first)
                {
                    row << ',';
                }
                first = false;
                row << '[' << copy.fecDecoded << ',' << copy.crcValid << ',' << copy.recordValid << ',' << FiniteMetric(copy.residual) << ','
                    << FiniteMetric(copy.sampleMidGrayFraction) << ']';
            }
            row << "],\"acceptedPayloadDigests\":[";
            first = true;
            for (std::uint32_t index = 0; index < result.demodulation.acceptedUnifiedBlockCount; index++)
            {
                const auto& block = result.demodulation.acceptedUnifiedBlocks[index];
                Require(block.size <= block.bytes.size(), "Recording accepted block size out of bounds");
                if (!first)
                {
                    row << ',';
                }
                first = false;
                row << '[' << static_cast<unsigned>(block.codewordSlot) << ',' << static_cast<unsigned>(block.kind) << ',' << block.size << ",\""
                    << DigestHex(pbprotocol::ComputeBlake3Digest(std::span(block.bytes).first(block.size))) << "\"]";
            }
            row << "],\"freshnessCurrent\":[";
            first = true;
            for (const auto& freshness : result.demodulation.unifiedObservation.freshness)
            {
                if (!first)
                {
                    row << ',';
                }
                first = false;
                row << freshness.current;
            }
            row << "],\"receiverDataDispositionAuthority\":\"LegacyDefaultDoNotUseForUnified\",\"receiverDecisions\":";
            if (receiverDecisionDiagnostics)
            {
                WriteReceiverDecisionTrace(row, decisions);
            }
            else
            {
                row << "null";
            }
            row << "}\n";
            const auto line = row.str();
            Require(CanAppendRecordedTrace(traceBytes, line.size()), "Recording trace budget reached");
            trace << line;
            Require(trace.good(), "Recording trace write failed");
            traceBytes += line.size();
            if (processingFailure)
            {
                std::rethrow_exception(processingFailure);
            }
        }
        pipeline.EndCaptureTelemetry(ElapsedMilliseconds(started));
    }
    catch (const std::exception& error)
    {
        output.error = error.what();
    }
    if (demodulator)
    {
        demodulator->DomainInvalidated(domain);
    }
    trace.flush();
    if (!trace.good() && output.error.empty())
    {
        output.error = "Recording trace flush failed";
    }
    output.decoder = snapshot.Get();
    // Preserve the production counters but do not export processing-rate FPS
    // under a live visual-channel metric name. This copy is offline-tool only.
    output.decoder.unifiedTelemetry.uniqueVisualFps.reset();
    output.decoder.endToEndUniqueVisualFps.reset();
    output.publishedAndReopened = completion.published;
    output.processingMilliseconds = ElapsedMilliseconds(started);
    return output;
}
