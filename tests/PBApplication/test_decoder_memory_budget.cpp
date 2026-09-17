#include "decoder_memory_budget.h"
#include "local_desktop_runtime.h"
#include "run_report.h"
#include "evidence_journal.h"
#include "unified_decoder_test_support.h"

#include <catch2/catch_test_macros.hpp>
#include <QJsonDocument>
#include <QJsonObject>

#include <array>
#include <limits>

namespace
{
constexpr std::uint64_t mebibyte = pbapp::decoderMemoryMebibyte;
using Error = pbapp::DecoderMemoryBudgetError;
}

TEST_CASE("Decoder custom memory budgets validate finite units and bind only local resource fields", "[application][decoder-memory]")
{
    const pbapp::DecoderMemoryBudget defaults;
    REQUIRE(defaults.totalDecoderBytes == 1024 * mebibyte);
    REQUIRE(defaults.perDecoderBytes == 512 * mebibyte);
    REQUIRE(pbapp::CalculateDecoderResumeBudgetBytes(defaults) == 256 * mebibyte);
    REQUIRE(pbapp::CalculateDecoderMemoryPlanningBytes(defaults) == 2560 * mebibyte);
    const auto original = pbapp::MakeBudgetBoundUnifiedReceiverResourcePolicy();
    const pbapp::DecoderMemoryBudget custom{4096 * mebibyte, 1024 * mebibyte};
    auto policy = original;
    REQUIRE(pbapp::ApplyDecoderMemoryBudget(custom, policy) == Error::None);
    REQUIRE(policy.maxTotalOuterFecDecoderBytes == custom.totalDecoderBytes);
    REQUIRE(policy.maxOuterFecDecoderBytes == custom.perDecoderBytes);
    REQUIRE(policy.maxResumeBytes == 1024 * mebibyte);
    REQUIRE(policy.maxActiveOuterFecDecoders == original.maxActiveOuterFecDecoders);
    REQUIRE(policy.maxSegmentCount == original.maxSegmentCount);
    REQUIRE(policy.maxRawSegmentBytes == original.maxRawSegmentBytes);
    REQUIRE(policy.maxEncodedSegmentBytes == original.maxEncodedSegmentBytes);
    REQUIRE(policy.maxAcceptedFileBytes == original.maxAcceptedFileBytes);
    REQUIRE(policy.maxZstdWindowBytes == original.maxZstdWindowBytes);
    for (const auto& invalid : std::array{
        pbapp::DecoderMemoryBudget{0, mebibyte}, pbapp::DecoderMemoryBudget{3 * mebibyte, mebibyte},
        pbapp::DecoderMemoryBudget{4 * mebibyte, 0}, pbapp::DecoderMemoryBudget{4 * mebibyte + 1, mebibyte},
        pbapp::DecoderMemoryBudget{4 * mebibyte, mebibyte + 1}, pbapp::DecoderMemoryBudget{4 * mebibyte, 5 * mebibyte},
        pbapp::DecoderMemoryBudget{pbapp::maximumDecoderMemoryBudgetBytes + mebibyte, mebibyte},
        pbapp::DecoderMemoryBudget{(std::numeric_limits<std::uint64_t>::max)(), mebibyte}})
    {
        REQUIRE(pbapp::ApplyDecoderMemoryBudget(invalid, policy) != Error::None);
        REQUIRE(policy.maxTotalOuterFecDecoderBytes == custom.totalDecoderBytes);
        REQUIRE(policy.maxOuterFecDecoderBytes == custom.perDecoderBytes);
        REQUIRE(policy.maxResumeBytes == 1024 * mebibyte);
        REQUIRE(pbapp::CalculateDecoderResumeBudgetBytes(invalid) == 0);
        REQUIRE(pbapp::CalculateDecoderMemoryPlanningBytes(invalid) == 0);
    }
    const pbapp::DecoderMemoryBudget maximum{pbapp::maximumDecoderMemoryBudgetBytes, pbapp::maximumDecoderMemoryBudgetBytes};
    REQUIRE(pbapp::ApplyDecoderMemoryBudget(maximum, policy) == Error::None);
    policy.maxActiveOuterFecDecoders = 0;
    REQUIRE(pbapp::ApplyDecoderMemoryBudget(defaults, policy) == Error::InvalidResourcePolicy);
    REQUIRE(policy.maxTotalOuterFecDecoderBytes == maximum.totalDecoderBytes);
    REQUIRE(policy.maxActiveOuterFecDecoders == 0);
}

