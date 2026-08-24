#pragma once

#include <algorithm>
#include <cstddef>
#include <memory_resource>
#include <span>
#include <utility>

namespace pbprotocol::detail {

// A small allocator-aware byte owner used on failure-injection paths. MSVC's
// Debug STL allocates iterator-proxy state from an allocator inside several
// container constructors that are declared noexcept. A bounded or deliberately
// failing memory_resource can therefore turn an ordinary allocation failure
// into std::terminate before the receiver can return ResourceExhausted. This
// owner performs every allocation from an explicitly throwing operation, so
// callers can catch and classify failures consistently in Debug and Release.
class PmrByteBuffer final
{
public:
    explicit PmrByteBuffer(
        std::pmr::memory_resource* memoryResource) noexcept
        : memoryResource_(memoryResource)
    {
    }

    ~PmrByteBuffer()
    {
        Reset();
    }

    PmrByteBuffer(const PmrByteBuffer&) = delete;
    PmrByteBuffer& operator=(const PmrByteBuffer&) = delete;

    PmrByteBuffer(PmrByteBuffer&& other) noexcept
        : memoryResource_(other.memoryResource_),
          data_(std::exchange(other.data_, nullptr)),
          size_(std::exchange(other.size_, 0))
    {
    }

    PmrByteBuffer& operator=(PmrByteBuffer&& other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }

        Reset();
        memoryResource_ = other.memoryResource_;
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
        return *this;
    }

    void Assign(const std::span<const std::byte> bytes)
    {
        if (bytes.empty())
        {
            Reset();
            return;
        }

        std::byte* const newData = static_cast<std::byte*>(
            memoryResource_->allocate(bytes.size(), alignof(std::byte)));
        std::copy(bytes.begin(), bytes.end(), newData);

        Reset();
        data_ = newData;
        size_ = bytes.size();
    }

    void ResizeZeroed(const std::size_t size)
    {
        if (size == 0)
        {
            Reset();
            return;
        }

        std::byte* const newData = static_cast<std::byte*>(
            memoryResource_->allocate(size, alignof(std::byte)));
        std::fill_n(newData, size, std::byte{0});

        Reset();
        data_ = newData;
        size_ = size;
    }

    [[nodiscard]] std::span<const std::byte> Bytes() const noexcept
    {
        return std::span<const std::byte>(data_, size_);
    }

    [[nodiscard]] std::span<std::byte> MutableBytes() noexcept
    {
        return std::span<std::byte>(data_, size_);
    }

    [[nodiscard]] std::size_t Size() const noexcept
    {
        return size_;
    }

private:
    void Reset() noexcept
    {
        if (data_ == nullptr)
        {
            size_ = 0;
            return;
        }

        memoryResource_->deallocate(data_, size_, alignof(std::byte));
        data_ = nullptr;
        size_ = 0;
    }

    std::pmr::memory_resource* memoryResource_ = nullptr;
    std::byte* data_ = nullptr;
    std::size_t size_ = 0;
};

} // namespace pbprotocol::detail
