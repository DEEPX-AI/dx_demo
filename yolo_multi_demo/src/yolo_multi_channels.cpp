#include <dxrt/inference_option.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <ctime>
#include <climits>  // INT_MAX, UINT_MAX, LLONG_MAX 등을 위해 추가
#include <sys/types.h>
#include <sys/stat.h>
#ifdef __linux
#include <sys/mman.h>
#include <unistd.h>
#endif

#include <string.h>
#include <errno.h>
#include <csignal>
#include <atomic>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <map>
#include <vector>
#include <cxxopts.hpp>
#include <opencv2/opencv.hpp>
#ifdef HAVE_OPENCV_FREETYPE
#include <opencv2/freetype.hpp>
#endif
#ifdef HAVE_FREETYPE_DIRECT
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

#include "display.h"

#include "od.h"
#include "profiler.h"

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include <utils/common_util.hpp>
#include <dxrt/device_info_status.h>

#ifdef _WIN32
// timeBeginPeriod / timeGetDevCaps.
// CMakeLists 가 WIN32_LEAN_AND_MEAN 을 정의하므로 <Windows.h> 가
// <mmsystem.h> 를 끌어오지 않는다. 직접 포함한다.
// (<Windows.h> 자체는 common_util.hpp 경유로 이미 들어와 있다.)
#include <timeapi.h>
// TIMERR_NOERROR 는 SDK 버전에 따라 mmsyscom.h / mmsystem.h 중 어디서
// 정의되는지가 달라 timeapi.h 만으로 안 잡히는 경우가 있다. 값은 0 으로 고정.
#ifndef TIMERR_NOERROR
#define TIMERR_NOERROR 0
#endif
#endif

#define DISPLAY_WINDOW_NAME "Object Detection"
#define EXPAND_WINDOW_NAME "Expand Window"
#define EXPAND_WINDOW 1

/* Tuning point */
#define INPUT_CAPTURE_PERIOD_MS 33

static bool g_full_screan = false;

/**
 * @brief AppConfig Definition
 *      application_type : 0 (single), 1 (multi)
 */
struct AppConfig
{
    int application_type;
    std::string model_path;
    std::string model_name;
    std::vector<std::pair<std::string, std::string>> video_sources;
    std::vector<int> pre_saved_frame_count;
    std::string display_label;

    int input_capture_period_ms;

    int board_width;
    int board_height;

    int is_show_fps;
    int is_fill_blank;
    int is_expand_mode;
    int is_fullsize_mode;

    int grid_cols;   // 0 = auto (ceil(sqrt(N)))
    int grid_rows;   // 0 = auto
    int num_devices; // for sidebar display
    float sidebar_font_scale; // 1.0 = default, <1.0 smaller, >1.0 larger
    float fps_value_font_scale; // 0.5 = default
    int display_fps; // max FPS for imshow rendering (0 = unlimited)
    int console_fps_period_ms; // periodic FPS log to terminal (0 = disabled)

    // --- stage profiler ("profile" block in json) ---
    int         profile_enabled;
    std::string profile_path;
    int         profile_period_ms;
    int         profile_warmup_ms;
    int         profile_per_channel;
};

// pre/post parameter table
extern YoloParam yolov5s_320, yolov5s_512, yolov5s_640,
yolov7_512, yolov7_640, yolov8_640, yolox_s_512, yolov5s_face_640, yolov3_512, yolov4_416,
yolov9_640, yolov5s_512_ppu, yolov5s_640_ppu, scrfd_face_640_ppu;
std::vector<YoloParam> yoloParams = {
    yolov5s_320,
    yolov5s_512,
    yolov5s_640,
    yolov7_512,
    yolov7_640,
    yolov8_640,
    yolox_s_512,
    yolov5s_face_640,
    yolov3_512,
    yolov4_416,
    yolov9_640,
    yolov5s_512_ppu,
    yolov5s_640_ppu,
    scrfd_face_640_ppu
};

const char* usage =
"yolo demo\n"
"  -c, --config        use config json file for run application\n"
"                      e.g. sudo yolo_multi -c _multi_od_.json -a \n"
"      --window_size    FPS by average over the last {window_size} seconds (default: 60)\n"
"                      e.g. sudo yolo_multi -c _multi_od_.json --window_size 60\n"
"      --fps_log_period  print FPS to terminal every N ms (0 = off,\n"
"                      default: 1000 on Windows, 0 on others)\n"
"                      e.g. yolo_multi -c _multi_od_.json --fps_log_period 500\n"
"      --profile [path]  write a per-stage timing log (default name if omitted)\n"
"                      e.g. yolo_multi -c _multi_od_.json --profile win.log\n"
"      --profile_period  profiler dump interval in ms (default: 5000)\n"
"      --profile_warmup  ignore the first N ms before measuring (default: 3000)\n"
"  -h, --help          show help\n"
;

void help()
{
    std::cout << usage << std::endl;
}

int ApplicationJsonParser(std::string configPath, AppConfig* dst)
{
    std::ifstream ifs(configPath);
    DXRT_ASSERT(ifs.is_open(), "can't open " + configPath );
    std::string json((std::istreambuf_iterator<char>(ifs)), (std::istreambuf_iterator<char>()));
    rapidjson::Document doc;
    doc.Parse(json.c_str());
    rapidjson::StringBuffer buffer;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);
    std::cout << buffer.GetString() << std::endl;
    if(doc.IsObject())
    {
        DXRT_ASSERT(doc.HasMember("usage"), "ERR. usage argument not placed");
        DXRT_ASSERT(doc["usage"].IsString(), "ERR. usage argument must be str");
        dst->application_type = std::string(doc["usage"].GetString()) == "multi" ? 1 : 0;

        DXRT_ASSERT(doc.HasMember("model_path"), "ERR. model_path argument not placed");
        DXRT_ASSERT(doc["model_path"].IsString(), "ERR. model_path argument must be str");
        dst->model_path = doc["model_path"].GetString();

        DXRT_ASSERT(doc.HasMember("model_name"), "ERR. model_name argument not placed");
        DXRT_ASSERT(doc["model_name"].IsString(), "ERR. model_name argument must be str");
        dst->model_name = doc["model_name"].GetString();

        DXRT_ASSERT(doc.HasMember("display_config"), "ERR. display_config argument not placed");
        DXRT_ASSERT(doc["display_config"].IsObject(), "ERR. display_config must be json object");

        const rapidjson::Value& displayConfig = doc["display_config"];

        DXRT_ASSERT(displayConfig.HasMember("display_label"), "ERR. display_label argument not placed");
        DXRT_ASSERT(displayConfig["display_label"].IsString(), "ERR. display_label must be str");
        dst->display_label = displayConfig["display_label"].GetString();

        if(!displayConfig.HasMember("output_width"))
        {
            g_full_screan = true;
#ifdef __linux__
            std::ifstream graphics_info_file("/sys/class/graphics/fb0/virtual_size");
            if(!graphics_info_file)
            {
                std::cout << "Failed to open framebuffer info, It will be set FHD size" << std::endl;
                dst->board_width = 1920;
                dst->board_height = 1080;
            }
            else
            {
                int graphics_info_w, graphics_info_h;
                char comma;
                graphics_info_file >> graphics_info_w >> comma >> graphics_info_h;
                dst->board_width = graphics_info_w;
                dst->board_height = graphics_info_h;
            }
#elif _WIN32
            dst->board_width = GetSystemMetrics(SM_CXSCREEN);
            dst->board_height = GetSystemMetrics(SM_CYSCREEN);
#endif
        }
        else
        {
            DXRT_ASSERT(displayConfig["output_width"].IsInt(), "ERR. output_width must be integer");
            dst->board_width = displayConfig["output_width"].GetInt();

            DXRT_ASSERT(displayConfig.HasMember("output_height"), "ERR. output_height argument not placed");
            DXRT_ASSERT(displayConfig["output_height"].IsInt(), "ERR. output_height must be integer");
            dst->board_height = displayConfig["output_height"].GetInt();
        }
        if(displayConfig.HasMember("capture_period"))
        {
            DXRT_ASSERT(displayConfig["capture_period"].IsInt(), "ERR. capture_period must be integer");
            dst->input_capture_period_ms = displayConfig["capture_period"].GetInt();
        }
        else
        {
            dst->input_capture_period_ms = INPUT_CAPTURE_PERIOD_MS;
        }

        if(displayConfig.HasMember("show_fps"))
        {
            DXRT_ASSERT(displayConfig["show_fps"].IsBool(), "ERR. show_fps must be boolean");
            dst->is_show_fps = displayConfig["show_fps"].GetBool();
        }
        else
        {
            dst->is_show_fps = true;
        }

        if(displayConfig.HasMember("fill_blank"))
        {
            DXRT_ASSERT(displayConfig["fill_blank"].IsBool(), "ERR. fill_blank must be boolean");
            dst->is_fill_blank = displayConfig["fill_blank"].GetBool();
        }
        else
        {
            dst->is_fill_blank = true;
        }

        if(displayConfig.HasMember("expand_mode"))
        {
            DXRT_ASSERT(displayConfig["expand_mode"].IsBool(), "ERR. expand_mode must be boolean");
            dst->is_expand_mode = displayConfig["expand_mode"].GetBool();
        }
        else
        {
            dst->is_expand_mode = false;
        }
        if(displayConfig.HasMember("dynamic_window_mode"))
        {
            DXRT_ASSERT(displayConfig["dynamic_window_mode"].IsBool(), "ERR. dynamic_window_mode must be boolean");
            dst->is_fullsize_mode = !displayConfig["dynamic_window_mode"].GetBool();
        }
        else
        {
            dst->is_fullsize_mode = true;
        }

        dst->grid_cols = 0;
        dst->grid_rows = 0;
        if(displayConfig.HasMember("grid_cols"))
        {
            DXRT_ASSERT(displayConfig["grid_cols"].IsInt(), "ERR. grid_cols must be integer");
            dst->grid_cols = displayConfig["grid_cols"].GetInt();
        }
        if(displayConfig.HasMember("grid_rows"))
        {
            DXRT_ASSERT(displayConfig["grid_rows"].IsInt(), "ERR. grid_rows must be integer");
            dst->grid_rows = displayConfig["grid_rows"].GetInt();
        }

        dst->num_devices = 1;
        if(doc.HasMember("num_devices"))
        {
            DXRT_ASSERT(doc["num_devices"].IsInt(), "ERR. num_devices must be integer");
            dst->num_devices = doc["num_devices"].GetInt();
        }

        dst->sidebar_font_scale = 1.0f;
        if(displayConfig.HasMember("sidebar_font_scale"))
        {
            DXRT_ASSERT(displayConfig["sidebar_font_scale"].IsNumber(), "ERR. sidebar_font_scale must be number");
            dst->sidebar_font_scale = (float)displayConfig["sidebar_font_scale"].GetDouble();
        }

        dst->fps_value_font_scale = 0.5f;

        dst->display_fps = 30;
        if(displayConfig.HasMember("display_fps"))
        {
            DXRT_ASSERT(displayConfig["display_fps"].IsInt(), "ERR. display_fps must be integer");
            dst->display_fps = displayConfig["display_fps"].GetInt();
        }
        if(displayConfig.HasMember("fps_value_font_scale"))
        {
            DXRT_ASSERT(displayConfig["fps_value_font_scale"].IsNumber(), "ERR. fps_value_font_scale must be number");
            dst->fps_value_font_scale = (float)displayConfig["fps_value_font_scale"].GetDouble();
        }

        // Windows: 화면에 헤더 HUD(FPS 카드)를 그리지 않으므로 콘솔 FPS 로그를
        //          기본 활성화한다. 다른 OS는 기본 비활성(HUD로 확인 가능).
#ifdef _WIN32
        dst->console_fps_period_ms = 1000;
#else
        dst->console_fps_period_ms = 0;
#endif
        if(displayConfig.HasMember("console_fps_period_ms"))
        {
            DXRT_ASSERT(displayConfig["console_fps_period_ms"].IsInt(), "ERR. console_fps_period_ms must be integer");
            dst->console_fps_period_ms = displayConfig["console_fps_period_ms"].GetInt();
        }

        // --- stage profiler 설정 ---
        // "profile": { "enabled": true, "path": "...", "period_ms": 5000,
        //              "warmup_ms": 3000, "per_channel": true }
        dst->profile_enabled     = 0;
        dst->profile_path        = "";
        dst->profile_period_ms   = 5000;
        dst->profile_warmup_ms   = 3000;
        dst->profile_per_channel = 1;
        if(doc.HasMember("profile") && doc["profile"].IsObject())
        {
            const rapidjson::Value& pf = doc["profile"];
            if(pf.HasMember("enabled"))
                dst->profile_enabled = pf["enabled"].IsBool() ? (pf["enabled"].GetBool() ? 1 : 0)
                                                              : pf["enabled"].GetInt();
            if(pf.HasMember("path"))
            {
                DXRT_ASSERT(pf["path"].IsString(), "ERR. profile.path must be string");
                dst->profile_path = pf["path"].GetString();
            }
            if(pf.HasMember("period_ms"))
            {
                DXRT_ASSERT(pf["period_ms"].IsInt(), "ERR. profile.period_ms must be integer");
                dst->profile_period_ms = pf["period_ms"].GetInt();
            }
            if(pf.HasMember("warmup_ms"))
            {
                DXRT_ASSERT(pf["warmup_ms"].IsInt(), "ERR. profile.warmup_ms must be integer");
                dst->profile_warmup_ms = pf["warmup_ms"].GetInt();
            }
            if(pf.HasMember("per_channel"))
                dst->profile_per_channel = pf["per_channel"].IsBool() ? (pf["per_channel"].GetBool() ? 1 : 0)
                                                                      : pf["per_channel"].GetInt();
        }

        DXRT_ASSERT(doc.HasMember("video_sources"), "ERR. video_sources argument not placed");
        DXRT_ASSERT(doc["video_sources"].IsArray(), "ERR. video_sources must be array");
        const rapidjson::Value& videoSources = doc["video_sources"];
        for(rapidjson::SizeType i = 0; i < videoSources.Size(); i++){
            const rapidjson::Value& videoSource = videoSources[i];
            std::pair<std::string, std::string> videoSourceInfo(std::pair<std::string, std::string>(videoSource[0].GetString(), videoSource[1].GetString()));
#if __riscv
            if(std::string(videoSource[1].GetString()) == "isp"){
                dst->video_sources.clear();
                dst->pre_saved_frame_count.clear();
                dst->video_sources.emplace_back(videoSourceInfo);
                dst->pre_saved_frame_count.emplace_back(-1);
                return 1;
            }
#endif
            if(std::string(videoSource[1].GetString()) == "offline")
            {
                if(videoSource.Size() == 2)
                {
                    dst->pre_saved_frame_count.emplace_back(0);
                }
                else if(videoSource.Size() == 3)
                {
                    dst->pre_saved_frame_count.emplace_back(videoSource[2].GetInt());
                }
            }else{
                dst->pre_saved_frame_count.emplace_back(-1);
            }
            dst->video_sources.emplace_back(videoSourceInfo);
        }
    }else{
        return -1;
    }
    return 1;
}

