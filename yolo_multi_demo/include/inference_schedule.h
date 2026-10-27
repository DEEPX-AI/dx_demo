#pragma once

#include <chrono>

class InferenceSchedule
{
public:
    using Clock = std::chrono::steady_clock;

    void Configure(int fps)
    {
        _interval = std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(1.0 / fps));
        _started = false;
        _pending = false;
    }

    bool CanSubmit(Clock::time_point now) const
    {
        return !_pending && (!_started || now - _lastSubmission >= _interval);
    }

    void Submitted(Clock::time_point now)
    {
        _lastSubmission = now;
        _started = true;
        _pending = true;
    }

    void Completed() { _pending = false; }
    bool Pending() const { return _pending; }

private:
    Clock::duration _interval{};
    Clock::time_point _lastSubmission{};
    bool _started = false;
    bool _pending = false;
};
