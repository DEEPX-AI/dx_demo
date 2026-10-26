#include "inference_schedule.h"
#include <iostream>
#include <stdexcept>

static void require(bool value, const char* message)
{
    if(!value) throw std::runtime_error(message);
}

int main()
{
    using namespace std::chrono;
    using Clock = InferenceSchedule::Clock;
    const Clock::time_point start{};
    InferenceSchedule schedule;
    schedule.Configure(15, false);
    require(schedule.CacheEnabled(), "normal mode must cache");
    require(schedule.CanSubmit(start, false), "first frame must be eligible");
    schedule.Submitted(start);
    require(!schedule.CanSubmit(start + seconds(1), false), "one request in flight");
    schedule.Completed();
    require(!schedule.CanSubmit(start + milliseconds(66), false), "15 FPS interval");
    require(schedule.CanSubmit(start + milliseconds(67), false), "interval expired");
    require(!schedule.CanSubmit(start + seconds(1), true), "cached frames must not infer");
    schedule.Submitted(start + seconds(10));
    schedule.Completed();
    require(!schedule.CanSubmit(start + seconds(10), false), "no catch-up burst");

    schedule.Configure(15, true);
    require(!schedule.CacheEnabled(), "benchmark must bypass cache");
    require(schedule.CanSubmit(start, true), "benchmark may repeat cached content");
    schedule.Submitted(start);
    require(!schedule.CanSubmit(start + seconds(1), true), "benchmark still bounded in flight");
    schedule.Completed();
    require(schedule.CanSubmit(start, true), "benchmark uses capture pacing, not 15 FPS cap");

    schedule.Configure(1, false);
    schedule.Submitted(start);
    schedule.Completed();
    require(!schedule.CanSubmit(start + milliseconds(999), false), "1 FPS cap");
    require(schedule.CanSubmit(start + seconds(1), false), "1 FPS boundary");

    schedule.Configure(15, false);
    int submissions = 0;
    for(int ms = 0; ms < 1000; ++ms)
    {
        if(schedule.CanSubmit(start + milliseconds(ms), false))
        {
            schedule.Submitted(start + milliseconds(ms));
            ++submissions;
            schedule.Completed();
        }
    }
    require(submissions == 15, "measured one-second submission count must be 15");
    std::cout << "Inference schedule tests passed\n";
}
