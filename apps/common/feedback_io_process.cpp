#include "feedback_io_process.h"
#include "feedback_io_channel.h"
#include "feedback_file_store.h"
#include "feedback_win32.h"
#include "pbprotocol/session_random.h"
#include <algorithm>
#include <string>
#include <utility>

namespace pbapp
{
namespace
{
constexpr std::uint64_t operationTimeoutNanoseconds = 5'000'000'000ULL;

std::wstring QuoteArgument(const std::wstring& argument)
{
    std::wstring result(1, L'"');
    std::size_t slashes = 0;
    for (const wchar_t character : argument)
    {
        if (character == L'\\')
        {
            slashes++;
            continue;
        }
        result.append(character == L'"' ? slashes * 2U + 1U : slashes, L'\\');
        slashes = 0;
        result.push_back(character);
    }
    result.append(slashes * 2U, L'\\');
    result.push_back(L'"');
    return result;
}

std::wstring Hex(const std::array<std::byte, 16>& bytes)
{
    std::wstring result;
    for (const auto value : bytes)
    {
        constexpr wchar_t digits[] = L"0123456789abcdef";
        result.push_back(digits[std::to_integer<unsigned int>(value) >> 4U]);
        result.push_back(digits[std::to_integer<unsigned int>(value) & 15U]);
    }
    return result;
}

FeedbackStatus NativeFailure(const char* const message)
{
    return {FeedbackError::Unavailable, message, GetLastError()};
}
}

struct FeedbackIoProcess::Implementation
{
    std::filesystem::path helperPath;
    detail::FeedbackHandle process;
    detail::FeedbackHandle job;
    detail::FeedbackHandle pipe;
    FeedbackIoRole role = FeedbackIoRole::Reader;
    FeedbackStatus status;
    std::optional<FeedbackSnapshot> pending;
    std::vector<std::byte> transmit;
    std::size_t transmitPosition = 0;
    std::vector<std::byte> receive;
    std::array<std::byte, 16> nonce{};
    std::array<std::byte, 16> receiverId{};
    std::uint64_t runToken = 0;
    std::uint64_t publishedSequence = 0;
    std::uint64_t writingSequence = 0;
    std::uint64_t lastProgress = 0;
    bool clockStarted = false;
    bool accepting = false;
    bool ready = false;
    bool timedOut = false;

    bool IsRunning() const noexcept
    {
        return process && WaitForSingleObject(process.Get(), 0) != WAIT_OBJECT_0;
    }