YoloParam getYoloParameter(std::string model_name){
    if(model_name == "yolov5s_320")
        return yolov5s_320;
    else if(model_name == "yolov5s_512")
        return yolov5s_512;
    else if(model_name == "yolov5s_640")
        return yolov5s_640;
    else if(model_name == "yolox_s_512")
        return yolox_s_512;
    else if(model_name == "yolov7_640")
        return yolov7_640;
    else if(model_name == "yolov7_512")
        return yolov7_512;
    else if(model_name == "yolov8_640")
        return yolov8_640;
    else if(model_name == "yolov5s_face_640")
        return yolov5s_face_640;
    else if(model_name == "yolov3_512")
        return yolov3_512;
    else if(model_name == "yolov4_416")
        return yolov4_416;
    else if(model_name == "yolov9_640")
        return yolov9_640;
    else if(model_name == "yolov5s_512_ppu")
        return yolov5s_512_ppu;
    else if(model_name == "yolov5s_640_ppu")
        return yolov5s_640_ppu;
    else if(model_name == "scrfd_face_640_ppu")
        return scrfd_face_640_ppu;
    return yolov5s_512;
}
YoloParam yoloParam;

// --- CPU 로드 측정 (Linux) ---
#ifdef __linux
static float getCpuLoad()
{
    static uint64_t lastIdle = 0, lastTotal = 0;
    std::ifstream stat("/proc/stat");
    if(!stat.is_open()) return 0.f;
    std::string cpu;
    uint64_t user, nice, system, idle, iowait = 0, irq = 0, softirq = 0;
    stat >> cpu >> user >> nice >> system >> idle >> iowait >> irq >> softirq;
    uint64_t total = user + nice + system + idle + iowait + irq + softirq;
    uint64_t deltaIdle  = idle  - lastIdle;
    uint64_t deltaTotal = total - lastTotal;
    lastIdle  = idle;
    lastTotal = total;
    if(deltaTotal == 0) return 0.f;
    return (1.0f - (float)deltaIdle / deltaTotal) * 100.f;
}
#else
static float getCpuLoad() { return 0.f; }
#endif

// --- NPU average temperature ---
static float getNpuAvgTemp(int numDevices)
{
    if(numDevices <= 0) return 0.f;
    int sum = 0, count = 0;
    for(int i = 0; i < numDevices; i++) {
        try {
            auto status = dxrt::DeviceStatus::GetCurrentStatus(i);
            int temp = status.Temperature(0);
            if(temp > 0) { sum += temp; count++; }
        } catch(...) {}
    }
    return (count > 0) ? (float)sum / count : 0.f;
}

// Helper for grid layout (unused after Task 3; may be used in future)
static int devideBoard(int numImages)
{
    return (int)ceil(sqrt(numImages));
}

