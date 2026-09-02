#include "profiler.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <opencv2/opencv.hpp>

#ifdef _WIN32
  #include <windows.h>
  #include <psapi.h>
  #include <intrin.h>
#else
  #include <unistd.h>
  #include <sys/utsname.h>
  #if defined(__linux__)
    #include <sys/sysinfo.h>
  #endif
  #if defined(__x86_64__) || defined(__i386__)
    #include <cpuid.h>
  #endif
#endif

namespace dxprof {

// ---------------------------------------------------------------------------
// Environment info
// ---------------------------------------------------------------------------
static std::string CpuBrand()
{
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    char brand[49] = {0};
    unsigned int regs[4] = {0, 0, 0, 0};
  #if defined(_MSC_VER)
    int r[4];
    __cpuid(r, 0x80000000);
    if((unsigned int)r[0] < 0x80000004u) return "unknown";
    for(unsigned int i = 0; i < 3; i++)
    {
        __cpuid(r, 0x80000002 + i);
        std::memcpy(brand + i * 16, r, 16);
    }
  #else
    if(__get_cpuid(0x80000000, &regs[0], &regs[1], &regs[2], &regs[3]) == 0
       || regs[0] < 0x80000004u) return "unknown";
    for(unsigned int i = 0; i < 3; i++)
    {
        if(__get_cpuid(0x80000002 + i, &regs[0], &regs[1], &regs[2], &regs[3]) == 0) break;
        std::memcpy(brand + i * 16, regs, 16);
    }
  #endif
    std::string s(brand);
    // Trim surrounding whitespace.
    size_t b = s.find_first_not_of(' ');
    size_t e = s.find_last_not_of(" \t\r\n\0");
    return (b == std::string::npos) ? "unknown" : s.substr(b, e - b + 1);
#else
    return "non-x86";
#endif
}

static std::string OsInfo()
{
#ifdef _WIN32
    std::ostringstream os;
    os << "Windows";
    typedef LONG (WINAPI *RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
    HMODULE h = GetModuleHandleW(L"ntdll.dll");
    if(h)
    {
        RtlGetVersionPtr fn = (RtlGetVersionPtr)GetProcAddress(h, "RtlGetVersion");
        if(fn)
        {
            RTL_OSVERSIONINFOW vi;
            std::memset(&vi, 0, sizeof(vi));
            vi.dwOSVersionInfoSize = sizeof(vi);
            if(fn(&vi) == 0)
                os << " " << vi.dwMajorVersion << "." << vi.dwMinorVersion
                   << " (build " << vi.dwBuildNumber << ")";
        }
    }
    return os.str();
#else
    struct utsname u;
    if(uname(&u) == 0)
        return std::string(u.sysname) + " " + u.release + " " + u.machine;
    return "unix";
#endif
}

static uint64_t TotalRamMB()
{
#ifdef _WIN32
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if(GlobalMemoryStatusEx(&ms)) return (uint64_t)(ms.ullTotalPhys / (1024ull * 1024ull));
    return 0;
#elif defined(__linux__)
    struct sysinfo si;
    if(sysinfo(&si) == 0) return (uint64_t)((uint64_t)si.totalram * si.mem_unit / (1024ull * 1024ull));
    return 0;
#else
    return 0;
#endif
}

// ---------------------------------------------------------------------------
// Process and system memory, plus page faults
// ---------------------------------------------------------------------------
bool ReadMemSnapshot(MemSnapshot& out)
{
#ifdef _WIN32
    const uint64_t MB = 1024ull * 1024ull;
    PROCESS_MEMORY_COUNTERS_EX pmc;
    std::memset(&pmc, 0, sizeof(pmc));
    pmc.cb = sizeof(pmc);
    if(GetProcessMemoryInfo(GetCurrentProcess(),
                            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
    {
        out.rss_mb      = (uint64_t)pmc.WorkingSetSize / MB;
        out.peak_rss_mb = (uint64_t)pmc.PeakWorkingSetSize / MB;
        out.commit_mb   = (uint64_t)pmc.PrivateUsage / MB;
        // Windows counts soft faults here too and offers no hard-fault-only
        // API, so only the rate of increase is meaningful.
        out.faults      = (uint64_t)pmc.PageFaultCount;
        out.faults_are_major = false;
        out.valid = true;
    }
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if(GlobalMemoryStatusEx(&ms))
    {
        out.sys_avail_mb = (uint64_t)(ms.ullAvailPhys / MB);
        out.sys_total_mb = (uint64_t)(ms.ullTotalPhys / MB);
    }
    return out.valid;
#elif defined(__linux__)
    // VmRSS / VmPeak / VmSize
    {
        std::ifstream f("/proc/self/status");
        std::string line;
        while(std::getline(f, line))
        {
            unsigned long long kb = 0;
            if(std::sscanf(line.c_str(), "VmRSS: %llu kB", &kb) == 1)       out.rss_mb = kb / 1024;
            else if(std::sscanf(line.c_str(), "VmHWM: %llu kB", &kb) == 1)  out.peak_rss_mb = kb / 1024;
            else if(std::sscanf(line.c_str(), "VmSize: %llu kB", &kb) == 1) out.commit_mb = kb / 1024;
        }
    }
    // Field 12 of /proc/self/stat is majflt: faults served from disk.
    {
        std::ifstream f("/proc/self/stat");
        std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        size_t rp = all.rfind(')');
        if(rp != std::string::npos)
        {
            std::istringstream is(all.substr(rp + 1));
            std::string tok;
            for(int i = 0; i <= 9; i++) if(!(is >> tok)) { tok.clear(); break; }
            if(!tok.empty()) out.faults = std::strtoull(tok.c_str(), nullptr, 10);
        }
        out.faults_are_major = true;
    }
    // MemAvailable / MemTotal
    {
        std::ifstream f("/proc/meminfo");
        std::string line;
        while(std::getline(f, line))
        {
            unsigned long long kb = 0;
            if(std::sscanf(line.c_str(), "MemAvailable: %llu kB", &kb) == 1) out.sys_avail_mb = kb / 1024;
            else if(std::sscanf(line.c_str(), "MemTotal: %llu kB", &kb) == 1) out.sys_total_mb = kb / 1024;
        }
    }
    out.valid = (out.rss_mb > 0);
    return out.valid;
#else
    (void)out;
    return false;
#endif
}

// ---------------------------------------------------------------------------
// CPU speed canary. Runs a fixed amount of work each interval, so the number
// grows when the CPU throttles. Portable, with no per-OS clock API, and small
// enough to stay in L1 so memory bandwidth does not affect it. Taking the
// minimum of several trials filters out contention noise.
// ---------------------------------------------------------------------------
static std::atomic<uint32_t> g_canarySink{0};

static double CpuCanaryUs()
{
    static std::vector<uint32_t> buf;
    if(buf.empty())
    {
        buf.resize(4096);
        for(size_t i = 0; i < buf.size(); i++)
            buf[i] = (uint32_t)(i * 2654435761u);
    }
    double best = 1e18;
    for(int trial = 0; trial < 5; trial++)
    {
        auto s = std::chrono::steady_clock::now();
        uint32_t acc = 1u;
        for(int rep = 0; rep < 32; rep++)
            for(size_t i = 0; i < buf.size(); i++)
                acc = acc * 1664525u + buf[i] + 1013904223u;
        double us = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - s).count();
        g_canarySink.store(acc, std::memory_order_relaxed);   // defeat DCE
        if(us < best) best = us;
    }
    return best;
}

// Actual steady_clock resolution: smallest gap where the value changes.
static double ClockGranularityUs()
{
    double best = 1e9;
    for(int i = 0; i < 20; i++)
    {
        auto a = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point b;
        do { b = std::chrono::steady_clock::now(); } while(b == a);
        double d = std::chrono::duration<double, std::micro>(b - a).count();
        if(d < best) best = d;
    }
    return best;
}

// How long a 1 ms sleep really takes. Exposes the Windows 15.6 ms timer tick.
static double SleepGranularityMs(int requestMs, int iters)
{
    double sum = 0.0;
    for(int i = 0; i < iters; i++)
    {
        auto s = std::chrono::steady_clock::now();
#ifdef _WIN32
        Sleep(requestMs);
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(requestMs));
#endif
        sum += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s).count();
    }
    return sum / iters;
}

std::string CollectEnvInfo(const std::string& extra)
{
    std::ostringstream os;
    std::time_t now = std::time(nullptr);
    char tbuf[64] = {0};
    std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", std::localtime(&now));

    os << "================================================================================\n"
       << " DXPROF - yolo_multi_demo stage profile\n"
       << "================================================================================\n"
       << "date               : " << tbuf << "\n"
       << "os                 : " << OsInfo() << "\n"
       << "cpu                : " << CpuBrand() << "\n"
       << "cpu.hw_threads     : " << std::thread::hardware_concurrency() << "\n"
       << "ram.total_mb       : " << TotalRamMB() << "\n";

#if defined(_MSC_VER)
    os << "compiler           : MSVC " << _MSC_VER << "\n";
#elif defined(__clang__)
    os << "compiler           : clang " << __clang_major__ << "." << __clang_minor__ << "\n";
#elif defined(__GNUC__)
    os << "compiler           : gcc " << __GNUC__ << "." << __GNUC_MINOR__ << "\n";
#endif
#ifdef NDEBUG
    os << "build              : Release (NDEBUG)\n";
#else
    os << "build              : Debug\n";
#endif
#ifdef USE_VAAPI
    os << "gst.vaapi          : ON\n";
#else
    os << "gst.vaapi          : OFF\n";
#endif

    os << "opencv.version     : " << CV_VERSION << "\n"
       << "opencv.threads     : " << cv::getNumThreads() << " / cpus " << cv::getNumberOfCPUs() << "\n"
       << "opencv.optimized   : " << (cv::useOptimized() ? "yes" : "no") << "\n"
       << "opencv.avx2        : " << (cv::checkHardwareSupport(CV_CPU_AVX2) ? "yes" : "no") << "\n"
       << "opencv.sse4_2      : " << (cv::checkHardwareSupport(CV_CPU_SSE4_2) ? "yes" : "no") << "\n";

    os << "clock.granularity_us : " << std::fixed << std::setprecision(3) << ClockGranularityUs() << "\n";
    os << "sleep(1ms).actual_ms : " << std::fixed << std::setprecision(3) << SleepGranularityMs(1, 20) << "\n";
    os << "sleep(5ms).actual_ms : " << std::fixed << std::setprecision(3) << SleepGranularityMs(5, 10) << "\n";

    MemSnapshot m;
    if(ReadMemSnapshot(m))
    {
        os << "--------------------------------------------------------------------------------\n"
           << "[memory at profiler start]  (after preload buffers were allocated)\n"
           << "proc.rss_mb        : " << m.rss_mb << "\n"
           << "proc.commit_mb     : " << m.commit_mb << "\n"
           << "sys.avail_mb       : " << m.sys_avail_mb << " / " << m.sys_total_mb << "\n"
           << "fault.counter_type : " << (m.faults_are_major ? "major only (disk I/O)"
                                                             : "all faults (soft+hard)") << "\n";
    }

    if(!extra.empty()) os << extra;

    os << "--------------------------------------------------------------------------------\n"
       << "[OpenCV build information]\n"
       << cv::getBuildInformation() << "\n"
       << "================================================================================\n\n";
    return os.str();
}

// ---------------------------------------------------------------------------
// Aggregation
// ---------------------------------------------------------------------------
Summary Profiler::Reduce(const StatSnap& s)
{
    Summary r;
    r.count = s.count;
    if(s.count == 0) return r;
    r.total = s.sum;
    r.avg  = (double)s.sum / (double)s.count;
    r.minv = (s.minv == UINT64_MAX) ? 0 : s.minv;
    r.maxv = s.maxv;
    r.p50  = PercentileFromHist(s.hist, s.count, 0.50);
    r.p90  = PercentileFromHist(s.hist, s.count, 0.90);
    r.p99  = PercentileFromHist(s.hist, s.count, 0.99);
    return r;
}

StatSnap Profiler::Delta(const StatSnap& cur, const StatSnap& base)
{
    StatSnap d;
    d.count = cur.count - base.count;
    d.sum   = cur.sum   - base.sum;
    d.minv  = cur.minv;          // min/max stay cumulative, not per-interval
    d.maxv  = cur.maxv;
    for(int i = 0; i < kHistBins; i++) d.hist[i] = cur.hist[i] - base.hist[i];
    return d;
}

void Profiler::Snapshot(std::vector<std::vector<StatSnap>>& out) const
{
    for(int c = 0; c <= _numChannels && c < kMaxChannels; c++)
    {
        for(int s = 0; s < ST_COUNT; s++)
        {
            const Stat& st = _stats[c][s];
            StatSnap& d = out[c][s];
            d.count = st.count.load(std::memory_order_relaxed);
            d.sum   = st.sum.load(std::memory_order_relaxed);
            d.minv  = st.minv.load(std::memory_order_relaxed);
            d.maxv  = st.maxv.load(std::memory_order_relaxed);
            for(int b = 0; b < kHistBins; b++)
                d.hist[b] = st.hist[b].load(std::memory_order_relaxed);
        }
    }
}

// Human-readable duration
static std::string FmtUs(double us)
{
    std::ostringstream o;
    if(us < 0.0)           o << "-";
    else if(us == 0.0)     o << "0us";
    else if(us < 1000.0)   o << std::fixed << std::setprecision(1) << us << "us";
    else if(us < 1000000.0)o << std::fixed << std::setprecision(2) << (us / 1000.0) << "ms";
    else                   o << std::fixed << std::setprecision(2) << (us / 1000000.0) << "s";
    return o.str();
}

static std::string Pad(const std::string& s, int w, bool left = false)
{
    if((int)s.size() >= w) return s;
    std::string sp(w - s.size(), ' ');
    return left ? (s + sp) : (sp + s);
}

void Profiler::WriteReport(std::ostream& os, const char* title,
                           const std::vector<std::vector<StatSnap>>& snap,
                           const std::vector<std::vector<StatSnap>>* base,
                           double wallSec, bool csv)
{
    os << "\n================================================================================\n"
       << " " << title << "   (" << std::fixed << std::setprecision(2) << wallSec << " s)\n"
       << "================================================================================\n";

    // A fault rate that stays high means the working set does not fit in
    // physical memory.
    {
        MemSnapshot m;
        if(ReadMemSnapshot(m))
        {
            os << "[memory] rss " << m.rss_mb << " MB"
               << " | commit " << m.commit_mb << " MB"
               << " | peak_rss " << m.peak_rss_mb << " MB"
               << " | sys_avail " << m.sys_avail_mb << "/" << m.sys_total_mb << " MB";
            if(_memPrev.valid && m.faults >= _memPrev.faults)
            {
                uint64_t d = m.faults - _memPrev.faults;
                double dt = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - _memPrevT).count();
                os << " | " << (m.faults_are_major ? "major_faults " : "page_faults ")
                   << "+" << d;
                if(dt > 0)
                    os << " (" << std::fixed << std::setprecision(0) << (d / dt) << "/s)";
            }
            os << "\n";
            _memPrev  = m;
            _memPrevT = std::chrono::steady_clock::now();
        }
    }

