#pragma once

namespace pbrenderd3d
{

// Owner-thread state for one completion query; a permit is never a visibility acknowledgement.
class GpuSubmissionPermit
{
public:
    [[nodiscard]] bool NeedsCompletion() const noexcept
    {
        return state_ == State::InFlight;
    }
    [[nodiscard]] bool TryAcquire(const bool completed) noexcept
    {
        if (state_ == State::Acquired || (state_ == State::InFlight && !completed))
        {
            return false;
        }
        state_ = State::Acquired;
        return true;
    }
    [[nodiscard]] bool Submit() noexcept
    {
        if (state_ != State::Acquired)
        {
            return false;
        }
        state_ = State::InFlight;
        return true;
    }
    // Only after initialization, a successful drain, or resource destruction.
    void Reset() noexcept
    {
        state_ = State::Ready;
    }

private:
    enum class State
    {
        Ready,
        Acquired,
        InFlight
    };
    State state_ = State::Ready;
};

}