// Suppress unused function warnings for CPU/NPU helpers removed by Task 3
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
static void suppressUnusedTaskThreeHelpers() {
    (void)getCpuLoad;
    (void)getNpuAvgTemp;
    (void)devideBoard;
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

// --- EXIT 요청 플래그 (마우스/시그널 공용) ---
static std::atomic<bool> g_exitRequested{false};

static void onExitSignal(int) { g_exitRequested = true; }

// --- DEEPX 디바이스 표시명 목록 ---
// dxrt-cli -s 의 "Device N: <variant>" 정보를 기반으로 사용자 친화적인 이름을 만든다.
// 예) M.2 보드의 M1 칩 → "DX-M1", H1 보드 → "DX-H1 Quattro"
//     동일 이름의 디바이스가 여러 개면 "DX-M1 * 3" 형태로 묶어서 반환.
static std::vector<std::string> getDeepxDeviceNames(int numDevices)
{
    std::vector<std::string> order;
    std::map<std::string, int> counts;
    for(int i = 0; i < numDevices; i++) {
        std::string name;
        try {
            auto status = dxrt::DeviceStatus::GetCurrentStatus(i);
            std::string board = status.BoardTypeStr();
            std::string variant = status.DeviceVariantStr();
            if(board == "H1") name = "DX-H1 Quattro";
            else if(!variant.empty()) name = "DX-" + variant;
            else name = "DEEPX DEVICE";
        } catch(...) {
            name = "DEEPX DEVICE";
        }
        if(counts.find(name) == counts.end()) order.push_back(name);
        counts[name]++;
    }
    // H1 Quattro 보드는 1장 = 4개의 칩으로 카운트되므로 4로 나눈다.
    auto it = counts.find("DX-H1 Quattro");
    if(it != counts.end() && it->second >= 4) it->second /= 4;

    std::vector<std::string> result;
    for(const auto& n : order) {
        int c = counts[n];
        if(c <= 1) result.push_back(n);
        else result.push_back(n + " * " + std::to_string(c));
    }
    return result;
}

static void onMouseCallback(int event, int /*x*/, int /*y*/, int /*flags*/, void* /*userdata*/)
{
    if(event == cv::EVENT_LBUTTONDOWN)
    {
        // No clickable overlay controls in the unified header UI.
    }
}

// --- HeaderUI: weighted Montserrat font loader ---
enum class HeaderFontWeight { Regular = 0, SemiBold, Bold, ExtraBold, Count };

static std::string fontFileName(HeaderFontWeight weight)
{
    switch(weight) {
        case HeaderFontWeight::Regular:   return "Montserrat-Regular.ttf";
        case HeaderFontWeight::SemiBold:  return "Montserrat-SemiBold.ttf";
        case HeaderFontWeight::Bold:      return "Montserrat-Bold.ttf";
        case HeaderFontWeight::ExtraBold: return "Montserrat-ExtraBold.ttf";
        default: return "Montserrat-Regular.ttf";
    }
}

static std::vector<std::string> fontCandidates(HeaderFontWeight weight)
{
    std::string fileName = fontFileName(weight);
    std::vector<std::string> candidates;

#ifdef PROJECT_ROOT_DIR
    candidates.push_back(std::string(PROJECT_ROOT_DIR) + "/sample/fonts/" + fileName);
#endif
    candidates.push_back("./sample/fonts/" + fileName);
    candidates.push_back("../sample/fonts/" + fileName);

    // Fallback to system DejaVu fonts
    candidates.push_back("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf");
    candidates.push_back("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");

    return candidates;
}

#ifdef HAVE_OPENCV_FREETYPE
static cv::Ptr<cv::freetype::FreeType2> g_headerFonts[static_cast<int>(HeaderFontWeight::Count)];
static bool g_headerFonts_initialized[static_cast<int>(HeaderFontWeight::Count)] = { false, false, false, false };

static cv::Ptr<cv::freetype::FreeType2> getHeaderFont(HeaderFontWeight weight)
{
    int idx = static_cast<int>(weight);
    if(!g_headerFonts_initialized[idx])
    {
        g_headerFonts_initialized[idx] = true;
        auto candidates = fontCandidates(weight);
        for(const auto& path : candidates)
        {
            try {
                auto ft = cv::freetype::createFreeType2();
                ft->loadFontData(path, 0);
                g_headerFonts[idx] = ft;
                std::cout << "[HeaderUI] Loaded font: " << path << std::endl;
                break;
            } catch(...) {
                // Try next candidate
            }
        }
    }
    return g_headerFonts[idx];
}
#endif

#ifdef HAVE_FREETYPE_DIRECT
static FT_Library directFreeTypeLibrary()
{
    static FT_Library library = nullptr;
    static bool initialized = false;

    if(!initialized) {
        initialized = true;
        if(FT_Init_FreeType(&library) != 0) {
            library = nullptr;
        }
    }

    return library;
}

static FT_Face getDirectHeaderFace(HeaderFontWeight weight)
{
    static FT_Face faces[static_cast<int>(HeaderFontWeight::Count)] = { nullptr, nullptr, nullptr, nullptr };
    static bool initialized[static_cast<int>(HeaderFontWeight::Count)] = { false, false, false, false };

    int idx = static_cast<int>(weight);
    if(!initialized[idx]) {
        initialized[idx] = true;
        FT_Library library = directFreeTypeLibrary();
        if(library != nullptr) {
            auto candidates = fontCandidates(weight);
            for(const auto& path : candidates) {
                FT_Face face = nullptr;
                if(FT_New_Face(library, path.c_str(), 0, &face) == 0) {
                    faces[idx] = face;
                    std::cout << "[HeaderUI] Loaded font with FreeType fallback: " << path << std::endl;
                    break;
                }
            }
        }
    }

    return faces[idx];
}

static cv::Size headerTextSizeWithFreeType(const std::string& text, int fontH, HeaderFontWeight weight)
{
    FT_Face face = getDirectHeaderFace(weight);
    if(face == nullptr || fontH <= 0) return cv::Size(0, 0);
    if(FT_Set_Pixel_Sizes(face, 0, fontH) != 0) return cv::Size(0, 0);

    FT_Pos penX = 0;
    FT_UInt prevGlyph = 0;
    int maxRight = 0;
    int maxTop = 0;
    int maxBottom = 0;
    bool useKerning = FT_HAS_KERNING(face);

    for(unsigned char ch : text) {
        FT_UInt glyphIndex = FT_Get_Char_Index(face, ch);
        if(useKerning && prevGlyph != 0 && glyphIndex != 0) {
            FT_Vector delta;
            if(FT_Get_Kerning(face, prevGlyph, glyphIndex, FT_KERNING_DEFAULT, &delta) == 0) {
                penX += delta.x;
            }
        }
        if(FT_Load_Glyph(face, glyphIndex, FT_LOAD_DEFAULT) != 0) {
            continue;
        }

        FT_GlyphSlot glyph = face->glyph;
        int glyphLeft = (int)((penX + glyph->metrics.horiBearingX) >> 6);
        int glyphRight = (int)((penX + glyph->metrics.horiBearingX + glyph->metrics.width + 63) >> 6);
        int glyphTop = (int)(glyph->metrics.horiBearingY >> 6);
        int glyphBottom = (int)((glyph->metrics.height - glyph->metrics.horiBearingY + 63) >> 6);

        maxRight = std::max(maxRight, std::max(glyphRight, glyphLeft));
        maxTop = std::max(maxTop, glyphTop);
        maxBottom = std::max(maxBottom, glyphBottom);
        penX += glyph->advance.x;
        prevGlyph = glyphIndex;
    }

    int advanceWidth = (int)((penX + 63) >> 6);
    int width = std::max(maxRight, advanceWidth);
    int height = std::max(1, maxTop + maxBottom);
    return cv::Size(std::max(1, width), height);
}

static bool drawHeaderTextWithFreeType(cv::Mat& img, const std::string& text, cv::Point org,
                                       int fontH, HeaderFontWeight weight,
                                       const cv::Scalar& color, int thickness)
{
    (void)thickness;
    FT_Face face = getDirectHeaderFace(weight);
    if(face == nullptr || fontH <= 0 || img.empty() || img.channels() != 3) return false;
    if(FT_Set_Pixel_Sizes(face, 0, fontH) != 0) return false;

    FT_Pos penX = ((FT_Pos)org.x) << 6;
    int baselineY = org.y;
    FT_UInt prevGlyph = 0;
    bool useKerning = FT_HAS_KERNING(face);

    for(unsigned char ch : text) {
        FT_UInt glyphIndex = FT_Get_Char_Index(face, ch);
        if(useKerning && prevGlyph != 0 && glyphIndex != 0) {
            FT_Vector delta;
            if(FT_Get_Kerning(face, prevGlyph, glyphIndex, FT_KERNING_DEFAULT, &delta) == 0) {
                penX += delta.x;
            }
        }
        if(FT_Load_Glyph(face, glyphIndex, FT_LOAD_DEFAULT) != 0) {
            continue;
        }
        if(FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0) {
            continue;
        }

        FT_GlyphSlot glyph = face->glyph;
        FT_Bitmap& bitmap = glyph->bitmap;
        int startX = (int)(penX >> 6) + glyph->bitmap_left;
        int startY = baselineY - glyph->bitmap_top;

        for(int row = 0; row < (int)bitmap.rows; row++) {
            int dstY = startY + row;
            if(dstY < 0 || dstY >= img.rows) continue;

            const unsigned char* srcRow = bitmap.buffer + row * bitmap.pitch;
            for(int col = 0; col < (int)bitmap.width; col++) {
                int dstX = startX + col;
                if(dstX < 0 || dstX >= img.cols) continue;

                double alpha = srcRow[col] / 255.0;
                if(alpha <= 0.0) continue;

                cv::Vec3b& pixel = img.at<cv::Vec3b>(dstY, dstX);
                pixel[0] = (uchar)(pixel[0] * (1.0 - alpha) + color[0] * alpha);
                pixel[1] = (uchar)(pixel[1] * (1.0 - alpha) + color[1] * alpha);
                pixel[2] = (uchar)(pixel[2] * (1.0 - alpha) + color[2] * alpha);
            }
        }

        penX += glyph->advance.x;
        prevGlyph = glyphIndex;
    }

    return true;
}
#endif

// --- Header HUD rendering helpers ---
static void drawRoundedPrimitive(cv::Mat& img, const cv::Rect& rect, int radius, const cv::Scalar& color)
{
    if(rect.width <= 0 || rect.height <= 0) return;

    int r = std::min(radius, std::min(rect.width / 2, rect.height / 2));
    int x = rect.x, y = rect.y, w = rect.width, h = rect.height;

    // Rectangles
    cv::rectangle(img, cv::Point(x + r, y), cv::Point(x + w - r, y + h), color, cv::FILLED);
    cv::rectangle(img, cv::Point(x, y + r), cv::Point(x + w, y + h - r), color, cv::FILLED);

    // Circles (corrected off-by-one for right/bottom corners)
    cv::circle(img, cv::Point(x + r, y + r), r, color, cv::FILLED);
    cv::circle(img, cv::Point(x + w - r - 1, y + r), r, color, cv::FILLED);
    cv::circle(img, cv::Point(x + r, y + h - r - 1), r, color, cv::FILLED);
    cv::circle(img, cv::Point(x + w - r - 1, y + h - r - 1), r, color, cv::FILLED);
}

static void drawFilledRoundedRect(cv::Mat& img, const cv::Rect& rect, int radius, const cv::Scalar& color, double alpha = 0.95)
{
    cv::Rect clipped = rect & cv::Rect(0, 0, img.cols, img.rows);
    if(clipped.width <= 0 || clipped.height <= 0) return;

    if(alpha >= 0.99) {
        drawRoundedPrimitive(img, clipped, radius, color);
    } else {
        // Optimize: blend only the clipped ROI
        cv::Mat roi = img(clipped);
        cv::Mat overlay;
        roi.copyTo(overlay);
        cv::Rect localRect(0, 0, clipped.width, clipped.height);
        drawRoundedPrimitive(overlay, localRect, radius, color);
        cv::addWeighted(overlay, alpha, roi, 1.0 - alpha, 0.0, roi);
    }
}

static cv::Size headerTextSize(const std::string& text, int fontH, HeaderFontWeight weight, int thickness = 1)
{
#ifdef HAVE_OPENCV_FREETYPE
    auto ft = getHeaderFont(weight);
    if(!ft.empty()) {
        int baseline = 0;
        return ft->getTextSize(text, fontH, thickness, &baseline);
    }
#elif defined(HAVE_FREETYPE_DIRECT)
    auto size = headerTextSizeWithFreeType(text, fontH, weight);
    if(size.width > 0 && size.height > 0) return size;
#else
    (void)weight;  // unused in fallback builds
#endif
    double scale = std::max(0.1, fontH / 28.0);
    int baseline = 0;
    return cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, scale, std::max(1, thickness), &baseline);
}

static int headerTextBaselineYForCenter(const std::string& text, int fontH, HeaderFontWeight weight, int centerY)
{
    auto sz = headerTextSize(text, fontH, weight);
    return centerY + sz.height / 2;
}

static void drawHeaderText(cv::Mat& img, const std::string& text, cv::Point org, int fontH,
                           HeaderFontWeight weight, const cv::Scalar& color, int thickness = -1)
{
#ifdef HAVE_OPENCV_FREETYPE
    auto ft = getHeaderFont(weight);
    if(!ft.empty()) {
        ft->putText(img, text, org, fontH, color, thickness, cv::LINE_AA, true);
        return;
    }
#elif defined(HAVE_FREETYPE_DIRECT)
    if(drawHeaderTextWithFreeType(img, text, org, fontH, weight, color, thickness)) {
        return;
    }
#else
    (void)weight;  // unused in fallback builds
#endif
    double scale = std::max(0.1, fontH / 28.0);
    int thick = (thickness > 0) ? thickness : std::max(1, (int)(scale * 2));
    cv::putText(img, text, org, cv::FONT_HERSHEY_SIMPLEX, scale, color, thick, cv::LINE_AA);
}

static int fitHeaderFontHeight(const std::string& text, int fontH, int maxWidth, HeaderFontWeight weight)
{
    auto sz = headerTextSize(text, fontH, weight);
    if(sz.width > maxWidth && sz.width > 0)
        return std::max(10, fontH * maxWidth / sz.width);
    return fontH;
}

// --- Text formatting helpers ---
static bool isNumericToken(const std::string& token)
{
    if(token.empty()) return false;
    for(char ch : token) {
        if(!std::isdigit((unsigned char)ch)) return false;
    }
    return true;
}

static std::string upperToken(const std::string& token)
{
    std::string result;
    for(char ch : token) {
        result += std::toupper((unsigned char)ch);
    }
    return result;
}

static std::string formatModelToken(const std::string& token)
{
    std::string lower = token;
    for(auto& ch : lower) ch = std::tolower((unsigned char)ch);

    // Special formatting for known tokens
    if(lower == "yolov5s") return "YOLOv5S";
    if(lower == "yolov7") return "YOLOv7";
    if(lower == "yolov8") return "YOLOv8";
    if(lower == "yolov9") return "YOLOv9";
    if(lower == "yolox") return "YOLOX";
    if(lower == "yolov3") return "YOLOv3";
    if(lower == "yolov4") return "YOLOv4";
    if(lower == "scrfd") return "SCRFD";
    if(lower == "ppu") return "PPU";
    if(lower == "face") return "Face";
    if(lower.find("yolo") == 0 && lower.size() > 4) {
        // Generic YOLOvX handling
        return "YOLO" + upperToken(lower.substr(4));
    }

    // Default: uppercase
    return upperToken(token);
}

static std::string formatHeaderModelName(const std::string& modelName)
{
    std::vector<std::string> tokens;
    std::string token;
    for(size_t i = 0; i <= modelName.size(); i++) {
        if(i == modelName.size() || modelName[i] == '_') {
            if(!token.empty() && !isNumericToken(token)) {
                tokens.push_back(formatModelToken(token));
            }
            token.clear();
        } else {
            token += modelName[i];
        }
    }

    if(tokens.empty()) return upperToken(modelName);

    std::string result;
    for(size_t i = 0; i < tokens.size(); i++) {
        if(i > 0) result += " ";
        result += tokens[i];
    }
    return result;
}