    // Ratio against the baseline; 1.00 means the same speed as at startup.
    {
        double us = CpuCanaryUs();
        if(_canaryBase <= 0.0) _canaryBase = us;
        os << "[cpu] canary " << std::fixed << std::setprecision(1) << us << "us"
           << "  (baseline " << std::setprecision(1) << _canaryBase << "us, "
           << std::setprecision(2) << (us / _canaryBase) << "x)";
        // The canary competes with the demo for CPU, so read the change, not
        // the absolute value. The baseline was taken under the same load.
        if(us > _canaryBase * 1.20) os << "  <-- CPU slower (throttling or contention?)";
        os << "\n";
    }

    // Injected sampler, e.g. NPU temperature and clock.
    {
        PeriodicSampler fn;
        {
            std::lock_guard<std::mutex> lk(_samplerMutex);
            fn = _sampler;
        }
        if(fn)
        {
            std::string line;
            try { line = fn(); } catch(...) {}
            if(!line.empty()) os << "[device]" << line << "\n";
        }
    }

    // Stage table, summed over all channels.
    os << Pad("stage", 20, true) << Pad("count", 9) << Pad("calls/s", 10)
       << Pad("avg", 11) << Pad("p50", 11) << Pad("p90", 11) << Pad("p99", 11)
       << Pad("max*", 11) << Pad("cpu-ms/s", 10) << "\n";
    os << std::string(104, '-') << "\n";

