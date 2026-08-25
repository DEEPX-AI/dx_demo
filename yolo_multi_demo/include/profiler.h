#pragma once
//
// dxprof - 구간별(stage) 실행시간 프로파일러
// -----------------------------------------------------------------------------
// Linux / Windows 성능 차이를 구간 단위로 비교하기 위한 경량 프로파일러.
//
//  * 샘플 1건당 비용: steady_clock 2회 + relaxed atomic 몇 개 (~50ns)
//  * 채널(스레드)별 / stage별로 슬롯이 분리되어 있어 lock 없이 기록
//  * 로그 히스토그램(2 mantissa bit)으로 p50/p90/p99 를 근사
//  * 별도 스레드가 주기적으로 "구간 델타"를 파일에 덤프하고,
//    종료 시 누적 요약 + CSV 블록을 남긴다.
//
// 사용:
//    dxprof::Profiler::Instance().Init(cfg);      // main 시작 시
//    DXPROF_SCOPE(ch, dxprof::ST_GET_INPUT);      // 측정할 블록에
//    dxprof::Profiler::Instance().Shutdown();     // 종료 시
//
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
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
// Stage 정의
// ---------------------------------------------------------------------------
enum Stage : int
{
    // --- 채널 워커 스레드 (ObjectDetection::threadFunc) ---
    ST_WORKER_LOOP = 0,   // 루프 1회 전체 (sleep 포함) = 채널 프레임 주기
    ST_WORKER_BUSY,       // 루프 1회 중 실제 작업 구간 (sleep 제외)
    ST_GET_INPUT,         // 캡처/디코드 + 전처리 (VideoStream::GetInputStream)
    ST_RUN_ASYNC,         // NPU 추론 enqueue (InferenceEngine::RunAsync)
    ST_BBOX_SCALE,        // 결과 bbox 좌표 스케일링 (+ _lock 대기)
    ST_GET_OUTPUT,        // 원본 리사이즈 + bbox 드로잉 (GetOutputStream)
    ST_BADGE,             // CH 배지 드로잉
    ST_FRAME_SWAP,        // 결과 프레임 swap (+ _frameLock 대기)
    ST_WORKER_SLEEP_REQ,  // 워커가 "요청한" sleep 시간
    ST_WORKER_SLEEP_ACT,  // 워커가 "실제로" 잔 시간  (Windows 타이머 해상도 확인용)

    // --- dxrt 콜백 스레드 ---
    ST_POSTPROC_WAIT,     // _lock 획득 대기
    ST_POSTPROC,          // yolo.PostProc (decode + NMS, CPU)
    ST_POSTPROC_GAP,      // 콜백 간 간격 = 채널 실측 추론 주기

    // --- dxrt 리포트 값 ---
    ST_NPU_INFER,         // GetNpuInferenceTime()
    ST_NPU_LATENCY,       // GetLatency()

    // --- 메인 스레드 (렌더/디스플레이 루프) ---
    ST_MAIN_LOOP,         // 루프 1회 전체 (sleep 포함)
    ST_MAIN_BUSY,         // 루프 1회 중 실제 작업 구간
    ST_COMPOSE,           // 각 채널 ResultFrame -> outFrame copyTo
    ST_FPS_CALC,          // FPS 집계 연산
    ST_HUD,               // 헤더 HUD 렌더링 (Linux 전용)
    ST_IMSHOW,            // cv::imshow
    ST_WAITKEY,           // cv::waitKey(1)
    ST_WINPROP,           // cv::getWindowProperty (창 닫힘 확인)
    ST_MAIN_SLEEP_REQ,    // 메인이 "요청한" sleep 시간
    ST_MAIN_SLEEP_ACT,    // 메인이 "실제로" 잔 시간

    ST_COUNT
};

inline const char* StageName(int s)
{
    static const char* kNames[ST_COUNT] = {
        "worker.loop",     "worker.busy",     "worker.get_input",  "worker.run_async",
        "worker.bbox",     "worker.get_output","worker.badge",     "worker.frame_swap",
        "worker.sleep_req","worker.sleep_act",
        "post.lock_wait",  "post.yolo",       "post.gap",
        "npu.infer",       "npu.latency",
        "main.loop",       "main.busy",       "main.compose",      "main.fps_calc",
        "main.hud",        "main.imshow",     "main.waitkey",      "main.winprop",
        "main.sleep_req",  "main.sleep_act",
    };
    return (s >= 0 && s < ST_COUNT) ? kNames[s] : "?";
}

