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
    schedule.Configure(15);
    require(schedule.CanSubmit(start), "first frame must be eligible");
    schedule.Submitted(start);
    require(!schedule.CanSubmit(start + seconds(1)), "one request in flight");
    schedule.Completed();
    require(!schedule.CanSubmit(start + milliseconds(66)), "15 FPS interval");
    require(schedule.CanSubmit(start + milliseconds(67)), "interval expired");
    schedule.Submitted(start + seconds(10));
    schedule.Completed();
    require(!schedule.CanSubmit(start + seconds(10)), "no catch-up burst");

    for(int fps : {1, 5, 15, 30, 1000})
    {
        schedule.Configure(fps);
        int submissions = 0;
        for(int ms = 0; ms < 3000; ++ms)
        {
            if(schedule.CanSubmit(start + milliseconds(ms)))
            {
                schedule.Submitted(start + milliseconds(ms));
                ++submissions;
                schedule.Completed();
            }
        }
        require(submissions <= fps * 3, "configured FPS upper bound");
        require(submissions > fps * 2, "inference must continue across repeated playback");
    }
    std::cout << "Continuous inference schedule tests passed\n";
}