TEST_CASE("Decoder memory recommendation uses available RAM and commit with headroom rather than installed RAM", "[application][decoder-memory]")
{
    pbapp::DecoderMemoryHostSnapshot host{true, 96 * 1024 * mebibyte, 64 * 1024 * mebibyte, 48 * 1024 * mebibyte};
    pbapp::DecoderMemoryBudget budget;
    REQUIRE(pbapp::RecommendDecoderMemoryBudget(host, budget) == Error::None);
    REQUIRE(budget.totalDecoderBytes > 1024 * mebibyte);
    REQUIRE(budget.perDecoderBytes == 512 * mebibyte);
    REQUIRE(pbapp::ValidateDecoderMemoryAgainstHost(budget, host) == Error::None);
    REQUIRE(pbapp::CalculateDecoderMemoryPlanningBytes(budget) <= host.availableCommitBytes - host.availableCommitBytes / 5);
    const auto original = budget;
    host.availableCommitBytes = mebibyte;
    REQUIRE(pbapp::ValidateDecoderMemoryAgainstHost(budget, host) == Error::InsufficientHostMemory);
    REQUIRE(pbapp::RecommendDecoderMemoryBudget(host, budget) == Error::InsufficientHostMemory);
    REQUIRE(budget == original);
    host.availableCommitBytes = 64 * 1024 * mebibyte;
    host.availablePhysicalBytes = 2 * 1024 * mebibyte;
    REQUIRE(pbapp::ValidateDecoderMemoryAgainstHost(original, host) == Error::InsufficientHostMemory);
    REQUIRE(pbapp::RecommendDecoderMemoryBudget(host, budget) == Error::None);
    REQUIRE(budget.totalDecoderBytes < 1024 * mebibyte);
    host.available = false;
    const auto unchanged = budget;
    REQUIRE(pbapp::RecommendDecoderMemoryBudget(host, budget) == Error::HostMemoryUnavailable);
    REQUIRE(budget == unchanged);
    host.available = true;
    host.totalPhysicalBytes = 0;
    REQUIRE(pbapp::ValidateDecoderMemoryAgainstHost(budget, host) == Error::HostMemoryUnavailable);
    host.totalPhysicalBytes = mebibyte;
    REQUIRE(pbapp::RecommendDecoderMemoryBudget(host, budget) == Error::HostMemoryUnavailable);
    host = {true, (std::numeric_limits<std::uint64_t>::max)(), (std::numeric_limits<std::uint64_t>::max)(), (std::numeric_limits<std::uint64_t>::max)()};
    REQUIRE(pbapp::RecommendDecoderMemoryBudget(host, budget) == Error::None);
    REQUIRE(budget.totalDecoderBytes == pbapp::maximumDecoderMemoryBudgetBytes);
    REQUIRE(pbapp::CalculateDecoderMemoryPlanningBytes(budget) > budget.totalDecoderBytes);
    // Exact headroom boundary, then change only one available byte.
    const pbapp::DecoderMemoryBudget smallBudget{4 * mebibyte, mebibyte};
    const auto boundary = pbapp::CalculateDecoderMemoryPlanningBytes(smallBudget) + 512 * mebibyte;
    host = {true, boundary, boundary, boundary};
    REQUIRE(pbapp::ValidateDecoderMemoryAgainstHost(smallBudget, host) == Error::None);
    host.availablePhysicalBytes--;
    REQUIRE(pbapp::ValidateDecoderMemoryAgainstHost(smallBudget, host) == Error::InsufficientHostMemory);
}