    void Fail(const FeedbackStatus failure) noexcept
    {
        status = failure;
        accepting = false;
        ready = false;
        pending.reset();
        pipe.Reset();
        if (job)
        {
            TerminateJobObject(job.Get(), 1);
        }
        if (process)
        {
            TerminateProcess(process.Get(), 1);
        }
    }
};

FeedbackIoProcess::FeedbackIoProcess(std::filesystem::path trustedHelperPath) : implementation_(std::make_unique<Implementation>())
{
    implementation_->helperPath = std::move(trustedHelperPath);
}

FeedbackIoProcess::~FeedbackIoProcess()
{
    Stop();
}

bool FeedbackIoProcess::IsRunning() const noexcept
{
    return implementation_->IsRunning();
}

std::size_t FeedbackIoProcess::GetPendingSnapshotCount() const noexcept
{
    return implementation_->pending ? 1U : 0U;
}

void FeedbackIoProcess::Stop() noexcept
{
    auto& state = *implementation_;
    state.accepting = false;
    state.ready = false;
    state.pending.reset();
    state.pipe.Reset();
    if (state.job)
    {
        TerminateJobObject(state.job.Get(), 0);
    }
    if (state.process)
    {
        TerminateProcess(state.process.Get(), 0);
        // Driver-delayed termination is not an invitation to join forever or
        // forget the live child. Start refuses to replace an unconfirmed child.
        if (WaitForSingleObject(state.process.Get(), 250) != WAIT_OBJECT_0)
        {
            state.status = {FeedbackError::Unavailable, "Old feedback helper has not exited"};
            return;
        }
    }
    state.process.Reset();
    state.job.Reset();
    state.transmit.clear();
    state.receive.clear();
}

FeedbackStatus FeedbackIoProcess::Start(const FeedbackIoRole role, const FeedbackConfig& config, const std::uint64_t runToken)
{
    Stop();
    auto& state = *implementation_;
    if (state.IsRunning())
    {
        return state.status;
    }
    if (runToken == 0 || (role != FeedbackIoRole::Reader && role != FeedbackIoRole::Writer))
    {
        return {FeedbackError::InvalidFormat, "Invalid feedback helper generation or role"};
    }
    std::filesystem::path path;
    const auto resolved = ResolveFeedbackFilePath(config, path);
    if (!resolved)
    {
        return resolved;
    }
    auto helperPath = state.helperPath;
    if (helperPath.empty())
    {
        std::wstring executable(32768, L'\0');
        const auto count = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (count == 0 || count >= executable.size())
        {
            return NativeFailure("Cannot locate sibling feedback helper");
        }
        executable.resize(count);
        helperPath = std::filesystem::path(executable).parent_path() / L"PBFeedbackIo.exe";
    }
    if (!helperPath.is_absolute())
    {
        return {FeedbackError::InvalidPath, "Feedback helper path must be absolute"};
    }
    const auto random = pbprotocol::GenerateRandomSessionId();
    if (!random)
    {
        return {FeedbackError::Unavailable, "Cannot create feedback helper nonce"};
    }
    state.role = role;
    state.runToken = runToken;
    state.nonce = random.Value().bytes;
    state.receiverId = {};
    state.publishedSequence = 0;
    state.writingSequence = 0;
    state.lastProgress = 0;
    state.clockStarted = false;
    state.timedOut = false;
    state.status = {};
    state.transmitPosition = 0;
    const std::wstring pipeName = L"\\\\.\\pipe\\PixelBridgeFeedback-" + Hex(state.nonce);
    detail::FeedbackHandle server(CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 131072, 131072, 0, nullptr));
    if (!server)
    {
        return NativeFailure("Cannot create bounded feedback pipe");
    }
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    detail::FeedbackHandle client(CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, &security, OPEN_EXISTING, 0, nullptr));
    if (!client)
    {
        return NativeFailure("Cannot connect local feedback pipe");
    }
    if (!ConnectNamedPipe(server.Get(), nullptr) && GetLastError() != ERROR_PIPE_CONNECTED)
    {
        return NativeFailure("Cannot accept local feedback pipe");
    }
    DWORD clientMode = PIPE_READMODE_BYTE | PIPE_WAIT;
    if (!SetNamedPipeHandleState(client.Get(), &clientMode, nullptr, nullptr))
    {
        return NativeFailure("Cannot set worker pipe mode");
    }
    detail::FeedbackHandle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    if (!job || !SetInformationJobObject(job.Get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
    {
        return NativeFailure("Cannot constrain feedback helper job");
    }
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<std::byte> attributeStorage(attributeBytes);
    auto* const attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes))
    {
        return NativeFailure("Cannot initialize explicit helper handles");
    }
    const HANDLE inherited = client.Get();
    const bool updated = UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        const_cast<HANDLE*>(&inherited), sizeof(inherited), nullptr, nullptr) != FALSE;
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.lpAttributeList = attributes;
    std::wstring command = QuoteArgument(helperPath.native()) + (role == FeedbackIoRole::Reader ? L" --read " : L" --write ") +
        QuoteArgument(path.native()) + L" " + std::to_wstring(reinterpret_cast<std::uintptr_t>(inherited)) + L" " +
        std::to_wstring(runToken) + L" " + Hex(state.nonce);
    PROCESS_INFORMATION child{};
    const bool created = updated && command.size() < 32767 && CreateProcessW(helperPath.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, helperPath.parent_path().c_str(), &startup.StartupInfo, &child);
    const DWORD creationError = GetLastError();
    DeleteProcThreadAttributeList(attributes);
    if (!created)
    {
        return {FeedbackError::Unavailable, "Cannot start feedback helper", creationError};
    }
    state.process.Reset(child.hProcess);
    state.job = std::move(job);
    detail::FeedbackHandle thread(child.hThread);
    if (!AssignProcessToJobObject(state.job.Get(), state.process.Get()) || ResumeThread(thread.Get()) == static_cast<DWORD>(-1))
    {
        const auto failure = NativeFailure("Cannot safely start constrained feedback helper");
        state.Fail(failure);
        Stop();
        return failure;
    }
    state.pipe = std::move(server);
    state.accepting = true;
    return {};
}