// 메인 스레드가 쓰는 stage 인지 (채널 0 슬롯 사용)
inline bool IsMainStage(int s) { return s >= ST_MAIN_LOOP; }

// ---------------------------------------------------------------------------
// 히스토그램: 0~31us 는 1us 단위, 그 이상은 옥타브당 16구간
//   -> 버킷 폭 6.25%, 중앙값 보정 후 백분위 오차 3% 이내.
//   -> 마지막 버킷은 약 67초. 그 이상은 clamp (max 는 별도 추적).
// ---------------------------------------------------------------------------
static const int kHistBins  = 384;   // 32 + 16 * 22
static const int kLinearMax = 32;
static const int kMaxChannels = 112;   // 100ch 데모 + 메인(0) + 여유

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

// 해당 bin 의 하한값(us)
inline uint64_t BinLowerBound(int b)
{
    if(b < kLinearMax) return (uint64_t)b;
    int idx = b - kLinearMax;
    int e = 5 + idx / 16;
    int m = idx % 16;
    return ((uint64_t)(16 + m)) << (e - 4);
}

// ---------------------------------------------------------------------------
// Stat 슬롯 (채널 x stage 마다 1개, 사실상 단일 writer)
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

// 스냅샷(비원자 복사본) - 델타 계산용
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
// 프로세스/시스템 메모리 스냅샷
//   페이징이 일어나고 있는지 판정하기 위한 값들.
//   major_faults 가 계속 증가하면 디스크에서 페이지를 다시 읽고 있다는 뜻이다.
// ---------------------------------------------------------------------------
struct MemSnapshot
{
    uint64_t rss_mb        = 0;   // 실제 물리 메모리에 올라와 있는 양 (working set)
    uint64_t commit_mb     = 0;   // 커밋(예약)한 양
    uint64_t peak_rss_mb   = 0;
    uint64_t faults        = 0;   // Linux: major fault, Windows: 전체 page fault
    uint64_t sys_avail_mb  = 0;   // 시스템 전체 가용 물리 메모리
    uint64_t sys_total_mb  = 0;
    bool     faults_are_major = false;  // Linux 는 major 만, Windows 는 soft 포함
    bool     valid         = false;
};

bool ReadMemSnapshot(MemSnapshot& out);

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

    // 핫패스에서 검사되는 플래그. Init 이전/warmup 중에는 false.
    static std::atomic<bool>& Active() { static std::atomic<bool> a{false}; return a; }

    void Init(const Config& cfg, const std::string& envInfo);
    void Shutdown();

    inline void Add(int ch, int stage, uint64_t us)
    {
        if(ch < 0 || ch >= kMaxChannels || stage < 0 || stage >= ST_COUNT) return;
        _stats[ch][stage].Add(us);
    }

    // 직전 호출 이후 경과시간을 기록 (콜백 간격 등)
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
    std::atomic<bool> _measured{false};   // warmup 을 넘겨 실제 측정에 들어갔는지
    MemSnapshot _memPrev;                 // 구간별 page fault 델타 계산용
    std::chrono::steady_clock::time_point _memPrevT;
    std::chrono::steady_clock::time_point _t0;
    int _intervalNo = 0;
};

// ---------------------------------------------------------------------------
// 스코프 타이머
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
    // 명시적으로 조기 종료 (lock 대기 구간 측정 등)
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

// 여러 stage 에 걸쳐 재사용하는 수동 타이머
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
// 블록 구간 측정
#define DXPROF_SCOPE(ch, stage) dxprof::ScopedTimer DXPROF_CAT(_dxprof_, __LINE__)((ch), (stage))
// 값 직접 기록
#define DXPROF_ADD(ch, stage, us)                                                   \
    do { if(dxprof::Profiler::Active().load(std::memory_order_relaxed))             \
             dxprof::Profiler::Instance().Add((ch), (stage), (uint64_t)(us)); } while(0)
// 직전 호출과의 간격 기록
#define DXPROF_MARK(ch, stage)                                                      \
    do { if(dxprof::Profiler::Active().load(std::memory_order_relaxed))             \
             dxprof::Profiler::Instance().Mark((ch), (stage)); } while(0)
