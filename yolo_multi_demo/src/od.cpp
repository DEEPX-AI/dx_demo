#include "od.h"
#include "yolo.h"
#include "profiler.h"
#include <utils/common_util.hpp>
#include <algorithm>

#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#endif

extern YoloParam yoloParam;

#ifdef __linux__
namespace {
struct CamMode { int w, h, fps; uint32_t pixfmt; };

// Picks the cheapest V4L2 mode of /dev/videoN that is still good enough for
// inference, preferring: >=640x360 at >=15 fps, then smaller area (less USB
// bandwidth and decode), then YUYV over MJPG (no JPEG decode).
static bool selectOptimalCameraMode(const std::string& devPath, CamMode& out)
{
    int fd = ::open(devPath.c_str(), O_RDWR | O_NONBLOCK);
    if(fd < 0) return false;

    std::vector<CamMode> modes;
    v4l2_fmtdesc fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    for(fmt.index = 0; ioctl(fd, VIDIOC_ENUM_FMT, &fmt) == 0; fmt.index++)
    {
        if(fmt.pixelformat != V4L2_PIX_FMT_YUYV
           && fmt.pixelformat != V4L2_PIX_FMT_MJPEG)
            continue;
        v4l2_frmsizeenum fs{};
        fs.pixel_format = fmt.pixelformat;
        for(fs.index = 0; ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs) == 0; fs.index++)
        {
            if(fs.type != V4L2_FRMSIZE_TYPE_DISCRETE) continue;
            int w = (int)fs.discrete.width;
            int h = (int)fs.discrete.height;
            v4l2_frmivalenum fi{};
            fi.pixel_format = fmt.pixelformat;
            fi.width = w; fi.height = h;
            int bestFps = 0;
            for(fi.index = 0; ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &fi) == 0; fi.index++)
            {
                if(fi.type != V4L2_FRMIVAL_TYPE_DISCRETE) continue;
                unsigned int n = std::max((unsigned int)1, fi.discrete.numerator);
                int fps = (int)(fi.discrete.denominator / n);
                if(fps > bestFps) bestFps = fps;
            }
            if(bestFps > 0) modes.push_back({w, h, bestFps, fmt.pixelformat});
        }
    }
    ::close(fd);
    if(modes.empty()) return false;

    std::sort(modes.begin(), modes.end(), [](const CamMode& a, const CamMode& b){
        bool aOK = (a.w >= 640 && a.h >= 360 && a.fps >= 15);
        bool bOK = (b.w >= 640 && b.h >= 360 && b.fps >= 15);
        if(aOK != bOK) return aOK;
        long aA = (long)a.w * a.h, bA = (long)b.w * b.h;
        if(aA != bA) return aA < bA;
        if(a.pixfmt != b.pixfmt) return a.pixfmt == V4L2_PIX_FMT_YUYV;
        return a.fps > b.fps;
    });
    out = modes.front();
    return true;
}
} // namespace
#endif

ObjectDetection::ObjectDetection(std::shared_ptr<dxrt::InferenceEngine> ie, std::pair<std::string, std::string> &videoSrc, int channel, 
        int width, int height, int destWidth, int destHeight,
        int posX, int posY, int numFrames)