FeedbackStatus FeedbackIoProcess::QueueSnapshot(const FeedbackSnapshot& snapshot, const std::uint64_t runToken)
{
    auto& state = *implementation_;
    if (!state.accepting || state.role != FeedbackIoRole::Writer || runToken != state.runToken)
    {
        return {FeedbackError::Superseded, "Feedback writer does not own this generation"};
    }
    const auto valid = ValidateFeedbackSnapshot(snapshot);
    if (!valid)
    {
        return valid;
    }
    state.pending = snapshot;
    return {};
}

FeedbackIoResult FeedbackIoProcess::Poll(const std::uint64_t nowNanoseconds)
{
    auto& state = *implementation_;
    FeedbackIoResult result;
    if (!state.clockStarted)
    {
        state.clockStarted = true;
        state.lastProgress = nowNanoseconds;
    }
    if (state.accepting && !state.IsRunning())
    {
        state.Fail({FeedbackError::Unavailable, "Feedback helper exited"});
    }
    // Every read is a local nonblocking read, every allocation is capped, and
    // at most eight messages can be handled in one owner iteration.
    for (unsigned int iteration = 0; state.accepting && iteration < 8; iteration++)
    {
        std::size_t expected = detail::feedbackIoHeaderBytes;
        if (state.receive.size() >= detail::feedbackIoHeaderBytes)
        {
            const auto valid = detail::GetFeedbackIoMessageSize(state.receive, expected);
            if (!valid)
            {
                state.Fail(valid);
                break;
            }
        }
        if (state.receive.size() < expected)
        {
            const auto previousSize = state.receive.size();
            state.receive.resize(expected);
            DWORD received = 0;
            const BOOL read = ReadFile(state.pipe.Get(), state.receive.data() + previousSize,
                static_cast<DWORD>(expected - previousSize), &received, nullptr);
            const auto error = read ? ERROR_SUCCESS : GetLastError();
            state.receive.resize(previousSize + received);
            if (!read && error != ERROR_NO_DATA)
            {
                state.Fail({FeedbackError::Unavailable, "Feedback pipe read failed", error});
                break;
            }
            if (received == 0)
            {
                break;
            }
            if (state.receive.size() < expected || expected == detail::feedbackIoHeaderBytes)
            {
                continue;
            }
        }
        detail::FeedbackIoMessage message;
        const auto valid = detail::DecodeFeedbackIoMessage(state.receive, message);
        state.receive.clear();
        if (!valid || message.runToken != state.runToken || message.nonce != state.nonce)
        {
            state.Fail({FeedbackError::InvalidFormat, "Rejected helper protocol or generation"});
            break;
        }
        if (message.type == detail::FeedbackIoMessageType::Ready && !state.ready && message.error == FeedbackError::None && message.body.size() == 17 &&
            message.body[0] == (state.role == FeedbackIoRole::Writer ? std::byte{1} : std::byte{0}))
        {
            std::copy_n(message.body.begin() + 1, 16, state.receiverId.begin());
            state.ready = true;
            state.status = {};
        }
        else if (message.type == detail::FeedbackIoMessageType::Error && message.error != FeedbackError::None && message.body.empty())
        {
            state.status = {message.error, "Feedback file unavailable or rejected", message.nativeError};
            state.writingSequence = 0;
        }
        else if (state.ready && state.role == FeedbackIoRole::Reader && message.type == detail::FeedbackIoMessageType::Snapshot && message.error == FeedbackError::None)
        {
            FeedbackSnapshot snapshot;
            const auto parsed = ParseFeedbackSnapshot(message.body, snapshot);
            if (!parsed)
            {
                state.Fail(parsed);
                break;
            }
            result.snapshot = std::move(snapshot);
            state.status = {};
        }
        else if (state.ready && state.role == FeedbackIoRole::Writer && message.type == detail::FeedbackIoMessageType::Published &&
            message.error == FeedbackError::None && message.body.size() == 8 && state.writingSequence != 0)
        {
            std::uint64_t sequence = 0;
            for (std::size_t index = 0; index < 8; index++)
            {
                sequence |= static_cast<std::uint64_t>(std::to_integer<unsigned int>(message.body[index])) << (index * 8U);
            }
            if (sequence != state.writingSequence)
            {
                state.Fail({FeedbackError::Conflict, "Feedback publication acknowledgement mismatch"});
                break;
            }
            state.publishedSequence = sequence;
            state.writingSequence = 0;
            state.status = {};
        }
        else
        {
            state.Fail({FeedbackError::InvalidFormat, "Unexpected helper reply"});
            break;
        }
        state.lastProgress = nowNanoseconds;
    }
    if (state.accepting && state.ready && state.role == FeedbackIoRole::Writer && state.writingSequence == 0 && state.transmit.empty() && state.pending)
    {
        auto snapshot = std::move(*state.pending);
        state.pending.reset();
        snapshot.receiverId = state.receiverId;
        detail::FeedbackIoMessage message;
        message.type = detail::FeedbackIoMessageType::WriteSnapshot;
        message.runToken = state.runToken;
        message.nonce = state.nonce;
        const auto serialized = SerializeFeedbackSnapshot(snapshot, message.body);
        const auto encoded = serialized ? detail::EncodeFeedbackIoMessage(message, state.transmit) : serialized;
        if (!encoded)
        {
            state.Fail(encoded);
        }
        else
        {
            state.writingSequence = snapshot.snapshotSequence;
            state.transmitPosition = 0;
            state.lastProgress = nowNanoseconds;
        }
    }
    if (state.accepting && !state.transmit.empty())
    {
        DWORD written = 0;
        const BOOL sent = WriteFile(state.pipe.Get(), state.transmit.data() + state.transmitPosition,
            static_cast<DWORD>(state.transmit.size() - state.transmitPosition), &written, nullptr);
        const auto error = sent ? ERROR_SUCCESS : GetLastError();
        if (!sent && error != ERROR_NO_DATA)
        {
            state.Fail({FeedbackError::Unavailable, "Feedback pipe write failed", error});
        }
        state.transmitPosition += written;
        if (state.transmitPosition == state.transmit.size())
        {
            state.transmit.clear();
            state.transmitPosition = 0;
        }
    }
    const bool operationPending = !state.ready || state.role == FeedbackIoRole::Reader || state.writingSequence != 0;
    if (state.accepting && operationPending && nowNanoseconds >= state.lastProgress && nowNanoseconds - state.lastProgress > operationTimeoutNanoseconds)
    {
        state.timedOut = true;
        state.Fail({FeedbackError::Unavailable, "Feedback helper operation timed out", WAIT_TIMEOUT});
    }
    if (!state.accepting)
    {
        result.snapshot.reset();
    }
    result.status = state.status;
    result.ready = state.ready;
    result.running = state.IsRunning();
    result.runToken = state.runToken;
    result.receiverId = state.receiverId;
    result.publishedSequence = state.publishedSequence;
    result.timedOut = state.timedOut;
    return result;
}
}
