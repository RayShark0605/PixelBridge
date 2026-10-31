#pragma once

#include <Windows.h>
#include <utility>

namespace pbapp::detail
{

class FeedbackHandle
{
public:
    explicit FeedbackHandle(const HANDLE value = INVALID_HANDLE_VALUE) noexcept : value_(value)
    {
    }
    ~FeedbackHandle()
    {
        Reset();
    }
    FeedbackHandle(const FeedbackHandle&) = delete;
    FeedbackHandle& operator=(const FeedbackHandle&) = delete;
    FeedbackHandle(FeedbackHandle&& other) noexcept : value_(other.Release())
    {
    }
    FeedbackHandle& operator=(FeedbackHandle&& other) noexcept
    {
        if (this != &other)
        {
            Reset(other.Release());
        }
        return *this;
    }
    HANDLE Get() const noexcept
    {
        return value_;
    }
    explicit operator bool() const noexcept
    {
        return value_ != INVALID_HANDLE_VALUE && value_ != nullptr;
    }
    HANDLE Release() noexcept
    {
        return std::exchange(value_, INVALID_HANDLE_VALUE);
    }
    void Reset(const HANDLE value = INVALID_HANDLE_VALUE) noexcept
    {
        if (*this)
        {
            CloseHandle(value_);
        }
        value_ = value;
    }
private:
    HANDLE value_ = INVALID_HANDLE_VALUE;
};

}