TEST_CASE("Decoder memory settings cannot silently enable admission or enter formal measurement", "[application][decoder-memory]")
{
    auto config = pbapp::MakeUnifiedDecoderConfig(std::filesystem::absolute(PB_TEST_SCRATCH_ROOT).wstring(), g16test::Region());
    REQUIRE_FALSE(config.budgetBoundDecoders);
    REQUIRE_FALSE(config.memoryBudget);
    config.visualProfile = pbapp::VisualProfile::UnifiedGrayFast;
    config.memoryBudget = pbapp::DecoderMemoryBudget{};
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.budgetBoundDecoders = true;
    REQUIRE(pbapp::ValidateDecoderConfig(config));
    config.memoryBudget->totalDecoderBytes = 4 * mebibyte;
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.memoryBudget = pbapp::DecoderMemoryBudget{};
    config.visualProfile = pbapp::VisualProfile::UnifiedLc4;
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.visualProfile = pbapp::VisualProfile::UnifiedGrayFast;
    config.measurement = std::make_shared<pbapp::RunMeasurementRecorder>();
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.measurement.reset();
    config.replayOutputPath = L"unused-memory-config.replay";
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
}

TEST_CASE("Decoder memory reports separate policy charges from start-time host planning", "[application][decoder-memory][report]")
{
    pbapp::DecoderSnapshot snapshot;
    snapshot.visualProfile = pbapp::VisualProfile::UnifiedGrayFast;
    snapshot.budgetBoundDecoderAdmission = true;
    snapshot.customDecoderMemoryBudget = true;
    snapshot.outerTotalDecoderByteLimit = 4096 * mebibyte;
    snapshot.outerPerDecoderByteLimit = 1024 * mebibyte;
    snapshot.receiverResumeByteLimit = 1024 * mebibyte;
    snapshot.memoryHostSnapshotAvailable = true;
    snapshot.memoryHostTotalPhysicalBytes = 96 * 1024 * mebibyte;
    snapshot.memoryHostAvailablePhysicalBytes = 64 * 1024 * mebibyte;
    snapshot.memoryHostAvailableCommitBytes = 48 * 1024 * mebibyte;
    snapshot.memoryPlanningBytes = 8704 * mebibyte;
    const pbapp::RunReportContext context{"PixelBridgeDecoder", "test", "test", "test"};
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(pbapp::BuildDecoderRunReportJson(context, snapshot)), &error);
    REQUIRE(error.error == QJsonParseError::NoError);
    const auto memory = document.object().value("decoderMemory").toObject();
    REQUIRE(memory.value("customBudget").toBool());
    REQUIRE(memory.value("totalDecoderBytes").toDouble() == static_cast<double>(snapshot.outerTotalDecoderByteLimit));
    REQUIRE(memory.value("perDecoderBytes").toDouble() == static_cast<double>(snapshot.outerPerDecoderByteLimit));
    REQUIRE(memory.value("resumeBytes").toDouble() == static_cast<double>(snapshot.receiverResumeByteLimit));
    REQUIRE(memory.value("planningBytes").toDouble() == static_cast<double>(snapshot.memoryPlanningBytes));
    REQUIRE(memory.value("hostSnapshotAvailable").toBool());
    REQUIRE(memory.value("hostAvailableCommitBytesAtStart").toDouble() == static_cast<double>(snapshot.memoryHostAvailableCommitBytes));
    REQUIRE(memory.value("boundary").toString().contains("not measured RSS"));
    const auto journal = QJsonDocument::fromJson(QByteArray::fromStdString(pbapp::BuildDecoderJournalRecord(1, snapshot)), &error);
    REQUIRE(error.error == QJsonParseError::NoError);
    REQUIRE(journal.object().value("customDecoderMemoryBudget").toBool());
    REQUIRE(journal.object().value("outerPerDecoderByteLimit").toDouble() == static_cast<double>(snapshot.outerPerDecoderByteLimit));
    REQUIRE(journal.object().value("receiverResumeByteLimit").toDouble() == static_cast<double>(snapshot.receiverResumeByteLimit));
}