    for(int s = 0; s < ST_COUNT; s++)
    {
        // Merge every channel's histogram for this stage.
        StatSnap agg;
        for(int c = 0; c <= _numChannels && c < kMaxChannels; c++)
        {
            StatSnap cur = base ? Delta(snap[c][s], (*base)[c][s]) : snap[c][s];
            if(cur.count == 0) continue;
            agg.count += cur.count;
            agg.sum   += cur.sum;
            agg.minv   = std::min(agg.minv, cur.minv);
            agg.maxv   = std::max(agg.maxv, cur.maxv);
            for(int b = 0; b < kHistBins; b++) agg.hist[b] += cur.hist[b];
        }
        if(agg.count == 0) continue;
        Summary r = Reduce(agg);
        std::ostringstream cps, cms;
        cps << std::fixed << std::setprecision(1) << (wallSec > 0 ? r.count / wallSec : 0.0);
        cms << std::fixed << std::setprecision(1) << (wallSec > 0 ? (r.total / 1000.0) / wallSec : 0.0);
        os << Pad(StageName(s), 20, true)
           << Pad(std::to_string(r.count), 9)
           << Pad(cps.str(), 10)
           << Pad(FmtUs(r.avg), 11)
           << Pad(FmtUs(r.p50), 11)
           << Pad(FmtUs(r.p90), 11)
           << Pad(FmtUs(r.p99), 11)
           << Pad(FmtUs((double)r.maxv), 11)
           << Pad(cms.str(), 10)
           << "\n";
    }
    os << "  * max is cumulative since start\n";
    os << "  * cpu-ms/s is the total time this stage consumed per second,\n"
       << "    summed over all channels; 30ch x 33ms lands near 1000\n";

