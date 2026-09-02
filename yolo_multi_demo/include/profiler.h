#pragma once
//
// dxprof - per-stage timing profiler
// -----------------------------------------------------------------------------
// Lightweight profiler for comparing pipeline stages across platforms.
//
//  * ~50 ns per sample: two steady_clock reads plus a few relaxed atomics
//  * one slot per (channel, stage), so recording needs no locks
//  * a log histogram approximates p50/p90/p99
//  * a background thread dumps per-interval deltas, and Shutdown writes a
//    cumulative summary plus a CSV block
//
// Usage:
//    dxprof::Profiler::Instance().Init(cfg);      // at startup
//    DXPROF_SCOPE(ch, dxprof::ST_GET_INPUT);      // around a block
//    dxprof::Profiler::Instance().Shutdown();     // before exit
//
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace dxprof {

// ---------------------------------------------------------------------------
// Stages
// ---------------------------------------------------------------------------
enum Stage : int
{
    // Channel worker thread (ObjectDetection::threadFunc)
    ST_WORKER_LOOP = 0,   // whole iteration incl. sleep = channel frame period
    ST_WORKER_BUSY,       // work per iteration, excluding sleep
    ST_GET_INPUT,         // capture/decode + preprocess (GetInputStream)
    ST_RUN_ASYNC,         // NPU enqueue (InferenceEngine::RunAsync)
    ST_BBOX_SCALE,        // bbox coordinate scaling (incl. _lock wait)
    ST_GET_OUTPUT,        // source resize + bbox drawing (GetOutputStream)
    ST_BADGE,             // channel badge drawing
    ST_FRAME_SWAP,        // result frame swap (incl. _frameLock wait)
    ST_WORKER_SLEEP_REQ,  // sleep the worker asked for
    ST_WORKER_SLEEP_ACT,  // sleep it actually got (exposes the timer tick)

    // dxrt callback thread
    ST_POSTPROC_WAIT,     // waiting for _lock
    ST_POSTPROC,          // yolo.PostProc (decode + NMS, CPU)
    ST_POSTPROC_GAP,      // interval between callbacks = measured channel FPS

    // Values dxrt reports
    ST_NPU_INFER,         // GetNpuInferenceTime()
    ST_NPU_LATENCY,       // GetLatency()
    ST_NPU_ANOMALY,       // implausible dxrt values, kept out of the above

    // Main render/display thread
    ST_MAIN_LOOP,         // whole iteration incl. sleep
    ST_MAIN_BUSY,         // work per iteration, excluding sleep
    ST_COMPOSE,           // per-channel ResultFrame -> outFrame copyTo
    ST_FPS_CALC,          // FPS aggregation
    ST_HUD,               // header HUD rendering (not drawn on Windows)
    ST_IMSHOW,            // cv::imshow
    ST_WAITKEY,           // cv::waitKey(1)
    ST_WINPROP,           // cv::getWindowProperty (window-closed check)
    ST_MAIN_SLEEP_REQ,    // sleep the main loop asked for
    ST_MAIN_SLEEP_ACT,    // sleep it actually got

    ST_COUNT
};

inline const char* StageName(int s)
{
    static const char* kNames[ST_COUNT] = {
        "worker.loop",     "worker.busy",     "worker.get_input",  "worker.run_async",
        "worker.bbox",     "worker.get_output","worker.badge",     "worker.frame_swap",
        "worker.sleep_req","worker.sleep_act",
        "post.lock_wait",  "post.yolo",       "post.gap",
        "npu.infer",       "npu.latency",     "npu.anomaly",
        "main.loop",       "main.busy",       "main.compose",      "main.fps_calc",
        "main.hud",        "main.imshow",     "main.waitkey",      "main.winprop",
        "main.sleep_req",  "main.sleep_act",
    };
    return (s >= 0 && s < ST_COUNT) ? kNames[s] : "?";
}

// Main-thread stages, which record into the channel-0 slot.
inline bool IsMainStage(int s) { return s >= ST_MAIN_LOOP; }

// ---------------------------------------------------------------------------
// Histogram: 1 us buckets below 32 us, then 16 buckets per octave. That is a
// 6.25% bucket width, so midpoint-corrected percentiles land within ~3%. The
// last bucket ends near 67 s and larger samples clamp into it; max is tracked
// separately and stays exact.
// ---------------------------------------------------------------------------
static const int kHistBins  = 384;   // 32 + 16 * 22
static const int kLinearMax = 32;
static const int kMaxChannels = 112;   // 100-channel demo + main(0) + slack

inline int Log2Floor(uint64_t v)
{
#if defined(_MSC_VER)
  #if defined(_WIN64)
    unsigned long i = 0; _BitScanReverse64(&i, v); return (int)i;
  #else
    unsigned long i = 0;
    if(_BitScanReverse(&i, (unsigned long)(v >> 32))) return (int)i + 32;
    _BitScanReverse(&i, (unsigned long)v); return (int)i;
  #endif
#else
    return 63 - __builtin_clzll(v);
#endif
}

inline int BinOf(uint64_t us)
{
    if(us < (uint64_t)kLinearMax) return (int)us;
    int e = Log2Floor(us);                        // >= 5
    int m = (int)((us >> (e - 4)) & 0xFULL);      // mantissa 4bit
    int b = kLinearMax + (e - 5) * 16 + m;
    return (b < kHistBins) ? b : (kHistBins - 1);
}

// Lower bound of a bin, in us.
inline uint64_t BinLowerBound(int b)
{
    if(b < kLinearMax) return (uint64_t)b;
    int idx = b - kLinearMax;
    int e = 5 + idx / 16;
    int m = idx % 16;
    return ((uint64_t)(16 + m)) << (e - 4);
}

// ---------------------------------------------------------------------------
// One slot per (channel, stage); in practice a single writer.
// ---------------------------------------------------------------------------
struct Stat
{
    std::atomic<uint64_t> count{0};
    std::atomic<uint64_t> sum{0};
    std::atomic<uint64_t> minv{UINT64_MAX};
    std::atomic<uint64_t> maxv{0};
    std::atomic<uint32_t> hist[kHistBins];

    Stat() { for(int i = 0; i < kHistBins; i++) hist[i].store(0, std::memory_order_relaxed); }

    inline void Add(uint64_t us)
    {
        count.fetch_add(1, std::memory_order_relaxed);
        sum.fetch_add(us, std::memory_order_relaxed);
        uint64_t m = minv.load(std::memory_order_relaxed);
        while(us < m && !minv.compare_exchange_weak(m, us, std::memory_order_relaxed)) {}
        uint64_t M = maxv.load(std::memory_order_relaxed);
        while(us > M && !maxv.compare_exchange_weak(M, us, std::memory_order_relaxed)) {}
        hist[BinOf(us)].fetch_add(1, std::memory_order_relaxed);
    }
};

// Non-atomic copy used for interval deltas.
struct StatSnap
{
    uint64_t count = 0, sum = 0, minv = UINT64_MAX, maxv = 0;
    uint32_t hist[kHistBins];
    StatSnap() { std::memset(hist, 0, sizeof(hist)); }
};

struct Summary
{
    uint64_t count = 0;
    double   avg = 0, p50 = 0, p90 = 0, p99 = 0;
    uint64_t minv = 0, maxv = 0, total = 0;
};

inline double PercentileFromHist(const uint32_t* hist, uint64_t /*count*/, double q)
{
    uint64_t total = 0;
    for(int b = 0; b < kHistBins; b++) total += hist[b];
    if(total == 0) return 0.0;
    uint64_t target = (uint64_t)(total * q);
    if(target >= total) target = total - 1;
    uint64_t acc = 0;
    for(int b = 0; b < kHistBins; b++)
    {
        acc += hist[b];
        if(acc > target)
        {
            uint64_t lo = BinLowerBound(b);
            uint64_t hi = (b + 1 < kHistBins) ? BinLowerBound(b + 1) : (lo * 2);
            return (double)(lo + hi) / 2.0;
        }
    }
    return (double)BinLowerBound(kHistBins - 1);
}

// ---------------------------------------------------------------------------
// Process and system memory, used to tell whether the working set is being
// paged out. A fault count that keeps climbing means pages are being re-read
// from disk.
// ---------------------------------------------------------------------------
struct MemSnapshot
{
    uint64_t rss_mb        = 0;   // resident (working set)
    uint64_t commit_mb     = 0;   // committed / reserved
    uint64_t peak_rss_mb   = 0;
    uint64_t faults        = 0;   // Linux: major only. Windows: all faults
    uint64_t sys_avail_mb  = 0;   // system-wide available physical memory
    uint64_t sys_total_mb  = 0;
    bool     faults_are_major = false;  // false when soft faults are included
    bool     valid         = false;
};

bool ReadMemSnapshot(MemSnapshot& out);

/// Appends one line to each interval report. Injected from main so the
/// profiler carries no dxrt dependency.
using PeriodicSampler = std::function<std::string()>;

struct Config
{
    bool        enabled   = false;
    std::string path;
    int         period_ms = 5000;
    int         warmup_ms = 3000;
    bool        per_channel = true;
};

// ---------------------------------------------------------------------------
// Profiler
// ---------------------------------------------------------------------------
class Profiler
{
public:
    static Profiler& Instance() { static Profiler p; return p; }

    // Checked on the hot path; false before Init and during warmup.
    static std::atomic<bool>& Active() { static std::atomic<bool> a{false}; return a; }

    void Init(const Config& cfg, const std::string& envInfo);
    void Shutdown();

    inline void Add(int ch, int stage, uint64_t us)
    {
        if(ch < 0 || ch >= kMaxChannels || stage < 0 || stage >= ST_COUNT) return;
        _stats[ch][stage].Add(us);
    }

    // Records the time since the previous call, e.g. a callback interval.
    inline void Mark(int ch, int stage)
    {
        if(ch < 0 || ch >= kMaxChannels || stage < 0 || stage >= ST_COUNT) return;
        auto now = std::chrono::steady_clock::now();
        auto& prev = _lastMark[ch][stage];
        if(prev.time_since_epoch().count() != 0)
        {
            Add(ch, stage, (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(now - prev).count());
        }
        prev = now;
    }

    void SetChannelCount(int n) { _numChannels = (n < kMaxChannels) ? n : kMaxChannels - 1; }

    /// Called from the dump thread; add e.g. NPU temperature and clock.
    void SetPeriodicSampler(PeriodicSampler fn)
    {
        std::lock_guard<std::mutex> lk(_samplerMutex);
        _sampler = std::move(fn);
    }
    bool Enabled() const { return _cfg.enabled; }
    const std::string& Path() const { return _cfg.path; }

private:
    Profiler() = default;
    ~Profiler() { Shutdown(); }
    Profiler(const Profiler&) = delete;
    Profiler& operator=(const Profiler&) = delete;

    void DumpLoop();
    void WriteReport(std::ostream& os, const char* title,
                     const std::vector<std::vector<StatSnap>>& snap,
                     const std::vector<std::vector<StatSnap>>* base,
                     double wallSec, bool csv);

    static Summary Reduce(const StatSnap& s);
    static StatSnap Delta(const StatSnap& cur, const StatSnap& base);
    void Snapshot(std::vector<std::vector<StatSnap>>& out) const;

    Config _cfg;
    std::string _envInfo;
    int  _numChannels = 0;
    Stat _stats[kMaxChannels][ST_COUNT];
    std::chrono::steady_clock::time_point _lastMark[kMaxChannels][ST_COUNT];

    std::ofstream _ofs;
    std::thread   _thread;
    std::mutex    _cvMutex;
    std::condition_variable _cv;
    std::atomic<bool> _running{false};
    std::atomic<bool> _measured{false};   // cleared warmup and started measuring
    PeriodicSampler _sampler;
    std::mutex      _samplerMutex;
    double _canaryBase = 0.0;             // CPU canary baseline, in us
    MemSnapshot _memPrev;                 // previous sample, for fault deltas
    std::chrono::steady_clock::time_point _memPrevT;
    std::chrono::steady_clock::time_point _t0;
    int _intervalNo = 0;
};

// ---------------------------------------------------------------------------
// Scoped timer
// ---------------------------------------------------------------------------
class ScopedTimer
{
public:
    ScopedTimer(int ch, int stage)
        : _ch(ch), _stage(stage), _on(Profiler::Active().load(std::memory_order_relaxed))
    {
        if(_on) _s = std::chrono::steady_clock::now();
    }
    ~ScopedTimer() { Stop(); }
    // Stop early, e.g. to time only a lock wait.
    uint64_t Stop()
    {
        if(!_on) return 0;
        _on = false;
        uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - _s).count();
        Profiler::Instance().Add(_ch, _stage, us);
        return us;
    }
private:
    int _ch, _stage;
    bool _on;
    std::chrono::steady_clock::time_point _s;
};

// Manual timer, reusable across stages.
class Stopwatch
{
public:
    Stopwatch() { Reset(); }
    void Reset() { _s = std::chrono::steady_clock::now(); }
    uint64_t LapUs()
    {
        auto now = std::chrono::steady_clock::now();
        uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(now - _s).count();
        _s = now;
        return us;
    }
    uint64_t ElapsedUs() const
    {
        return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - _s).count();
    }
private:
    std::chrono::steady_clock::time_point _s;
};

std::string CollectEnvInfo(const std::string& extra);

} // namespace dxprof

#define DXPROF_CAT2(a, b) a##b
#define DXPROF_CAT(a, b) DXPROF_CAT2(a, b)
// Time a block
#define DXPROF_SCOPE(ch, stage) dxprof::ScopedTimer DXPROF_CAT(_dxprof_, __LINE__)((ch), (stage))
// Record a value directly
#define DXPROF_ADD(ch, stage, us)                                                   \
    do { if(dxprof::Profiler::Active().load(std::memory_order_relaxed))             \
             dxprof::Profiler::Instance().Add((ch), (stage), (uint64_t)(us)); } while(0)
// Record the interval since the previous call
#define DXPROF_MARK(ch, stage)                                                      \
    do { if(dxprof::Profiler::Active().load(std::memory_order_relaxed))             \
             dxprof::Profiler::Instance().Mark((ch), (stage)); } while(0)