static std::string headerDeviceLabel(int numDevices)
{
    auto deviceNames = getDeepxDeviceNames(numDevices);
    if(deviceNames.empty()) return "DEEPX DEVICE";

    std::string result;
    for(size_t i = 0; i < deviceNames.size(); i++) {
        if(i > 0) result += ", ";
        result += deviceNames[i];
    }
    return result;
}

static std::string headerHardwareLabel(int numDevices)
{
    std::string deviceLabel = headerDeviceLabel(numDevices);
    if(deviceLabel.find("DEEPX") == 0) return deviceLabel;
    return "DEEPX " + deviceLabel;
}

static std::string fpsValueText(float fps)
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << fps;
    return oss.str();
}

static cv::Mat headerDeepxLogo()
{
    static cv::Mat g_deepxLogo;
    static bool g_deepxLogoLoadAttempted = false;

    if(g_deepxLogoLoadAttempted) {
        return g_deepxLogo;
    }

    g_deepxLogoLoadAttempted = true;

    std::vector<std::string> searchPaths;

#ifdef PROJECT_ROOT_DIR
    searchPaths.push_back(std::string(PROJECT_ROOT_DIR) + "/sample/header/deepx_logo.png");
#endif
    searchPaths.push_back("./sample/header/deepx_logo.png");
    searchPaths.push_back("../sample/header/deepx_logo.png");

    for(const auto& path : searchPaths) {
        cv::Mat logo = cv::imread(path, cv::IMREAD_UNCHANGED);
        if(!logo.empty()) {
            g_deepxLogo = logo;
            std::cout << "[HeaderUI] Loaded logo: " << path << std::endl;
            return g_deepxLogo;
        }
    }

    std::cout << "[HeaderUI] Failed to load deepx logo from any search path" << std::endl;
    return cv::Mat();
}

static cv::Size headerLogoTargetSize(const cv::Mat& logo, int maxW, int maxH)
{
    static constexpr int MIN_LOGO_DIMENSION = 10;
    if(logo.empty() || logo.cols < MIN_LOGO_DIMENSION || logo.rows < MIN_LOGO_DIMENSION || maxW <= 0 || maxH <= 0) {
        return cv::Size(0, 0);
    }

    double scaleW = (double)maxW / logo.cols;
    double scaleH = (double)maxH / logo.rows;
    double scale = std::min(scaleW, scaleH);

    int targetW = (int)(logo.cols * scale);
    int targetH = (int)(logo.rows * scale);

    if(targetW < MIN_LOGO_DIMENSION || targetH < MIN_LOGO_DIMENSION) {
        return cv::Size(0, 0);
    }

    return cv::Size(targetW, targetH);
}

static cv::Size drawHeaderLogo(cv::Mat& frame, cv::Point org, int maxW, int maxH)
{
    if(frame.empty() || frame.cols <= 0 || frame.rows <= 0 || maxW <= 0 || maxH <= 0) {
        return cv::Size(0, 0);
    }

    if(frame.type() != CV_8UC3) {
        return cv::Size(0, 0);
    }

    cv::Mat logo = headerDeepxLogo();
    cv::Size logoSize = headerLogoTargetSize(logo, maxW, maxH);

    if(logoSize.width <= 0 || logoSize.height <= 0) {
        return cv::Size(0, 0);
    }

    if(org.x < 0 || org.y < 0 ||
       org.x + logoSize.width > frame.cols ||
       org.y + logoSize.height > frame.rows) {
        return cv::Size(0, 0);
    }

    static cv::Mat cachedResizedLogo;
    static cv::Size cachedLogoSize;
    if(cachedResizedLogo.empty() || cachedLogoSize != logoSize) {
        cv::resize(logo, cachedResizedLogo, logoSize, 0, 0, cv::INTER_AREA);
        cachedLogoSize = logoSize;
    }

    if(cachedResizedLogo.empty()) {
        return cv::Size(0, 0);
    }

    cv::Rect roi(org.x, org.y, logoSize.width, logoSize.height);

    if(cachedResizedLogo.channels() == 4) {
        for(int y = 0; y < cachedResizedLogo.rows; y++) {
            for(int x = 0; x < cachedResizedLogo.cols; x++) {
                cv::Vec4b pixel = cachedResizedLogo.at<cv::Vec4b>(y, x);
                double alpha = pixel[3] / 255.0;

                cv::Vec3b& framePixel = frame.at<cv::Vec3b>(org.y + y, org.x + x);
                framePixel[0] = (uchar)(framePixel[0] * (1.0 - alpha) + pixel[0] * alpha);
                framePixel[1] = (uchar)(framePixel[1] * (1.0 - alpha) + pixel[1] * alpha);
                framePixel[2] = (uchar)(framePixel[2] * (1.0 - alpha) + pixel[2] * alpha);
            }
        }
    } else if(cachedResizedLogo.channels() == 3) {
        cachedResizedLogo.copyTo(frame(roi));
    } else {
        return cv::Size(0, 0);
    }

    return logoSize;
}