    // Per-channel averages for the key stages.
    if(_cfg.per_channel && _numChannels > 0)
    {
        static const int kCols[] = { ST_WORKER_LOOP, ST_WORKER_BUSY, ST_GET_INPUT, ST_RUN_ASYNC,
                                     ST_GET_OUTPUT, ST_FRAME_SWAP, ST_WORKER_SLEEP_ACT,
                                     ST_POSTPROC, ST_POSTPROC_GAP, ST_NPU_INFER, ST_NPU_LATENCY };
        const int nCols = (int)(sizeof(kCols) / sizeof(kCols[0]));
        os << "\n[per-channel avg]\n" << Pad("ch", 4);
        for(int i = 0; i < nCols; i++)
        {
            std::string n = StageName(kCols[i]);
            size_t dot = n.find('.');
            if(dot != std::string::npos) n = n.substr(dot + 1);
            os << Pad(n, 12);
        }
        os << Pad("fps", 8) << "\n" << std::string(4 + nCols * 12 + 8, '-') << "\n";

        for(int c = 1; c <= _numChannels && c < kMaxChannels; c++)
        {
            StatSnap loop = base ? Delta(snap[c][ST_WORKER_LOOP], (*base)[c][ST_WORKER_LOOP])
                                 : snap[c][ST_WORKER_LOOP];
            StatSnap gapS = base ? Delta(snap[c][ST_POSTPROC_GAP], (*base)[c][ST_POSTPROC_GAP])
                                 : snap[c][ST_POSTPROC_GAP];
            if(loop.count == 0 && gapS.count == 0) continue;
            os << Pad(std::to_string(c), 4);
            for(int i = 0; i < nCols; i++)
            {
                StatSnap cur = base ? Delta(snap[c][kCols[i]], (*base)[c][kCols[i]]) : snap[c][kCols[i]];
                os << Pad(cur.count ? FmtUs((double)cur.sum / cur.count) : std::string("-"), 12);
            }
            std::ostringstream f;
            f << std::fixed << std::setprecision(1) << (wallSec > 0 ? gapS.count / wallSec : 0.0);
            os << Pad(f.str(), 8) << "\n";
        }
    }