: _ie(ie), _channel(channel + 1),
    _width(width), _height(height), _destWidth(destWidth), _destHeight(destHeight), 
    _posX(posX), _posY(posY), _videoSrc(videoSrc)
{
    AppInputType inputType = AppInputType::VIDEO;
    if(_videoSrc.second == "camera")
        inputType = AppInputType::CAMERA;
    else if(_videoSrc.second == "camera_image")
        inputType = AppInputType::IMAGE;        // camera-highlight UI test
    else if(_videoSrc.second == "camera_video")
        inputType = AppInputType::VIDEO;        // camera-highlight UI test
    else if(_videoSrc.second == "image")
        inputType = AppInputType::IMAGE;
    else if(_videoSrc.second == "rtsp")
        inputType = AppInputType::RTSP;
#if __riscv
    else if(_videoSrc.second == "isp")
        inputType = AppInputType::ISP;
#endif
    else
        inputType = AppInputType::VIDEO;
    auto inputShape = _ie->GetInputs().front().shape();
    auto npuShape = dxdemo::common::Size((int)inputShape[1],(int)inputShape[1]);
    auto dstShape = dxdemo::common::Size(_destWidth, _destHeight);

    int camW = 1280, camH = 720, camFps = 30;
#ifdef __linux__
    if(inputType == AppInputType::CAMERA
       && _videoSrc.first.find("/dev/video") != std::string::npos)
    {
        CamMode m;
        if(selectOptimalCameraMode(_videoSrc.first, m))
        {
            camW = m.w; camH = m.h;
            camFps = std::min(30, m.fps);
            std::cout << "[Camera] Auto-selected mode for " << _videoSrc.first
                      << ": " << camW << "x" << camH << " @ " << camFps << "fps "
                      << (m.pixfmt == V4L2_PIX_FMT_YUYV ? "(YUYV)" : "(MJPG)")
                      << std::endl;
        }
    }
#endif

    _vStream = VideoStream(inputType, _videoSrc.first, numFrames, npuShape, AppInputFormat::IMAGE_BGR, dstShape, _ie, camFps, camW, camH);
    auto srcShape = _vStream._srcSize;
    _srcWidth = srcShape._width;
    _srcHeight = srcShape._height;
    _name = "app" + std::to_string(_channel);
    dxdemo::common::Size_f _postprocRatio;
    _postprocRatio._width = (float)dstShape._width/srcShape._width;
    _postprocRatio._height = (float)dstShape._height/srcShape._height;

    float _preprocRatio = std::min((float)npuShape._width/srcShape._width, (float)npuShape._height/srcShape._height);
    
    if(srcShape == npuShape)
    {
        _postprocPaddedSize._width = 0.f;
        _postprocPaddedSize._height = 0.f;
    }
    else
    {
        dxdemo::common::Size resizeShpae((int)(srcShape._width * _preprocRatio), (int)(srcShape._height * _preprocRatio));
        _postprocPaddedSize._width = (npuShape._width - resizeShpae._width) / 2.f;
        _postprocPaddedSize._height = (npuShape._height - resizeShpae._height) / 2.f;
    }

    _postprocScaleRatio = dxdemo::common::Size_f(_postprocRatio._width/_preprocRatio, _postprocRatio._height/_preprocRatio);
    
    _resultFrame = cv::Mat(_destHeight, _destWidth, CV_8UC3, cv::Scalar(0, 0, 0));
    _displayFrame = cv::Mat(_destHeight, _destWidth, CV_8UC3, cv::Scalar(0, 0, 0));
    yolo = Yolo(yoloParam);
    if(!yolo.LayerReorder(_ie->GetOutputs()))
        return;

    outputMemory = (uint8_t*)operator new(_ie->GetOutputSize());
    output_length = 0;
    for(auto &o:_ie->GetOutputs())
    {
        output_shape.emplace_back(o.shape());
    }
    data_type = _ie->GetOutputs().front().type();

}
ObjectDetection::ObjectDetection(std::shared_ptr<dxrt::InferenceEngine> ie, int channel, int destWidth, int destHeight, int posX, int posY)
: _ie(ie), _channel(channel+1), _destWidth(destWidth), _destHeight(destHeight), _posX(posX), _posY(posY)
{
    _name = "app" + std::to_string(_channel);
    if(dxdemo::common::pathValidation("./sample/dx_colored_logo.png"))
    {
        _logo = cv::imread("./sample/dx_colored_logo.png", cv::IMREAD_COLOR);
        cv::resize(_logo, _resultFrame, cv::Size(_destWidth, _destHeight), 0, 0, cv::INTER_LINEAR);
        _displayFrame = _resultFrame.clone();
    }
    else
    {
        _resultFrame = cv::Mat(_destHeight, _destWidth, CV_8UC3, cv::Scalar(0, 0, 0));
        _displayFrame = cv::Mat(_destHeight, _destWidth, CV_8UC3, cv::Scalar(0, 0, 0));
    }
    outputMemory = nullptr;
}
ObjectDetection::~ObjectDetection() {
    if(outputMemory)
        operator delete(outputMemory);
}
dxdemo::common::DetectObject ObjectDetection::GetScalingBBox(std::vector<BoundingBox>& bboxes)
{
    dxdemo::common::DetectObject result;
    result._num_of_detections = bboxes.size();
    for (auto& b : bboxes)
    {
        dxdemo::common::BBox box;
        box._xmin = (b.box[0] - _postprocPaddedSize._width) * _postprocScaleRatio._width;
        box._ymin = (b.box[1] - _postprocPaddedSize._height) * _postprocScaleRatio._height;
        box._xmax = (b.box[2] - _postprocPaddedSize._width) * _postprocScaleRatio._width;
        box._ymax = (b.box[3] - _postprocPaddedSize._height) * _postprocScaleRatio._height;
        box._width = (b.box[2] - b.box[0]) * _postprocScaleRatio._width;
        box._height = (b.box[3] - b.box[1]) * _postprocScaleRatio._height;
        box._kpts.emplace_back(dxdemo::common::Point_f(-1 , -1, -1));
    
        dxdemo::common::Object object;
        object._bbox = box;
        object._conf = b.score;
        object._classId = b.label;
        object._name = b.labelname;
        result._detections.emplace_back(object);
    }
    return result;
}
void ObjectDetection::threadFunc(int period)
{
    std::chrono::high_resolution_clock::time_point _cap_start, _proc_start;
    cv::Mat member_temp;
    while(1)
    {        
        if(stop) break;
        // Interval between iterations = this channel's real frame period.
        DXPROF_MARK(_channel, dxprof::ST_WORKER_LOOP);
        dxprof::Stopwatch _pfBusy;
        _proc_start = std::chrono::high_resolution_clock::now();
        _cap_start = std::chrono::high_resolution_clock::now();
        void* input = nullptr;
        {
            DXPROF_SCOPE(_channel, dxprof::ST_GET_INPUT);
            input = _vStream.GetInputStream();
        }
        {
            DXPROF_SCOPE(_channel, dxprof::ST_RUN_ASYNC);
            std::ignore = _ie->RunAsync(input, (void*)this, (void*)outputMemory);
        }
        std::vector<BoundingBox> bboxes;
        dxdemo::common::DetectObject bboxes_objects;
        {
            DXPROF_SCOPE(_channel, dxprof::ST_BBOX_SCALE);
            std::unique_lock<std::mutex> lk(_lock);
            if(!_bboxes.empty() && _toggleDrawing)
            {
                bboxes = std::vector<BoundingBox>(_bboxes);
                bboxes_objects = GetScalingBBox(bboxes);
            }
        }
        {
            DXPROF_SCOPE(_channel, dxprof::ST_GET_OUTPUT);
            member_temp = _vStream.GetOutputStream(bboxes_objects);
        }
            
        {
            DXPROF_SCOPE(_channel, dxprof::ST_BADGE);
            const int frameW = member_temp.cols;
            const int frameH = member_temp.rows;

            std::string chLabel = "CH " + std::to_string(_channel);

            int badgeH = std::max(22, (int)(frameH * 0.055));
            double fontScale = badgeH * 0.028;
            int textThick = std::max(1, (int)(badgeH / 14));
            int baseLine = 0;
            cv::Size textSize = cv::getTextSize(chLabel, cv::FONT_HERSHEY_DUPLEX,
                                                fontScale, textThick, &baseLine);

            int padX = std::max(8, badgeH / 2);
            int accentW = std::max(3, badgeH / 8);
            int badgeW = accentW + padX + textSize.width + padX;
            int margin = std::max(6, (int)(frameH * 0.012));

            badgeW = std::min(badgeW, std::max(0, frameW - margin * 2));
            badgeH = std::min(badgeH, std::max(0, frameH - margin * 2));
            if(badgeW > 0 && badgeH > 0)
            {
                cv::Rect badgeRect(margin, margin, badgeW, badgeH);

                cv::rectangle(member_temp, badgeRect, cv::Scalar(0, 0, 0), cv::FILLED);

                cv::rectangle(member_temp,
                              cv::Rect(badgeRect.x, badgeRect.y, accentW, badgeRect.height),
                              cv::Scalar(0, 0, 255), cv::FILLED);

                cv::Point textOrg(badgeRect.x + accentW + padX,
                                  badgeRect.y + (badgeRect.height + textSize.height) / 2 - 1);
                cv::putText(member_temp, chLabel, textOrg,
                            cv::FONT_HERSHEY_DUPLEX, fontScale,
                            cv::Scalar(255, 255, 255), textThick, cv::LINE_AA);

            }
        }

        
        _inferenceTime = _ie->GetNpuInferenceTime();
        _latencyTime = _ie->GetLatency();

        // dxrt occasionally returns a bogus value (~1,249 s samples observed),
        // which would poison the interval average and the running max. Route
        // those to npu.anomaly instead of dropping them, so they stay visible.
        const uint64_t kNpuTimeSaneUs = 1000000;   // 1 s; normal is ~10 ms
        if(_inferenceTime < kNpuTimeSaneUs)
            DXPROF_ADD(_channel, dxprof::ST_NPU_INFER, _inferenceTime);
        else
            DXPROF_ADD(_channel, dxprof::ST_NPU_ANOMALY, _inferenceTime);

        if(_latencyTime < kNpuTimeSaneUs)
            DXPROF_ADD(_channel, dxprof::ST_NPU_LATENCY, _latencyTime);
        else
            DXPROF_ADD(_channel, dxprof::ST_NPU_ANOMALY, _latencyTime);
        
        int64_t cap_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now() - _cap_start).count();
        int64_t t = (period*1000 - cap_us)/1000;
        if(t<0 || t>period) t = 0;
        
        if(_processed_count > 0)
        {
            dxprof::ScopedTimer _pfSwap(_channel, dxprof::ST_FRAME_SWAP);
            std::unique_lock<std::mutex> lk(_frameLock);
            cv::swap(member_temp, _resultFrame);
            _pfSwap.Stop();   // keep pause waiting out of the statistics
            if(_isPause){
                _cv.wait(lk, [this]{return !_isPause;});
            }
        }

        _processTime = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now() - _proc_start).count();

        // Requested vs actual sleep, which exposes the Windows 15.6 ms timer
        // tick making Sleep(1) take 15 ms or more.
        DXPROF_ADD(_channel, dxprof::ST_WORKER_BUSY, _pfBusy.ElapsedUs());
        DXPROF_ADD(_channel, dxprof::ST_WORKER_SLEEP_REQ, (uint64_t)t * 1000);
        dxprof::Stopwatch _pfSleep;
