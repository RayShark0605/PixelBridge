#pragma once

#ifndef PB_RESUME_BATCH_TESTS
#error Resume I/O hooks are restricted to the dedicated test target.
#endif

#include <Windows.h>
#include <cstddef>
#include <span>

namespace pbapp::test
{

[[nodiscard]] BOOL SeekJournalAppend(HANDLE file, LARGE_INTEGER offset) noexcept;
[[nodiscard]] BOOL WriteJournalAppend(HANDLE file, std::span<const std::byte> bytes, DWORD& writtenBytes) noexcept;
[[nodiscard]] BOOL FlushJournalAppend(HANDLE file) noexcept;
void BeforeJournalBatchSerialize();

} // namespace pbapp::test