    // CSV block, for scripted comparison.
    if(csv)
    {
        os << "\n#CSV,channel,stage,count,avg_us,min_us,p50_us,p90_us,p99_us,max_us,total_us\n";
        for(int c = 0; c <= _numChannels && c < kMaxChannels; c++)
        {
            for(int s = 0; s < ST_COUNT; s++)
            {
                StatSnap cur = base ? Delta(snap[c][s], (*base)[c][s]) : snap[c][s];
                if(cur.count == 0) continue;
                Summary r = Reduce(cur);
                os << "CSV," << c << "," << StageName(s) << "," << r.count << ","
                   << std::fixed << std::setprecision(1) << r.avg << ","
                   << r.minv << "," << r.p50 << "," << r.p90 << "," << r.p99 << ","
                   << r.maxv << "," << r.total << "\n";
            }
        }
    }
    os.flush();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void Profiler::Init(const Config& cfg, const std::string& envInfo)
{
    _cfg = cfg;
    if(!_cfg.enabled) return;

    _ofs.open(_cfg.path, std::ios::out | std::ios::trunc);
    if(!_ofs.is_open())
    {
        std::cerr << "[DXPROF] cannot open log file: " << _cfg.path << " (profiling disabled)" << std::endl;
        _cfg.enabled = false;
        return;
    }
    _envInfo = envInfo;
    _t0 = std::chrono::steady_clock::now();   // in case we exit during warmup
    _ofs << _envInfo;
    _ofs.flush();

    std::cout << "[DXPROF] profiling enabled -> " << _cfg.path
              << " (period " << _cfg.period_ms << " ms, warmup " << _cfg.warmup_ms << " ms)" << std::endl;

    _running.store(true);
    _thread = std::thread(&Profiler::DumpLoop, this);
}

void Profiler::DumpLoop()
{
    // Warmup: keep startup and cache-warming out of the statistics.
    if(_cfg.warmup_ms > 0)
    {
        std::unique_lock<std::mutex> lk(_cvMutex);
        _cv.wait_for(lk, std::chrono::milliseconds(_cfg.warmup_ms), [this]{ return !_running.load(); });
    }
    if(!_running.load()) return;

    // Discard whatever accumulated during warmup.
    for(int c = 0; c < kMaxChannels; c++)
        for(int s = 0; s < ST_COUNT; s++)
        {
            _stats[c][s].count.store(0, std::memory_order_relaxed);
            _stats[c][s].sum.store(0, std::memory_order_relaxed);
            _stats[c][s].minv.store(UINT64_MAX, std::memory_order_relaxed);
            _stats[c][s].maxv.store(0, std::memory_order_relaxed);
            for(int b = 0; b < kHistBins; b++) _stats[c][s].hist[b].store(0, std::memory_order_relaxed);
        }

    _t0 = std::chrono::steady_clock::now();
    // Seed the baseline so interval #1 already reports a fault delta.
    ReadMemSnapshot(_memPrev);
    _memPrevT = _t0;
    _canaryBase = CpuCanaryUs();
    _measured.store(true);
    Active().store(true, std::memory_order_relaxed);
    _ofs << "[DXPROF] warmup " << _cfg.warmup_ms << " ms done, measurement started.\n";
    _ofs.flush();

    const int nSlots = std::min(_numChannels + 1, kMaxChannels);
    std::vector<std::vector<StatSnap>> cur(nSlots, std::vector<StatSnap>(ST_COUNT));
    std::vector<std::vector<StatSnap>> prev(nSlots, std::vector<StatSnap>(ST_COUNT));
    auto lastT = _t0;

    while(_running.load())
    {
        {
            std::unique_lock<std::mutex> lk(_cvMutex);
            _cv.wait_for(lk, std::chrono::milliseconds(_cfg.period_ms), [this]{ return !_running.load(); });
        }
        if(!_running.load()) break;

        auto now = std::chrono::steady_clock::now();
        double wall = std::chrono::duration<double>(now - lastT).count();
        lastT = now;
        Snapshot(cur);

        std::ostringstream title;
        title << "INTERVAL #" << (++_intervalNo)
              << "   t = " << std::fixed << std::setprecision(1)
              << std::chrono::duration<double>(now - _t0).count() << " s";
        WriteReport(_ofs, title.str().c_str(), cur, &prev, wall, false);
        prev = cur;
    }
}

void Profiler::Shutdown()
{
    if(!_cfg.enabled) return;
    _cfg.enabled = false;

    Active().store(false, std::memory_order_relaxed);
    _running.store(false);
    _cv.notify_all();
    if(_thread.joinable()) _thread.join();

    if(_ofs.is_open())
    {
        double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - _t0).count();
        if(!_measured.load())
        {
            // Exited before warmup finished, so nothing was measured.
            _ofs << "\n[DXPROF] exited before the warmup of " << _cfg.warmup_ms
                 << " ms finished, so no samples were collected (ran "
                 << std::fixed << std::setprecision(2) << wall << " s).\n"
                 << "[DXPROF] lower --profile_warmup or run for longer.\n";
            _ofs.close();
            std::cerr << "[DXPROF] exited during warmup; statistics are empty: " << _cfg.path << std::endl;
            return;
        }
        std::vector<std::vector<StatSnap>> cur(std::min(_numChannels + 1, kMaxChannels),
                                               std::vector<StatSnap>(ST_COUNT));
        Snapshot(cur);
        WriteReport(_ofs, "CUMULATIVE SUMMARY (everything after warmup)", cur, nullptr, wall, true);
        _ofs << "\n[DXPROF] end of log\n";
        _ofs.close();
        std::cout << "[DXPROF] profile log written: " << _cfg.path << std::endl;
    }
}

} // namespace dxprof
