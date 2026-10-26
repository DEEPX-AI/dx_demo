#pragma once

#include <chrono>

class InferenceSchedule
{
public:
    using Clock = std::chrono::steady_clock;

    void Configure(int fps, bool benchmark)
    {
        _interval = benchmark ? Clock::duration::zero()
            : std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(1.0 / fps));
        _cacheEnabled = !benchmark;
        _started = false;
        _pending = false;
    }

    bool CanSubmit(Clock::time_point now, bool cached) const
    {
        return !_pending && !(_cacheEnabled && cached)
            && (!_started || now - _lastSubmission >= _interval);
    }

    void Submitted(Clock::time_point now)
    {
        _lastSubmission = now;
        _started = true;
        _pending = true;
    }

    void Completed() { _pending = false; }
    bool Pending() const { return _pending; }
    bool CacheEnabled() const { return _cacheEnabled; }

private:
    Clock::duration _interval{};
    Clock::time_point _lastSubmission{};
    bool _cacheEnabled = true;
    bool _started = false;
    bool _pending = false;
};