// --- Unified Header HUD renderer (cached static layer + per-frame FPS update) ---
static void renderHeaderHud(cv::Mat& frame,
                           int boardW, int titleH,
                           int activeStreams, int numDevices,
                           float totalFps,
                           bool showFps, bool calcFps,
                           const std::string& modelName,
                           float fpsValueFontScale = 0.5f)
{
    if(frame.empty() || boardW <= 0 || titleH <= 0) return;

    // --- Static cache for the HUD layer (rebuilt only when parameters change) ---
    static cv::Mat s_hudCache;
    static int s_boardW = 0, s_titleH = 0, s_streams = 0, s_devices = 0;
    static std::string s_model;
    static float s_fpsScale = -1.0f;
    // Cached positions for per-frame FPS value drawing
    static int s_totalValueSlotX = 0, s_avgValueSlotX = 0;
    static int s_totalValueSlotW = 0, s_avgValueSlotW = 0;
    static int s_metricsBaselineY = 0, s_valueFontH = 0;

    bool needRebuild = s_hudCache.empty() ||
                       s_boardW != boardW || s_titleH != titleH ||
                       s_streams != activeStreams || s_devices != numDevices ||
                       s_model != modelName || s_fpsScale != fpsValueFontScale;

    if(needRebuild) {
        s_hudCache = cv::Mat::zeros(titleH, boardW, CV_8UC3);
        s_boardW = boardW; s_titleH = titleH;
        s_streams = activeStreams; s_devices = numDevices;
        s_model = modelName; s_fpsScale = fpsValueFontScale;

        // Colors (BGR)
        const cv::Scalar COLOR_HUD_BG   (26,  26,  26);
        const cv::Scalar COLOR_DEEPX    (255, 85,  47);
        const cv::Scalar COLOR_TEXT_PRI (245, 245, 245);
        const cv::Scalar COLOR_TEXT_SEC (208, 208, 208);
        const cv::Scalar COLOR_BADGE_BG    (0,   0,   0);
        const cv::Scalar COLOR_BADGE_BORDER(245, 245, 245);
        const cv::Scalar COLOR_BADGE_DOT   (31,  31, 255);

        // Layout proportions
        int cardH   = titleH;
        int cardTop = 0;
        int marginX = 0;
        int gap     = std::max(4, (int)(boardW * 0.004));
        int metricsW = std::max(280, (int)(boardW * 0.235));
        int mainW   = boardW - marginX * 2 - gap - metricsW;
        int mainCardRadius = std::max(24, (int)(cardH * 0.28));

        if(mainW < 320) {
            metricsW = std::max(160, boardW - marginX * 2 - gap - 320);
            if(metricsW < 160) metricsW = 160;
            mainW = boardW - marginX * 2 - gap - metricsW;
            if(mainW < 1) mainW = 1;
        }

        int mainX    = marginX;
        int metricsX = marginX + mainW + gap;

        // Draw cards (opaque — no alpha blend needed for cache)
        drawFilledRoundedRect(s_hudCache, cv::Rect(mainX, cardTop, mainW, cardH), mainCardRadius, COLOR_HUD_BG, 1.0);
        drawFilledRoundedRect(s_hudCache, cv::Rect(metricsX, cardTop, metricsW, cardH), mainCardRadius, COLOR_HUD_BG, 1.0);

        // --- Main card content ---
        int contentX = mainX + std::max(20, (int)(mainW * 0.035));
        int contentY = cardTop + cardH / 2;

        // DEEPX logo
        cv::Size logoSize = headerLogoTargetSize(headerDeepxLogo(), mainW / 4, (int)(cardH * 0.45));
        int afterDeepx;

        if(logoSize.width > 0 && logoSize.height > 0) {
            int logoY = contentY - logoSize.height / 2;
            logoSize = drawHeaderLogo(s_hudCache, cv::Point(contentX, logoY), mainW / 4, (int)(cardH * 0.45));
        }

        if(logoSize.width > 0 && logoSize.height > 0) {
            afterDeepx = contentX + logoSize.width + std::max(16, (int)(mainW * 0.024));
        } else {
            int deepxFontH = std::max(28, (int)(cardH * 0.45));
            deepxFontH = fitHeaderFontHeight("DEEPX", deepxFontH, mainW / 4, HeaderFontWeight::ExtraBold);
            auto deepxSz = headerTextSize("DEEPX", deepxFontH, HeaderFontWeight::ExtraBold);
            drawHeaderText(s_hudCache, "DEEPX", cv::Point(contentX, contentY - deepxSz.height / 6),
                           deepxFontH, HeaderFontWeight::ExtraBold, COLOR_DEEPX);
            afterDeepx = contentX + deepxSz.width + std::max(16, (int)(mainW * 0.024));
        }

        std::string displayModel = formatHeaderModelName(modelName);

        // Title + badge layout
        int badgeH = std::max(20, (int)(cardH * 0.28));
        std::string liveTxt = "LIVE";
        int liveFontH = std::max(12, (int)(badgeH * 0.56));
        auto liveSz = headerTextSize(liveTxt, liveFontH, HeaderFontWeight::Bold);
        int badgeExtraPad = std::max(16, (int)(badgeH * 0.45));
        int badgeW = liveSz.width + std::max(26, (int)(badgeH)) + badgeExtraPad;
        int rightPad = std::max(14, (int)(mainW * 0.018));
        int separatorGap = std::max(14, (int)(mainW * 0.018));
        int subAreaW = std::max(230, (int)(mainW * 0.28));
        subAreaW = std::min(subAreaW, std::max(180, (int)(mainW * 0.34)));
        int separatorX = mainX + mainW - rightPad - subAreaW - separatorGap;
        int titleBadgeGap = std::max(10, (int)(mainW * 0.012));
        int titleMaxW = separatorX - afterDeepx - badgeW - titleBadgeGap * 2;
        titleMaxW = std::max(80, titleMaxW);

        std::string title = std::to_string(activeStreams) + " CH. Real-time Processing";
        int titleFontH = std::max(16, (int)(cardH * 0.28));
        int titleTextMaxW = std::max(80, titleMaxW);
        titleFontH = fitHeaderFontHeight(title, titleFontH, titleTextMaxW, HeaderFontWeight::Bold);
        auto titleTextSz = headerTextSize(title, titleFontH, HeaderFontWeight::Bold);
        int titleBadgeNarrowGap = std::max(8, (int)(mainW * 0.04));
        int titleAreaInset = std::max(6, (int)(titleMaxW * 0.02));
        int titleX = afterDeepx + titleAreaInset;
        int titleY = headerTextBaselineYForCenter(title, titleFontH, HeaderFontWeight::Bold, contentY);

        drawHeaderText(s_hudCache, title, cv::Point(titleX, titleY - (titleTextSz.height * 0.16)),
                       titleFontH, HeaderFontWeight::Bold, COLOR_TEXT_PRI);

        // LIVE badge
        int badgeX = titleX + titleTextSz.width + titleBadgeNarrowGap;
        badgeX = std::min(badgeX, separatorX - badgeW);
        badgeX = std::max(afterDeepx, badgeX);
        int badgeY = cardTop + (cardH - badgeH) / 2;
        int badgeBorder = 1;
        cv::Rect badgeRect(badgeX, badgeY, badgeW, badgeH);
        drawFilledRoundedRect(s_hudCache, badgeRect, badgeH / 2, COLOR_BADGE_BORDER, 1.0);
        cv::Rect innerBadgeRect(badgeRect.x + badgeBorder,
                                badgeRect.y + badgeBorder,
                                badgeRect.width - badgeBorder * 2,
                                badgeRect.height - badgeBorder * 2);
        if(innerBadgeRect.width > 0 && innerBadgeRect.height > 0) {
            drawFilledRoundedRect(s_hudCache, innerBadgeRect, innerBadgeRect.height / 2, COLOR_BADGE_BG, 1.0);
        }

        int dotR = std::max(4, (int)(cardH * 0.064));
        int liveDotTextGap = std::max(6, (int)(badgeH * 0.2));
        int liveGroupW = dotR * 2 + liveDotTextGap + liveSz.width;
        int liveGroupX = badgeX + (badgeW - liveGroupW) / 2;
        int dotX = liveGroupX + dotR;
        int dotY = badgeY + badgeH / 2;
        cv::circle(s_hudCache, cv::Point(dotX, dotY), dotR, COLOR_BADGE_DOT, cv::FILLED);

        int liveTextX = dotX + dotR + liveDotTextGap;
        int liveTextY = headerTextBaselineYForCenter(liveTxt, liveFontH, HeaderFontWeight::Bold, badgeY + badgeH / 2);
        drawHeaderText(s_hudCache, liveTxt, cv::Point(liveTextX, liveTextY),
                       liveFontH, HeaderFontWeight::Bold, COLOR_TEXT_PRI);

        // Separator
        int separatorTop = cardTop + std::max(10, (int)(cardH * 0.22));
        int separatorBottom = cardTop + cardH - std::max(10, (int)(cardH * 0.22));
        cv::line(s_hudCache, cv::Point(separatorX, separatorTop),
                 cv::Point(separatorX, separatorBottom), COLOR_TEXT_SEC, 1, cv::LINE_AA);

        // AI Model / Hardware info
        int subAreaX = separatorX + separatorGap;
        int subAreaMaxW = mainX + mainW - rightPad - subAreaX;
        std::string aiModelText = "AI Model: Object Detection (" + displayModel + ")";
        std::string hardwareText = "Hardware: " + headerHardwareLabel(numDevices);
        int subFontH = std::max(12, (int)(cardH * 0.24));
        subFontH = std::min(fitHeaderFontHeight(aiModelText, subFontH, subAreaMaxW, HeaderFontWeight::SemiBold),
                            fitHeaderFontHeight(hardwareText, subFontH, subAreaMaxW, HeaderFontWeight::SemiBold));
        auto aiModelSz = headerTextSize(aiModelText, subFontH, HeaderFontWeight::SemiBold);
        auto hardwareSz = headerTextSize(hardwareText, subFontH, HeaderFontWeight::SemiBold);
        int subLineGap = std::max(5, (int)(cardH * 0.14));
        int subGroupH = aiModelSz.height + subLineGap + hardwareSz.height;
        int aiModelY = contentY - subGroupH / 2 + aiModelSz.height;
        int hardwareY = aiModelY + subLineGap + hardwareSz.height;

        drawHeaderText(s_hudCache, aiModelText, cv::Point(subAreaX, aiModelY),
                       subFontH, HeaderFontWeight::SemiBold, COLOR_TEXT_SEC);
        drawHeaderText(s_hudCache, hardwareText, cv::Point(subAreaX, hardwareY),
                       subFontH, HeaderFontWeight::SemiBold, COLOR_TEXT_SEC);

        // --- Metrics card: labels only (values drawn per-frame) ---
        int mContentX = metricsX + std::max(16, (int)(metricsW * 0.06));
        int mContentW = metricsW - std::max(32, (int)(metricsW * 0.12));

        int labelFontH = std::max(13, (int)(cardH * 0.27));
        int valueFontH = std::max(labelFontH + 4, (int)(cardH * 0.36 * fpsValueFontScale));
        int metricLabelValueGap = std::max(6, (int)(metricsW * 0.018));
        int metricItemGap = std::max(18, (int)(metricsW * 0.055));
        std::string totalValueSlotText = "9999.9";
        std::string avgValueSlotText = "99.9";
        auto totalLabelSz = headerTextSize("Total FPS", labelFontH, HeaderFontWeight::Bold);
        auto totalValueSlotSz = headerTextSize(totalValueSlotText, valueFontH, HeaderFontWeight::Bold);
        auto avgLabelSz = headerTextSize("AVG FPS", labelFontH, HeaderFontWeight::Bold);
        auto avgValueSlotSz = headerTextSize(avgValueSlotText, valueFontH, HeaderFontWeight::Bold);
        int totalValueSlotW = totalValueSlotSz.width;
        int avgValueSlotW = avgValueSlotSz.width;
        int metricsGroupW = totalLabelSz.width + metricLabelValueGap + totalValueSlotW + metricItemGap + avgLabelSz.width + metricLabelValueGap + avgValueSlotW;
        if(metricsGroupW > mContentW && metricsGroupW > 0) {
            double shrink = std::max(0.72, (double)mContentW / metricsGroupW);
            labelFontH = std::max(8, (int)(labelFontH * shrink));
            valueFontH = std::max(labelFontH + 2, (int)(valueFontH * shrink));
            totalLabelSz = headerTextSize("Total FPS", labelFontH, HeaderFontWeight::Bold);
            totalValueSlotSz = headerTextSize(totalValueSlotText, valueFontH, HeaderFontWeight::Bold);
            avgLabelSz = headerTextSize("AVG FPS", labelFontH, HeaderFontWeight::Bold);
            avgValueSlotSz = headerTextSize(avgValueSlotText, valueFontH, HeaderFontWeight::Bold);
            totalValueSlotW = totalValueSlotSz.width;
            avgValueSlotW = avgValueSlotSz.width;
            metricsGroupW = totalLabelSz.width + metricLabelValueGap + totalValueSlotW + metricItemGap + avgLabelSz.width + metricLabelValueGap + avgValueSlotW;
        }

        int metricX = mContentX + std::max(0, (mContentW - metricsGroupW) / 2);
        int metricsBaselineY = cardTop + (cardH + valueFontH) / 2 - std::max(1, (int)(cardH * 0.03));

        // Draw static labels into cache
        drawHeaderText(s_hudCache, "Total FPS", cv::Point(metricX, metricsBaselineY),
                       labelFontH, HeaderFontWeight::Bold, COLOR_TEXT_SEC);
        int totalSlotStartX = metricX + totalLabelSz.width + metricLabelValueGap;
        int avgLabelX = totalSlotStartX + totalValueSlotW + metricItemGap;
        drawHeaderText(s_hudCache, "AVG FPS", cv::Point(avgLabelX, metricsBaselineY),
                       labelFontH, HeaderFontWeight::Bold, COLOR_TEXT_SEC);
        int avgSlotStartX = avgLabelX + avgLabelSz.width + metricLabelValueGap;

        // Save positions for per-frame value rendering
        s_totalValueSlotX = totalSlotStartX;
        s_avgValueSlotX = avgSlotStartX;
        s_totalValueSlotW = totalValueSlotW;
        s_avgValueSlotW = avgValueSlotW;
        s_metricsBaselineY = metricsBaselineY;
        s_valueFontH = valueFontH;
    }

    // --- Per-frame: copy cached HUD and draw dynamic FPS values ---
    if(boardW <= frame.cols && titleH <= frame.rows) {
        cv::Mat hudRoi = frame(cv::Rect(0, 0, boardW, titleH));
        s_hudCache.copyTo(hudRoi);
    }

    // Draw FPS values (only dynamic part)
    const cv::Scalar COLOR_FPS(20, 255, 32);
    bool hasFps = showFps && calcFps && activeStreams > 0;
    if(hasFps) {
        std::string totalText = fpsValueText(totalFps);
        std::string avgText   = fpsValueText(totalFps / activeStreams);
        auto totalValueSz = headerTextSize(totalText, s_valueFontH, HeaderFontWeight::Bold);
        auto avgValueSz = headerTextSize(avgText, s_valueFontH, HeaderFontWeight::Bold);
        int totalValueX = s_totalValueSlotX + s_totalValueSlotW - totalValueSz.width;
        int avgValueX = s_avgValueSlotX + s_avgValueSlotW - avgValueSz.width;
        drawHeaderText(frame, totalText, cv::Point(totalValueX, s_metricsBaselineY),
                       s_valueFontH, HeaderFontWeight::Bold, COLOR_FPS);
        drawHeaderText(frame, avgText, cv::Point(avgValueX, s_metricsBaselineY),
                       s_valueFontH, HeaderFontWeight::Bold, COLOR_FPS);
    }
}


#ifdef _WIN32
/**
 * @brief 프로세스 수명 동안 윈도우 타이머 해상도를 올린다.
 *
 * 윈도우 기본 타이머 틱은 15.6ms 다. Sleep(t) 와 cv::waitKey(1) 이 모두 이 틱
 * 경계로 반올림되므로 다음이 통째로 낭비된다.
 *   - 채널 워커의 프레임 페이싱 : 반복당 약 5.6ms 초과 수면
 *     (측정값: sleep_req 25.32ms -> sleep_act 30.89ms, p90 은 27.14 -> 44.03ms)
 *   - 렌더 루프의 waitKey(1)    : 틱 2개 = 약 31ms
 *
 * Windows 11 부터 timeBeginPeriod 는 호출한 프로세스에만 적용되므로
 * 시스템 전역에 영향을 주지 않는다. 소멸자에서 반드시 짝을 맞춘다.
 */
class TimerResolution
{
public:
    explicit TimerResolution(UINT desiredMs)
    {
        TIMECAPS tc;
        if(timeGetDevCaps(&tc, sizeof(tc)) != TIMERR_NOERROR)
            return;
        UINT want = std::min(std::max(desiredMs, tc.wPeriodMin), tc.wPeriodMax);
        if(timeBeginPeriod(want) == TIMERR_NOERROR)
            _period = want;
    }
    ~TimerResolution() { if(_period != 0) timeEndPeriod(_period); }

    TimerResolution(const TimerResolution&) = delete;
    TimerResolution& operator=(const TimerResolution&) = delete;

    /// 실제로 적용된 주기(ms). 0 이면 실패.
    UINT Period() const { return _period; }

private:
    UINT _period = 0;
};
#endif