#ifdef __linux__
        usleep(t*1000);
#elif _WIN32
        Sleep(t);
#endif
        DXPROF_ADD(_channel, dxprof::ST_WORKER_SLEEP_ACT, _pfSleep.ElapsedUs());
    }
    std::cout << _channel << " ended." << std::endl;
}
void ObjectDetection::threadFillBlank(int period)
{
    while(1)
    {        
        if(stop) break;
#ifdef __linux__
        usleep(period * 1000);
#elif _WIN32
        Sleep(period);
#endif
    }
    std::cout << _channel << " ended." << std::endl;
}
void ObjectDetection::Run(int period)
{
    stop = false;
    if(_videoSrc.first.empty())
        _thread = std::thread(&ObjectDetection::threadFillBlank, this, period);
    else
        _thread = std::thread(&ObjectDetection::threadFunc, this, period);
}
void ObjectDetection::Stop()
{
    stop = true;
    _thread.join();
}
void ObjectDetection::Pause()
{
    std::unique_lock<std::mutex> lk(_frameLock);
    if(!_isPause)
        _isPause = true;
}
void ObjectDetection::Play()
{
    std::unique_lock<std::mutex> lk(_frameLock);
    if(_isPause){
        _isPause = false;
        _cv.notify_all();
    }
}
cv::Mat ObjectDetection::ResultFrame()
{
    std::unique_lock<std::mutex> lk(_frameLock);
    _resultFrame.copyTo(_displayFrame);
    return _displayFrame;
}
std::pair<int, int> ObjectDetection::Position()
{
    return std::make_pair(_posX, _posY);
}
std::pair<int, int> ObjectDetection::Resolution()
{
    return std::make_pair(_destWidth, _destHeight);
}
uint64_t ObjectDetection::GetLatencyTime()
{
    return _latencyTime;
}
uint64_t ObjectDetection::GetInferenceTime()
{
    return _inferenceTime;
}
uint64_t ObjectDetection::GetProcessingTime()
{
    return _processTime;
}
int ObjectDetection::Channel()
{
    return _channel;
}
std::string &ObjectDetection::Name()
{
    return _name;
}
void ObjectDetection::Toggle()
{
    _toggleDrawing = !_toggleDrawing;
}
void ObjectDetection::PostProc(std::vector<std::shared_ptr<dxrt::Tensor>> &outputs)
{
    // Interval between callbacks = the rate this channel actually gets
    // results, i.e. its FPS.
    DXPROF_MARK(_channel, dxprof::ST_POSTPROC_GAP);
    {
        dxprof::ScopedTimer _pfWait(_channel, dxprof::ST_POSTPROC_WAIT);
        std::unique_lock<std::mutex> lk(_lock);
        _pfWait.Stop();
        DXPROF_SCOPE(_channel, dxprof::ST_POSTPROC);
        _bboxes = yolo.PostProc(outputs);
    }
    _processed_count++;
    _ret_processed_count++;
}
uint64_t ObjectDetection::GetPostProcessCount()
{
    return _ret_processed_count.load();
}
std::ostream& operator<<(std::ostream& os, const ObjectDetection& od)
{
    os << od._name << ": " << od._channel << ", "
        << od._videoSrc.first << ", " << od._videoSrc.second << ", "
        << od._width << "x" << od._height << " -> "
        << od._destWidth << "x" << od._destHeight
        << " @(" << od._posX << "," << od._posY << ")";
    return os;
}