int main(int argc, char *argv[])
{
DXRT_TRY_CATCH_BEGIN
    // Ctrl+C / kill -> clean shutdown (needed since a non-activating fullscreen
    // window has no 'X' button and can't receive ESC/'q' key events).
    std::signal(SIGINT, onExitSignal);
    std::signal(SIGTERM, onExitSignal);

#ifdef _WIN32
    // 기본 15.6ms 틱 -> 1ms. 자세한 배경은 TimerResolution 주석 참고.
    // 프로파일러 로그의 sleep(1ms).actual_ms 로 적용 여부를 검증할 수 있다.
    TimerResolution timerRes(1);
    if(timerRes.Period() == 0)
        std::cerr << "[Timer] WARNING: failed to raise timer resolution; "
                     "Sleep() stays quantized to ~15.6 ms" << std::endl;
    else
        std::cout << "[Timer] resolution raised to " << timerRes.Period()
                  << " ms" << std::endl;

    SetProcessPriorityBoost(GetCurrentProcess(), TRUE);
#endif

    std::string configPath = "";
    double frameCount = 0.0, window_size = 60.0;
    bool loggingVersion = false;
    int fpsLogPeriodMs = 0; // CLI override for console FPS log period (ms)
    std::string profilePath = "";
    int profilePeriodMs = 0, profileWarmupMs = 0;

    AppConfig appConfig;

    cxxopts::Options options("yolo_multi", "yolo multi channels application usage ");
    options.add_options()
        ("c, config", "(* required) use config json file for run application", cxxopts::value<std::string>(configPath))
        ("t, test", "test mode", cxxopts::value<bool>(loggingVersion)->default_value("false"))
        ("window_size", "FPS by average over the last {window_size} seconds (default: 60)", cxxopts::value<double>(window_size)->default_value("60"))
        ("fps_log_period", "print FPS to terminal every N ms (0 = off)", cxxopts::value<int>(fpsLogPeriodMs))
        ("profile", "write per-stage timing log to this file", cxxopts::value<std::string>(profilePath)->implicit_value(""))
        ("profile_period", "profiler dump interval in ms", cxxopts::value<int>(profilePeriodMs))
        ("profile_warmup", "ignore the first N ms before measuring", cxxopts::value<int>(profileWarmupMs))
        ("h, help", "print usage")
    ;
    auto cmd = options.parse(argc, argv);
    if(cmd.count("help"))
    {
        std::cout << options.help() << std::endl;
        exit(0);
    }
    if(configPath.empty())
    {
        std::cout << "error: no config json file arguments." << std::endl;
        std::cout << "Use -h or --help for usage information." << std::endl;
        exit(0);
    }

    if(ApplicationJsonParser(configPath, &appConfig) < 0)
    {
        std::cout << "error: failed to parse config json file." << std::endl;
        std::cout << "Use -h or --help for usage information." << std::endl;
        exit(0);
    }

    LOG_VALUE(configPath);

    // CLI(--fps_log_period)가 주어지면 JSON 설정보다 우선한다.
    if(cmd.count("fps_log_period"))
        appConfig.console_fps_period_ms = std::max(0, fpsLogPeriodMs);

    const int BOARD_WIDTH = appConfig.board_width;
    const int BOARD_HEIGHT = appConfig.board_height;

    // 상단 타이틀 바 높이 (통합 HUD 헤더)
    // Windows: 디스플레이 창에서 헤더 패널(로고/모델/FPS 카드)이 깨져서
    //          헤더를 완전히 제거하고 그리드가 전체 높이를 사용하도록 함.
#ifdef _WIN32
    const int TITLE_HEIGHT = 0;
#else
    const int HEADER_MIN_HEIGHT = 90;
    const int TITLE_HEIGHT = std::max(HEADER_MIN_HEIGHT, (int)(BOARD_HEIGHT * 0.105));
#endif
    const int GRID_WIDTH = BOARD_WIDTH;

    // 그리드 크기 결정: JSON에 명시되면 사용, 아니면 자동(sqrt)
    int grid_cols, grid_rows;
    if(appConfig.grid_cols > 0 && appConfig.grid_rows > 0)
    {
        grid_cols = appConfig.grid_cols;
        grid_rows = appConfig.grid_rows;
    }
    else
    {
        int n = (int)appConfig.video_sources.size();
        grid_cols = (int)ceil(sqrt((double)n));
        grid_rows = (grid_cols > 0) ? (int)ceil((double)n / grid_cols) : 1;
    }
    int divWidth  = GRID_WIDTH  / grid_cols;
    int divHeight = (BOARD_HEIGHT - TITLE_HEIGHT) / grid_rows;
    if(appConfig.is_expand_mode && appConfig.video_sources.size()!=33 && appConfig.video_sources.size()!=73 && appConfig.video_sources.size()!= 61 && appConfig.video_sources.size()!=41) {
        appConfig.is_expand_mode = false;
    }

    // 스트림 셀 간 구분선 두께(px). 각 셀을 GAP만큼 줄이고 GAP/2 만큼 이동시켜
    // 인접 셀 사이가 회색 배경(아래 outFrame 초기색)으로 노출되도록 함.
    // 매 프레임 그리지 않으므로 런타임 비용 0.
    const int SEPARATOR_GAP = std::max(2, BOARD_HEIGHT / 360);
    const int CELL_OFFSET   = SEPARATOR_GAP / 2;
    const cv::Scalar SEPARATOR_COLOR(0, 0, 0);

    cv::Mat outFrame = cv::Mat(cv::Size(BOARD_WIDTH, BOARD_HEIGHT), CV_8UC3, SEPARATOR_COLOR);
    // 사이드바 영역은 사이드바 자체가 매 프레임 채우므로 그대로 둠.
    auto io = dxrt::InferenceOption();
    io.useORT = false;
    auto ie = std::make_shared<dxrt::InferenceEngine>(appConfig.model_path, io);
    if(!(dxdemo::common::minversionforRTandCompiler(ie.get()) || ie.get()->IsPPU()))
    {
        std::cerr << "[DXDEMO] [ER] The version of the compiled model is not compatible with the version of the runtime. Please compile the model again." << std::endl;
        return -1;
    }
    // 프로파일러의 preload 메모리 추정에 쓰는 NPU 입력 한 변의 크기
    const int npuInputSize = (int)ie->GetInputs().front().shape()[1];
    yoloParam = getYoloParameter(appConfig.model_name);
    Yolo yolo = Yolo(yoloParam);
    std::vector<std::shared_ptr<ObjectDetection>> apps;
    uint64_t allFrameCount = 0;  // 64비트로 변경하여 오버플로우 방지
    bool calcFps = false;

    // ---- 카메라 강조 레이아웃 (is_expand_mode와 별개) ----
    // "camera" 입력이 있으면 그 중 첫 번째를 가운데 영역에 확장 배치하고
    // 노란 테두리 + LIVE 배지를 표시. 모든 카메라 입력에는 노란 테두리/LIVE 적용.
    const int CAMERA_BORDER       = std::max(4, BOARD_HEIGHT / 240);  // 노란 테두리 두께
    const cv::Scalar CAMERA_COLOR(0, 255, 255);                        // BGR Yellow

    int cameraExpandIdx = -1;
    int cameraScale = 1;
    int cameraOriginCol = 0, cameraOriginRow = 0;
    std::vector<bool> cameraReservedCell(grid_cols * grid_rows, false);
    if(!appConfig.is_expand_mode)
    {
        for(int i = 0; i < (int)appConfig.video_sources.size(); i++)
        {
            const std::string& srcType = appConfig.video_sources[i].second;
            if(srcType == "camera"
               || srcType == "camera_image"
               || srcType == "camera_video")
            {
                cameraExpandIdx = i;
                break;
            }
        }
        if(cameraExpandIdx >= 0)
        {
            // 카메라 셀이 전체 그리드 면적의 20% 를 넘지 않도록 결정
            //   scale ≤ floor( sqrt(0.20 × cols × rows) )
            // 예) 5x5→2(16%), 8x8→3(14%), 10x10→4(16%), 16x16→7(19%),
            //     30x30→13(18%), 100x100→44(19%), 4x4→1(확장 없음)
            cameraScale = (int)std::floor(std::sqrt(0.20 * (double)grid_cols * grid_rows));
            if(cameraScale < 1) cameraScale = 1;

            int totalCells   = grid_cols * grid_rows;
            int othersCount  = (int)appConfig.video_sources.size() - 1;
            // 다른 스트림이 들어갈 셀이 부족하면 점진적으로 축소
            while(cameraScale > 1
                  && othersCount + cameraScale * cameraScale > totalCells)
            {
                cameraScale--;
            }

            cameraOriginCol = (grid_cols - cameraScale) / 2;
            cameraOriginRow = (grid_rows - cameraScale) / 2;
            for(int r = 0; r < cameraScale; r++)
                for(int c = 0; c < cameraScale; c++)
                    cameraReservedCell[(cameraOriginRow + r) * grid_cols
                                        + (cameraOriginCol + c)] = true;
        }
    }

    if(appConfig.is_expand_mode)
    {
	    int position_index=0;
        int Window_scale = 2;
        if(appConfig.video_sources.size() == 41 || appConfig.video_sources.size() == 73){
            Window_scale = 3;
        }
        for(int i=0;i<(int)appConfig.video_sources.size(); i++)
        {
		if(appConfig.video_sources.size() == 33){
            if(i < 14){
				position_index = i;
			} else if(i < 18){
				position_index = i+2;
			} else if (i < 32) {
				position_index = i+4;
			} else {
				position_index = 14;
			}
        }else if(appConfig.video_sources.size() == 41){
            if(i < 16){
				position_index = i;
			} else if(i < 20){
				position_index = i+3;
			} else if (i < 24) {
				position_index = i+6;
			} else if (i < 40) {
				position_index = i+9;
			} else {
				position_index = 16;
			}
        }else if(appConfig.video_sources.size() == 73){
            if(i < 30){
				position_index = i;
			} else if(i < 36){
				position_index = i+3;
			} else if (i < 42) {
				position_index = i+6;
			} else if (i < 72) {
				position_index = i+9;
			} else {
				position_index = 30;
			}
        }else if(appConfig.video_sources.size() == 61){
		    if(i < 27){
                position_index = i;
            } else if(i < 33){
                    position_index = i+2;
            } else if (i < 60) {
                    position_index = i+4;
            } else {
                    position_index = 27;
            }
	}

	   if( i == (int)appConfig.video_sources.size() - 1){
                apps.emplace_back(
                    std::make_shared<ObjectDetection>(
                        ie, appConfig.video_sources[i], i, yoloParam.width, yoloParam.height,
                        divWidth*Window_scale - SEPARATOR_GAP, divHeight*Window_scale - SEPARATOR_GAP,
                        divWidth*(position_index%grid_cols) + CELL_OFFSET,
                        TITLE_HEIGHT + divHeight*(position_index/grid_cols) + CELL_OFFSET,
                        appConfig.pre_saved_frame_count[i]
                    )
                );
	   } else {
                apps.emplace_back(
                    std::make_shared<ObjectDetection>(
                        ie, appConfig.video_sources[i], i, yoloParam.width, yoloParam.height,
                        divWidth - SEPARATOR_GAP, divHeight - SEPARATOR_GAP,
                        divWidth*(position_index%grid_cols) + CELL_OFFSET,
                        TITLE_HEIGHT + divHeight*(position_index/grid_cols) + CELL_OFFSET,
                        appConfig.pre_saved_frame_count[i]
                    )
                );
	    }

            std::cout << *apps.back() << std::endl;
        }
    }else
    {
        // 일반 레이아웃 (카메라 강조 포함)
        auto cellRectFor = [&](int gridIdx, bool isCameraExpanded) {
            int cols = isCameraExpanded ? cameraScale : 1;
            int rows = isCameraExpanded ? cameraScale : 1;
            int col  = isCameraExpanded ? cameraOriginCol : (gridIdx % grid_cols);
            int row  = isCameraExpanded ? cameraOriginRow : (gridIdx / grid_cols);
            cv::Rect r;
            r.x = divWidth  * col + CELL_OFFSET;
            r.y = TITLE_HEIGHT + divHeight * row + CELL_OFFSET;
            r.width  = divWidth  * cols - SEPARATOR_GAP;
            r.height = divHeight * rows - SEPARATOR_GAP;
            return r;
        };

        // 일반 입력에 사용할 다음 빈 셀 인덱스
        int nextCellIdx = 0;
        auto advanceToFreeCell = [&]() {
            while(nextCellIdx < grid_cols * grid_rows
                  && cameraReservedCell[nextCellIdx])
                nextCellIdx++;
        };

        for(int i = 0; i < (int)appConfig.video_sources.size(); i++)
        {
            const std::string& srcType  = appConfig.video_sources[i].second;
            const bool isCamera         = (srcType == "camera"
                                           || srcType == "camera_image"
                                           || srcType == "camera_video");
            const bool isCameraExpanded = (i == cameraExpandIdx);

            cv::Rect cell;
            if(isCameraExpanded)
            {
                cell = cellRectFor(0, true);
            }
            else
            {
                advanceToFreeCell();
                if(nextCellIdx >= grid_cols * grid_rows) break;
                cell = cellRectFor(nextCellIdx, false);
                nextCellIdx++;
            }

            // 카메라 입력은 노란 테두리 폭만큼 안쪽으로 더 inset
            int border = isCamera ? CAMERA_BORDER : 0;
            int destW = std::max(1, cell.width  - 2 * border);
            int destH = std::max(1, cell.height - 2 * border);
            int posX  = cell.x + border;
            int posY  = cell.y + border;

            // 노란 테두리: outFrame 위에 셀 영역 전체를 노랑으로 1회 채움
            // (스트림 ROI는 안쪽에 copy 되므로 가장자리만 노랑이 남음)
            if(isCamera)
            {
                cv::rectangle(outFrame, cell, CAMERA_COLOR, cv::FILLED);
            }

            apps.emplace_back(
                std::make_shared<ObjectDetection>(
                    ie, appConfig.video_sources[i], i, yoloParam.width, yoloParam.height,
                    destW, destH, posX, posY,
                    appConfig.pre_saved_frame_count[i]
                )
            );
            if(isCamera) apps.back()->SetLive(true);
            std::cout << *apps.back() << std::endl;
        }
        if(appConfig.is_fill_blank && !appConfig.is_expand_mode)
        {
            for(int i = (int)appConfig.video_sources.size();
                i < grid_cols * grid_rows; i++)
            {
                advanceToFreeCell();
                if(nextCellIdx >= grid_cols * grid_rows) break;
                cv::Rect cell = cellRectFor(nextCellIdx, false);
                nextCellIdx++;
                apps.emplace_back(
                    std::make_shared<ObjectDetection>(
                        ie, i,
                        cell.width, cell.height,
                        cell.x, cell.y
                    )
                );
            }
        }
    }


    std::function<int(std::vector<std::shared_ptr<dxrt::Tensor>>, void*)> postProcCallBack = \
        [&](std::vector<std::shared_ptr<dxrt::Tensor>> outputs, void* arg)
        {
            ObjectDetection *app = (ObjectDetection *)arg;
            app->PostProc(outputs);
            return 0;
        };
    ie->RegisterCallback(postProcCallBack);

    // ------------------------------------------------------------------
    // Stage profiler 초기화
    //   JSON "profile" 블록 또는 --profile CLI 로 활성화.
    //   CLI 가 JSON 보다 우선한다.
    // ------------------------------------------------------------------
    {
        dxprof::Config pcfg;
        pcfg.enabled     = (appConfig.profile_enabled != 0);
        pcfg.path        = appConfig.profile_path;
        pcfg.period_ms   = appConfig.profile_period_ms;
        pcfg.warmup_ms   = appConfig.profile_warmup_ms;
        pcfg.per_channel = (appConfig.profile_per_channel != 0);

        if(cmd.count("profile"))
        {
            pcfg.enabled = true;
            if(!profilePath.empty()) pcfg.path = profilePath;
        }
        if(cmd.count("profile_period")) pcfg.period_ms = std::max(200, profilePeriodMs);
        if(cmd.count("profile_warmup")) pcfg.warmup_ms = std::max(0, profileWarmupMs);

        if(pcfg.enabled && pcfg.path.empty())
        {
            // profile_<os>_<Nch>_<timestamp>.log
            std::time_t nowT = std::time(nullptr);
            char tb[32] = {0};
            std::strftime(tb, sizeof(tb), "%Y%m%d_%H%M%S", std::localtime(&nowT));
#ifdef _WIN32
            const char* osTag = "win";
#elif defined(__linux__)
            const char* osTag = "linux";
#else
            const char* osTag = "os";
#endif
            pcfg.path = std::string("profile_") + osTag + "_"
                      + std::to_string(appConfig.video_sources.size()) + "ch_" + tb + ".log";
        }

        if(pcfg.enabled)
        {
            std::ostringstream extra;
            extra << "--------------------------------------------------------------------------------\n"
                  << "[demo config]\n"
                  << "config.path        : " << configPath << "\n"
                  << "model.path         : " << appConfig.model_path << "\n"
                  << "model.name         : " << appConfig.model_name << "\n"
                  << "channels           : " << appConfig.video_sources.size() << "\n"
                  << "grid               : " << grid_cols << " x " << grid_rows << "\n"
                  << "cell               : " << divWidth << " x " << divHeight << "\n"
                  << "board              : " << BOARD_WIDTH << " x " << BOARD_HEIGHT
                  << " (title " << TITLE_HEIGHT << ")\n"
                  << "capture_period_ms  : " << appConfig.input_capture_period_ms << "\n"
                  << "display_fps        : " << appConfig.display_fps << "\n"
                  << "num_devices        : " << appConfig.num_devices << "\n";
            extra << "preload_frames     : ";
            for(size_t i = 0; i < appConfig.pre_saved_frame_count.size(); i++)
                extra << appConfig.pre_saved_frame_count[i] << " ";
            extra << "\n";
#ifdef DXRT_VERSION
            extra << "dxrt.version       : " << DXRT_VERSION << "\n";
#endif
#ifdef _WIN32
            extra << "timer.period_ms    : ";
            if(timerRes.Period() == 0) extra << "(요청 실패, 기본 15.6ms 틱)\n";
            else                       extra << timerRes.Period() << "\n";
#endif

            // preload 버퍼가 물리 메모리를 얼마나 쓰는지 추정.
            // PRELOAD 모드는 원본 해상도 프레임을 그대로 들고 있으므로
            // 채널 수 x 프레임 수 만큼 곱해져서 커진다.
            {
                double srcMB = 0.0, preMB = 0.0;
                for(size_t i = 0; i < apps.size() && i < appConfig.pre_saved_frame_count.size(); i++)
                {
                    int n = appConfig.pre_saved_frame_count[i];
                    if(n <= 0) continue;   // RUNTIME 모드는 프레임을 쌓지 않는다
                    auto res = apps[i]->SourceResolution();
                    srcMB += (double)n * res.first * res.second * 3.0 / (1024.0 * 1024.0);
                    preMB += (double)n * npuInputSize * npuInputSize * 3.0 / (1024.0 * 1024.0);
                }
                extra << "preload.src_mb     : " << std::fixed << std::setprecision(0) << srcMB
                      << "   (원본 해상도 프레임 버퍼)\n"
                      << "preload.npu_mb     : " << std::fixed << std::setprecision(0) << preMB
                      << "   (NPU 입력 프레임 버퍼)\n"
                      << "preload.total_mb   : " << std::fixed << std::setprecision(0) << (srcMB + preMB)
                      << "\n";
            }

            // --- NPU 디바이스 정보 (config 의 num_devices 가 아니라 실측) ---
            extra << "--------------------------------------------------------------------------------\n"
                  << "[npu devices]\n";
            int npuCount = 0;
            try { npuCount = dxrt::DeviceStatus::GetDeviceCount(); } catch(...) { npuCount = -1; }
            extra << "npu.count          : " << npuCount
                  << "   (config num_devices = " << appConfig.num_devices << ")\n";
            for(int d = 0; d < npuCount; d++)
            {
                try
                {
                    auto st = dxrt::DeviceStatus::GetCurrentStatus(d);
                    extra << "npu[" << d << "].board      : " << st.BoardTypeStr() << "\n"
                          << "npu[" << d << "].variant    : " << st.DeviceVariantStr() << "\n"
                          << "npu[" << d << "].type       : " << st.DeviceTypeStr() << "\n"
                          << "npu[" << d << "].memory     : " << st.MemoryTypeStr() << " "
                                                              << st.MemorySizeStrBinaryPrefix() << "\n"
                          << "npu[" << d << "].npu_clock  : " << st.NpuClock(0) << "\n"
                          << "npu[" << d << "].temp_c     : " << st.Temperature(0) << "\n"
                          << "npu[" << d << "].driver     : " << st.DriverVersionStr() << "\n"
                          << "npu[" << d << "].pcie       : " << st.PcieInfoStr() << "\n";
                    std::string info = st.GetInfoString();
                    if(!info.empty())
                        extra << "npu[" << d << "].status     :\n" << info << "\n";
                }
                catch(...)
                {
                    extra << "npu[" << d << "] : <query failed>\n";
                }
            }

            // --- config json 원본 전체 ---
            // 파생값만으로는 원래 설정을 복원할 수 없어서 원문을 그대로 남긴다.
            {
                std::ifstream cfgIfs(configPath);
                if(cfgIfs.is_open())
                {
                    std::string cfgTxt((std::istreambuf_iterator<char>(cfgIfs)),
                                        std::istreambuf_iterator<char>());
                    extra << "--------------------------------------------------------------------------------\n"
                          << "[config json: " << configPath << "]\n"
                          << cfgTxt << "\n";
                }
            }

            // 구간마다 NPU 온도/클럭을 샘플링한다.
            // 헤더에만 찍히면 스로틀링 추이를 볼 수 없다.
            if(npuCount > 0)
            {
                dxprof::Profiler::Instance().SetPeriodicSampler([npuCount]() -> std::string {
                    std::ostringstream o;
                    for(int d = 0; d < npuCount; d++)
                    {
                        try
                        {
                            auto st = dxrt::DeviceStatus::GetCurrentStatus(d);
                            // M1 은 디바이스 1개에 NPU 코어가 여러 개다.
                            // 코어 0 만 보면 다른 코어의 스로틀링을 놓친다.
                            o << " npu[" << d << "]";
                            for(int c = 0; c < 3; c++)
                            {
                                int t = st.Temperature(c);
                                if(t <= 0) break;
                                o << " c" << c << ":" << t << "'C/"
                                  << st.NpuClock(c) << "MHz/" << st.Voltage(c) << "mV";
                            }
                        }
                        catch(...) {}
                    }
                    return o.str();
                });
            }

            dxprof::Profiler::Instance().SetChannelCount((int)apps.size());
            dxprof::Profiler::Instance().Init(pcfg, dxprof::CollectEnvInfo(extra.str()));
        }
    }

#if !__riscv
    cv::namedWindow(DISPLAY_WINDOW_NAME, cv::WINDOW_NORMAL);
    if(appConfig.is_fullsize_mode)
    {
        cv::setWindowProperty(DISPLAY_WINDOW_NAME, cv::WND_PROP_FULLSCREEN, cv::WINDOW_FULLSCREEN);
    }
    else
    {
        cv::resizeWindow(DISPLAY_WINDOW_NAME, BOARD_WIDTH, BOARD_HEIGHT);
    }
    cv::moveWindow(DISPLAY_WINDOW_NAME, 0, 0);
    cv::setMouseCallback(DISPLAY_WINDOW_NAME, onMouseCallback, nullptr);
#endif

    for(auto &app:apps)
    {
        app->Run(appConfig.input_capture_period_ms);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    /* Debugging */
    std::vector<cv::Rect> dstPoint = std::vector<cv::Rect>(apps.size(), cv::Rect(0, 0, 0, 0));
    for(int i = 0; i < (int)apps.size(); i++)
    {
        dstPoint[i].x = apps[i]->Position().first;
        dstPoint[i].y = apps[i]->Position().second;
        dstPoint[i].width = apps[i]->Resolution().first;
        dstPoint[i].height = apps[i]->Resolution().second;
    }
    dxdemo::common::StatusLog sl;
    sl.period = 500, sl.threadStatus.store(0);
    std::thread log_thread = std::thread(&dxdemo::common::logThreadFunction, &sl);
    auto start = std::chrono::high_resolution_clock::now();
    long long duration = 0;
    long long passTime = 0;
    std::deque<std::pair<std::chrono::time_point<std::chrono::high_resolution_clock>, uint64_t>> timestampedCounts;
    bool calcStarted = false;
    std::vector<uint64_t> lastProcessedCounts;

    // CPU/NPU 시스템 모니터 표시 토글 ('m' 키). 기본 off.
    bool showSysStats = false;

    // --- Display FPS throttle ---
    // display_fps=0 means unlimited; otherwise cap imshow to N fps.
    const int display_period_ms = (appConfig.display_fps > 0)
        ? std::max(1, 1000 / appConfig.display_fps)
        : 1;

    // --- Console FPS log ---
    // 터미널에 주기적으로 FPS를 출력 (Windows 기본 1초, 0이면 비활성).
    const int console_fps_period_ms = appConfig.console_fps_period_ms;
    auto lastFpsLog = std::chrono::steady_clock::now();
    if(console_fps_period_ms > 0)
    {
        std::cout << "[FPS] console log enabled (every " << console_fps_period_ms
                  << " ms, avg window " << window_size << " s)" << std::endl;
    }

    // Per-app dirty tracking: only copyTo when the app produced a new frame.
    std::vector<uint64_t> lastRenderCounts(apps.size(), UINT64_MAX);

    while(true)
    {
        auto renderStart = std::chrono::steady_clock::now();
        DXPROF_MARK(0, dxprof::ST_MAIN_LOOP);       // 렌더 루프 주기
        dxprof::Stopwatch _pfMainBusy;
        frameCount = 0.1;
        float resultFps = 0.f;

        {
            DXPROF_SCOPE(0, dxprof::ST_COMPOSE);
            for(int i = 0; i < (int)apps.size(); i++)
            {
                uint64_t cnt = apps[i]->GetPostProcessCount();
                if(cnt != lastRenderCounts[i])
                {
                    cv::Mat roi = outFrame(dstPoint[i]);
                    apps[i]->ResultFrame().copyTo(roi);
                    lastRenderCounts[i] = cnt;
                }
            }
        }

        allFrameCount++;

        dxprof::Stopwatch _pfFps;
        if(calcFps)
        {
            uint64_t checkSum = 0;  // int에서 uint64_t로 변경하여 오버플로우 방지
            for(int i = 0; i < (int)appConfig.video_sources.size(); i++)
            {
                uint64_t currentCount = apps[i]->GetPostProcessCount();
                if(calcStarted && i < (int)lastProcessedCounts.size())
                {
                    // 이전 측정값과의 차이만 계산 (delta 방식)
                    uint64_t delta = (currentCount > lastProcessedCounts[i]) ?
                                    (currentCount - lastProcessedCounts[i]) : 0;
                    // 오버플로우 체크 추가
                    if(checkSum > UINT64_MAX - delta) {
                        std::cerr << "Warning: checkSum overflow detected, resetting..." << std::endl;
                        checkSum = delta;  // 오버플로우 시 현재 delta만 사용
                    } else {
                        checkSum += delta;
                    }
                }
                // 현재 카운트를 저장 (다음 측정을 위해)
                if(i >= (int)lastProcessedCounts.size())
                    lastProcessedCounts.resize(i + 1);
                lastProcessedCounts[i] = currentCount;
            }

            // 현재 시간과 함께 프레임 카운트 저장
            auto now = std::chrono::high_resolution_clock::now();
            timestampedCounts.push_back({now, checkSum});

            // window_size 초보다 오래된 데이터 제거 (오버플로우 방지)
            if(window_size > 0 && window_size < LLONG_MAX / 1000) {
                auto cutoff = now - std::chrono::milliseconds(static_cast<long long>(window_size * 1000));
                while(!timestampedCounts.empty() && timestampedCounts.front().first < cutoff)
                {
                    timestampedCounts.pop_front();
                }
            }
        }


        auto end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration_cast<std::chrono::milliseconds>(end-start).count();
        if(passTime != -1) passTime = duration;
        if(passTime > 1000 && calcFps == false)
        {
            calcFps = true;
            calcStarted = true;
            passTime = 0;
            start = std::chrono::high_resolution_clock::now();
            // 초기 카운트 값들을 저장
            lastProcessedCounts.resize(appConfig.video_sources.size());
            for(int i = 0; i < (int)appConfig.video_sources.size(); i++)
            {
                lastProcessedCounts[i] = apps[i]->GetPostProcessCount();
            }
        }

        // 프레임 카운트 집계
        frameCount = 0.0;
        for(const auto& entry : timestampedCounts)
        {
            frameCount += entry.second;
        }

        if(calcFps && calcStarted)
        {
            if(!timestampedCounts.empty())
            {
                if(timestampedCounts.size() > 1)
                {
                    // 첫 번째와 마지막 타임스탬프 간의 실제 시간 간격 계산
                    auto timeSpan = std::chrono::duration_cast<std::chrono::milliseconds>(
                        timestampedCounts.back().first - timestampedCounts.front().first).count();

                    if(timeSpan > 0)
                    {
                        // 실제 시간 간격 기반으로 FPS 계산
                        resultFps = (frameCount * 1000.0) / timeSpan;
                    }
                    else
                    {
                        // 시간 간격이 0이면 현재 프레임 카운트만 사용
                        resultFps = frameCount;
                    }
                }
                else
                {
                    // 단일 샘플인 경우
                    resultFps = frameCount;
                }
            }
            else
            {
                resultFps = 0.0f;
            }
        }

        DXPROF_ADD(0, dxprof::ST_FPS_CALC, _pfFps.ElapsedUs());

        if(console_fps_period_ms > 0)
        {
            auto nowLog = std::chrono::steady_clock::now();
            if(std::chrono::duration_cast<std::chrono::milliseconds>(nowLog - lastFpsLog).count()
                    >= console_fps_period_ms)
            {
                lastFpsLog = nowLog;
                int activeStreams = (int)appConfig.video_sources.size();
                if(!calcFps)
                {
                    std::cout << "[FPS] measuring..." << std::endl;
                }
                else
                {
                    float avgFps = (activeStreams > 0) ? (resultFps / activeStreams) : 0.f;
                    std::cout << "[FPS] elapsed " << (duration / 1000) << "s"
                              << " | Total " << fpsValueText(resultFps)
                              << " | AVG " << fpsValueText(avgFps)
                              << " (" << activeStreams << " ch)" << std::endl;
                }
            }
        }

#ifndef _WIN32
        // Windows: 헤더 패널이 깨져 표시되므로 그리지 않음 (TITLE_HEIGHT == 0)
        {
        DXPROF_SCOPE(0, dxprof::ST_HUD);
        renderHeaderHud(outFrame,
                        BOARD_WIDTH, TITLE_HEIGHT,
                        (int)appConfig.video_sources.size(),
                        appConfig.num_devices,
                        resultFps,
                        appConfig.is_show_fps,
                        calcFps,
                        appConfig.model_name,
                        appConfig.fps_value_font_scale);
        }
#endif
        sl.frameNumber = std::min(allFrameCount, (uint64_t)UINT_MAX);  // 오버플로우 방지
        sl.runningTime = duration;
        if (loggingVersion)
            sl.threadStatus.store(2);
        else
            sl.threadStatus.store(1);

        sl.statusCheckCV.notify_one();

#if __riscv
        std::cout << "press 'q' and enter to exit. " << std::endl;
        int key = getchar();
#else
        {
            DXPROF_SCOPE(0, dxprof::ST_IMSHOW);
            cv::imshow(DISPLAY_WINDOW_NAME, outFrame);
        }
        int key = 0;
        {
            DXPROF_SCOPE(0, dxprof::ST_WAITKEY);
            key = cv::waitKey(1);
        }

        auto renderElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - renderStart).count();
        int sleepMs = display_period_ms - (int)renderElapsed;
        DXPROF_ADD(0, dxprof::ST_MAIN_BUSY, _pfMainBusy.ElapsedUs());
        DXPROF_ADD(0, dxprof::ST_MAIN_SLEEP_REQ, (uint64_t)(sleepMs > 0 ? sleepMs : 0) * 1000);
        if(sleepMs > 0)
        {
            dxprof::Stopwatch _pfSleep;
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
            DXPROF_ADD(0, dxprof::ST_MAIN_SLEEP_ACT, _pfSleep.ElapsedUs());
        }
#endif
        bool windowClosed = false;
#if !__riscv
        {
        DXPROF_SCOPE(0, dxprof::ST_WINPROP);
        try {
            windowClosed = (cv::getWindowProperty(DISPLAY_WINDOW_NAME, cv::WND_PROP_VISIBLE) < 1);
        } catch(const cv::Exception&) {
            windowClosed = true;
        }
        }
#endif
        if(key == 0x1B || key == 0x71 || g_exitRequested || windowClosed) //'ESC' or 'q' or window 'X'
        {
            sl.threadStatus.store(-1);
            // 프로파일러를 먼저 닫아 워커 스레드 종료 지연이 통계에 섞이지 않게 한다.
            dxprof::Profiler::Instance().Shutdown();
            for(auto &app:apps)
            {
                app->Stop();
            }
            log_thread.join();
            // 콘솔 FPS 로그가 켜져 있으면(Windows 기본) 종료 시 최종 요약을 출력한다.
            if(console_fps_period_ms > 0)
            {
                int activeStreams = (int)appConfig.video_sources.size();
                float avgFps = (activeStreams > 0) ? (resultFps / activeStreams) : 0.f;
                std::cout << "\n========================================\n"
                          << " Final FPS Summary\n"
                          << "   Total FPS : " << fpsValueText(resultFps) << "\n"
                          << "   AVG FPS   : " << fpsValueText(avgFps)
                          << "  (over " << activeStreams << " channels)\n"
                          << "========================================" << std::endl;
            }
            break;
        }
        else if(key == 0x74) // 't'
        {
            for(auto &app:apps)
            {
                app->Toggle();
            }
        }
        else if(key == 0x6D) // 'm' : CPU LOAD / NPU TEMP 표시 토글
        {
            showSysStats = !showSysStats;
        }

    }
    dxprof::Profiler::Instance().Shutdown();   // 비정상 경로 대비 (idempotent)
#ifdef __linux__
    sleep(1);
#elif _WIN32
    Sleep(1000);
#endif
DXRT_TRY_CATCH_END
    return 0;
}
