#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <dlfcn.h>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include "font.h"
#include "jvm_runtime.h"
#include "menu_ui.h"
#include "overlay_canvas.h"
#include "overlay_draw.h"
#include "overlay_surface.h"
#include "raw_capture.h"
#include "launch_args.h"
#include "touchproxy.h"

namespace {

constexpr int kTouchPointerId = 0;
constexpr int kDefaultTargetFps = 120;
constexpr int kFrameIntervalUs = 1000000 / kDefaultTargetFps;
constexpr int kDisplayPollIntervalMs = 500;
constexpr int kOverlayIntervalMs = 50;
constexpr const char* kRuntimeRootDir = "/data/local/tmp/com.enenh.ai";
constexpr const char* kRuntimeLibDir = "/data/local/tmp/com.enenh.ai/lib";
constexpr const char* kQnnCacheDir = "/data/local/tmp/com.enenh.ai/qnn_cache";
constexpr const char* kPidFilePath = "/data/local/tmp/com.enenh.ai/kaiagentd.pid";
constexpr const char* kLogFilePath = "/data/local/tmp/com.enenh.ai/kaiagentd.log";
constexpr const char* kDevNullPath = "/dev/null";
constexpr int kTouchAimMinIntervalMs = 8;
constexpr int kTouchOvershootSettleMs = 24;
constexpr float kTouchDeadzonePx = 2.00f;
constexpr float kTouchDeadzoneReleaseScale = 1.00f;
constexpr float kTouchDeadzoneIdleExitScale = 1.60f;
constexpr float kTouchAnchorXRatio = 0.60f;
constexpr float kTouchAnchorYRatio = 0.50f;
constexpr float kTouchDragScale = 0.022f;
constexpr float kTouchWarmupDragScale = 0.012f;
constexpr float kTouchXAxisScale = 1.00f;
constexpr float kTouchYAxisScale = 1.00f;
constexpr float kTouchMaxDistanceRatio = 0.012f;
constexpr float kTouchSnapEnterPx = 48.0f;
constexpr float kTouchSnapExitPx = 24.0f;
constexpr float kTouchSnapGain = 0.045f;
constexpr float kTouchSnapGainScale = 1.60f;
constexpr float kTouchSnapMaxDistanceRatio = 0.014f;
constexpr float kTouchDivergeBrakePx = 10.0f;
constexpr float kTouchDivergeBrakeRatio = 0.25f;
constexpr float kTouchFineDistanceRatio = 0.025f;
constexpr float kTouchBrakeDistanceRatio = 0.014f;
constexpr float kTouchFineGainScale = 0.22f;
constexpr float kTouchOvershootGainScale = 0.08f;
constexpr float kTouchMinDragPx = 0.0f;
constexpr float kTouchOvershootFilterRatio = 0.20f;
constexpr float kTouchSmoothAlpha = 0.90f;
constexpr int kTouchWarmupFrames = 3;
constexpr int kTargetHoldFrames = 8;
constexpr int kTargetHeadHoldFrames = 6;
constexpr int kTargetAcquireFrames = 2;
constexpr int kTargetReacquireCooldownFrames = 0;
constexpr int kTargetDiagnosticIntervalMs = 500;
constexpr int kTargetGroupSwitchFrames = 4;
constexpr int kTargetLastLostMaxFrames = 30;
constexpr int kTargetBodyClass = 0;
constexpr int kTargetHeadClass = 1;
constexpr float kTargetAcquireMinConfidence = 0.35f;
constexpr float kTargetTrackMinConfidence = 0.30f;
constexpr float kTargetGateRatio = 0.12f;
constexpr float kTargetCenterWeight = 0.12f;
constexpr float kTargetLastLostWeight = 0.20f;
constexpr float kTargetSmoothAlpha = 0.28f;
constexpr float kTargetBodyFallbackPenaltyRatio = 0.05f;
constexpr float kTargetContainToleranceRatio = 0.005f;
constexpr float kTargetGroupSwitchDistanceRatio = 0.55f;
constexpr float kConfThreshold = 0.25f;
constexpr float kIouThreshold = 0.45f;
constexpr int kNumClasses = 6;
constexpr uint32_t kBoxColor = rgba(255, 80, 80, 245);
constexpr uint32_t kTargetColor = rgba(255, 80, 80, 245);
constexpr uint32_t kTextColor = rgba(255, 255, 255, 235);

struct TouchTuning {
    int aimMinIntervalMs = kTouchAimMinIntervalMs;
    int overshootSettleMs = kTouchOvershootSettleMs;
    int warmupFrames = kTouchWarmupFrames;
    float deadzonePx = kTouchDeadzonePx;
    float deadzoneReleaseScale = kTouchDeadzoneReleaseScale;
    float dragScale = kTouchDragScale;
    float warmupDragScale = kTouchWarmupDragScale;
    float xAxisScale = kTouchXAxisScale;
    float yAxisScale = kTouchYAxisScale;
    float maxDistanceRatio = kTouchMaxDistanceRatio;
    float fineDistanceRatio = kTouchFineDistanceRatio;
    float brakeDistanceRatio = kTouchBrakeDistanceRatio;
    float fineGainScale = kTouchFineGainScale;
    float overshootGainScale = kTouchOvershootGainScale;
    float minDragPx = kTouchMinDragPx;
    float overshootFilterRatio = kTouchOvershootFilterRatio;
    float smoothAlpha = kTouchSmoothAlpha;
};

enum class TouchAimMode {
    Idle,
    Normal,
    Snap,
    Fine,
    Deadzone,
    Brake
};

struct TouchAimStatus {
    bool valid = false;
    TouchAimMode mode = TouchAimMode::Idle;
    float rawDist = 0.0f;
    float dist = 0.0f;
    float drag = 0.0f;
    float gain = 0.0f;
    int anchorX = 0;
    int anchorY = 0;
};

const char* touchAimModeLabel(TouchAimMode mode) {
    switch (mode) {
        case TouchAimMode::Normal:
            return "普通";
        case TouchAimMode::Snap:
            return "snap";
        case TouchAimMode::Fine:
            return "近距";
        case TouchAimMode::Deadzone:
            return "死区";
        case TouchAimMode::Brake:
            return "刹车";
        default:
            return "空闲";
    }
}

void resetTouchTuning(TouchTuning* tuning) {
    if (tuning) {
        *tuning = TouchTuning {};
    }
}

void clampTouchTuning(TouchTuning* tuning) {
    if (!tuning) {
        return;
    }
    tuning->dragScale = std::clamp(tuning->dragScale, 0.005f, 0.400f);
    tuning->warmupDragScale = std::clamp(tuning->warmupDragScale, 0.005f, 0.400f);
    tuning->xAxisScale = std::clamp(tuning->xAxisScale, 0.05f, 1.50f);
    tuning->yAxisScale = std::clamp(tuning->yAxisScale, 0.05f, 1.50f);
    tuning->deadzonePx = std::clamp(tuning->deadzonePx, 0.5f, 20.0f);
    tuning->deadzoneReleaseScale = std::clamp(tuning->deadzoneReleaseScale, 1.0f, 5.0f);
    tuning->smoothAlpha = std::clamp(tuning->smoothAlpha, 0.05f, 1.00f);
    tuning->minDragPx = std::clamp(tuning->minDragPx, 0.0f, 20.0f);
    tuning->maxDistanceRatio = std::clamp(tuning->maxDistanceRatio, 0.001f, 0.300f);
    tuning->fineDistanceRatio = std::clamp(tuning->fineDistanceRatio, 0.001f, 0.300f);
    tuning->brakeDistanceRatio = std::clamp(tuning->brakeDistanceRatio, 0.001f, 0.300f);
    tuning->fineGainScale = std::clamp(tuning->fineGainScale, 0.10f, 1.50f);
    tuning->overshootGainScale = std::clamp(tuning->overshootGainScale, 0.05f, 1.00f);
    tuning->overshootFilterRatio = std::clamp(tuning->overshootFilterRatio, 0.001f, 0.300f);
    tuning->aimMinIntervalMs = std::clamp(tuning->aimMinIntervalMs, 0, 400);
    tuning->overshootSettleMs = std::clamp(tuning->overshootSettleMs, 0, 300);
    tuning->warmupFrames = std::clamp(tuning->warmupFrames, 0, 10);
}

std::atomic<bool> g_stop(false);

void onSignal(int) {
    g_stop.store(true);
}

void logf(const char* level, const char* fmt, ...) {
#ifndef NDEBUG
    timespec ts {};
    clock_gettime(CLOCK_REALTIME, &ts);
    tm local {};
    localtime_r(&ts.tv_sec, &local);
    fprintf(stderr, "%02d:%02d:%02d.%03ld %s ",
            local.tm_hour, local.tm_min, local.tm_sec, ts.tv_nsec / 1000000L, level);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
#else
    (void)level;
    (void)fmt;
#endif
}

bool ensureRuntimeRoot() {
    if (mkdir(kRuntimeRootDir, 0755) == 0) {
        return true;
    }
    if (errno != EEXIST) {
        return false;
    }
    struct stat st {};
    return stat(kRuntimeRootDir, &st) == 0 && S_ISDIR(st.st_mode);
}

bool redirectFd(const char* path, int flags, mode_t mode, int targetFd) {
    int fd = open(path, flags, mode);
    if (fd < 0) {
        return false;
    }
    const bool ok = dup2(fd, targetFd) >= 0;
    if (fd != targetFd) {
        close(fd);
    }
    return ok;
}

bool configureDaemonStdio() {
    if (!redirectFd(kDevNullPath, O_RDONLY, 0, STDIN_FILENO)) {
        return false;
    }
#ifndef NDEBUG
    constexpr int kLogFlags = O_WRONLY | O_CREAT | O_APPEND;
    if (!redirectFd(kLogFilePath, kLogFlags, 0644, STDOUT_FILENO)) {
        return false;
    }
    return redirectFd(kLogFilePath, kLogFlags, 0644, STDERR_FILENO);
#else
    if (!redirectFd(kDevNullPath, O_WRONLY, 0, STDOUT_FILENO)) {
        return false;
    }
    return redirectFd(kDevNullPath, O_WRONLY, 0, STDERR_FILENO);
#endif
}

bool startDaemonProcess() {
    if (!ensureRuntimeRoot()) {
        fprintf(stderr, "ERROR: mkdir %s failed: %s\n", kRuntimeRootDir, strerror(errno));
        return false;
    }
    if (daemon(0, 0) != 0) {
        fprintf(stderr, "ERROR: daemon failed: %s\n", strerror(errno));
        return false;
    }
    return configureDaemonStdio();
}

class PidFileGuard {
public:
    explicit PidFileGuard(const char* path) : path_(path) {}

    PidFileGuard(const PidFileGuard&) = delete;
    PidFileGuard& operator=(const PidFileGuard&) = delete;

    ~PidFileGuard() {
        remove();
    }

    bool write() {
        FILE* fp = fopen(path_, "w");
        if (!fp) {
            logf("E", "pid file open failed path=%s err=%s", path_, strerror(errno));
            return false;
        }
        const bool wrote = fprintf(fp, "%d\n", getpid()) > 0 && fflush(fp) == 0;
        const int closeErr = fclose(fp);
        if (!wrote || closeErr != 0) {
            logf("E", "pid file write failed path=%s err=%s", path_, strerror(errno));
            unlink(path_);
            return false;
        }
        active_ = true;
        return true;
    }

    void remove() {
        if (!active_) {
            return;
        }
        unlink(path_);
        active_ = false;
    }

private:
    const char* path_ = nullptr;
    bool active_ = false;
};

void configureSignals() {
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    signal(SIGHUP, SIG_IGN);
}

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

double elapsedMs(TimePoint start, TimePoint end = Clock::now()) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

std::string joinPath(const std::string& dir, const char* leaf) {
    if (dir.empty()) {
        return leaf ? std::string(leaf) : std::string();
    }
    if (dir.back() == '/' || dir.back() == '\\') {
        return dir + (leaf ? leaf : "");
    }
    return dir + "/" + (leaf ? leaf : "");
}

std::string pathLeaf(const std::string& path) {
    const size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) {
        return path;
    }
    return path.substr(pos + 1);
}

struct Options {
    std::string libDir = kRuntimeLibDir;
    std::string qnnSkelDir = kRuntimeLibDir;
    std::string qnnCacheDir = kQnnCacheDir;
    std::string modelFile;
    std::string modelToken;
    std::string precision = "fp16";
    std::string perf = "high";
    int screenWidth = 0;
    int screenHeight = 0;
    int touchScreenWidth = 0;
    int touchScreenHeight = 0;
    int rotation = 0;
    int inputSize = 416;
    int numBoxes = 3549;
    int numClasses = kNumClasses;
    bool splitOutput = false;
    bool touch = true;
};

struct ModelVariant {
    const char* id = "";
    const char* label = "";
    const char* fileName = "";
    const char* token = "";
    const char* precision = "";
    const char* perf = "";
    int inputSize = 0;
    int numBoxes = 0;
    int numClasses = kNumClasses;
    bool splitOutput = false;
};

constexpr ModelVariant kModelVariants[] = {
    {
        "fp16",
        "fp16 416",
        "model.tflite",
        "enemy_detector_416_fp16",
        "fp16",
        "high",
        416,
        3549,
        6,
        false
    },
    {
        "int8",
        "int8 416",
        "model_int8.tflite",
        "enemy_detector_416_int8_split_f32out",
        "int8",
        "high",
        416,
        3549,
        6,
        true
    },
    {
        "fast256",
        "fast256",
        "model_fast256_int8.tflite",
        "enemy_detector_256_int8_split_f32out_burst",
        "int8",
        "burst",
        256,
        1344,
        6,
        true
    }
};

constexpr int kModelVariantCount =
        static_cast<int>(sizeof(kModelVariants) / sizeof(kModelVariants[0]));
constexpr int kDefaultModelVariantIndex = 2;

struct ModelMenuState {
    std::vector<int> available;
    int active = -1;
    int requested = -1;
    bool switching = false;
    std::string status;
};

void applyModelVariant(const ModelVariant& variant, Options* opts) {
    if (!opts) {
        return;
    }
    opts->modelFile = joinPath(opts->libDir, variant.fileName);
    opts->modelToken = variant.token;
    opts->precision = variant.precision;
    opts->perf = variant.perf;
    opts->inputSize = variant.inputSize;
    opts->numBoxes = variant.numBoxes;
    opts->numClasses = variant.numClasses;
    opts->splitOutput = variant.splitOutput;
}

int findModelVariant(const Options& opts) {
    const std::string fileName = pathLeaf(opts.modelFile);
    for (int i = 0; i < kModelVariantCount; ++i) {
        const ModelVariant& variant = kModelVariants[i];
        if (opts.modelToken == variant.token &&
            opts.inputSize == variant.inputSize &&
            opts.numBoxes == variant.numBoxes &&
            opts.numClasses == variant.numClasses &&
            opts.splitOutput == variant.splitOutput) {
            return i;
        }
        if (!fileName.empty() && fileName == variant.fileName) {
            return i;
        }
    }
    return -1;
}

bool isModelVariantReadable(const Options& opts, int index) {
    if (index < 0 || index >= kModelVariantCount) {
        return false;
    }
    const std::string path = joinPath(opts.libDir, kModelVariants[index].fileName);
    return access(path.c_str(), R_OK) == 0;
}

ModelMenuState buildModelMenuState(const Options& opts) {
    ModelMenuState state;
    state.active = findModelVariant(opts);
    for (int i = 0; i < kModelVariantCount; ++i) {
        if (isModelVariantReadable(opts, i)) {
            state.available.push_back(i);
        }
    }
    if (state.active >= 0) {
        state.status = std::string("当前: ") + kModelVariants[state.active].label;
    } else {
        state.status = "当前: 自定义";
    }
    return state;
}

const char* modelVariantLabel(int index) {
    if (index < 0 || index >= kModelVariantCount) {
        return "自定义";
    }
    return kModelVariants[index].label;
}

struct DisplayState {
    int width = 0;
    int height = 0;
    int rotation = 0;
};

bool isValidDisplayState(const DisplayState& state) {
    return state.width > 0 && state.height > 0 && state.rotation >= 0 && state.rotation <= 3;
}

DisplayState displayStateFromOptions(const Options& opts) {
    return DisplayState { opts.screenWidth, opts.screenHeight, opts.rotation };
}

void applyDisplayState(const DisplayState& state, Options* opts) {
    opts->screenWidth = state.width;
    opts->screenHeight = state.height;
    opts->rotation = state.rotation;
}

void ensureTouchScreenState(Options* opts) {
    if (opts->touchScreenWidth > 0 && opts->touchScreenHeight > 0) {
        return;
    }
    if (opts->screenWidth <= 0 || opts->screenHeight <= 0) {
        return;
    }
    if (opts->rotation == 1 || opts->rotation == 3) {
        opts->touchScreenWidth = opts->screenHeight;
        opts->touchScreenHeight = opts->screenWidth;
    } else {
        opts->touchScreenWidth = opts->screenWidth;
        opts->touchScreenHeight = opts->screenHeight;
    }
}

bool sameDisplayState(const DisplayState& a, const DisplayState& b) {
    return a.width == b.width && a.height == b.height && a.rotation == b.rotation;
}

bool readNativeDisplayState(JNIEnv* env, DisplayState* state) {
    if (!env || !state) {
        return false;
    }
    DisplayState next {};
    if (!surface_get_display_size(env, next.width, next.height, &next.rotation) ||
        !isValidDisplayState(next)) {
        return false;
    }
    *state = next;
    return true;
}

bool resolveDisplayState(JNIEnv* env, Options* opts) {
    DisplayState nativeState {};
    if (readNativeDisplayState(env, &nativeState)) {
        applyDisplayState(nativeState, opts);
        ensureTouchScreenState(opts);
        logf("I", "display native size=%dx%d rotation=%d",
             opts->screenWidth, opts->screenHeight, opts->rotation);
        return true;
    }

    logf("E", "display native unavailable");
    return false;
}

extern "C" {
typedef struct TfLiteModel TfLiteModel;
typedef struct TfLiteInterpreterOptions TfLiteInterpreterOptions;
typedef struct TfLiteInterpreter TfLiteInterpreter;
typedef struct TfLiteTensor TfLiteTensor;
typedef struct TfLiteDelegate TfLiteDelegate;
typedef int32_t TfLiteStatus;
typedef enum TfLiteType {
    kTfLiteNoType = 0,
    kTfLiteFloat32 = 1,
    kTfLiteInt32 = 2,
    kTfLiteUInt8 = 3,
    kTfLiteInt64 = 4,
    kTfLiteString = 5,
    kTfLiteBool = 6,
    kTfLiteInt16 = 7,
    kTfLiteComplex64 = 8,
    kTfLiteInt8 = 9,
} TfLiteType;
typedef struct TfLiteQuantizationParams {
    float scale;
    int32_t zero_point;
} TfLiteQuantizationParams;

typedef enum TfLiteQnnDelegateBackendType {
    kUndefinedBackend = 0,
    kGpuBackend,
    kHtpBackend,
    kDspBackend,
    kIrBackend,
} TfLiteQnnDelegateBackendType;
typedef enum TfLiteQnnDelegateLogLevel {
    kLogOff = 0,
    kLogLevelError = 1,
} TfLiteQnnDelegateLogLevel;
typedef enum TfLiteQnnDelegateGraphPriority {
    kQnnPriorityDefault = 0,
} TfLiteQnnDelegateGraphPriority;
typedef enum TfLiteQnnDelegateProfilingOptions {
    kProfilingOff = 0,
} TfLiteQnnDelegateProfilingOptions;
typedef enum TfLiteQnnDelegateGpuPrecision {
    kGpuFp16 = 2,
} TfLiteQnnDelegateGpuPrecision;
typedef enum TfLiteQnnDelegateGpuPerformanceMode {
    kGpuDefault = 0,
} TfLiteQnnDelegateGpuPerformanceMode;
typedef enum TfLiteQnnDelegateHtpPerformanceMode {
    kHtpDefault = 0,
    kHtpBurst = 2,
    kHtpHighPerformance = 3,
} TfLiteQnnDelegateHtpPerformanceMode;
typedef enum TfLiteQnnDelegateHtpPerfCtrlStrategy {
    kHtpPerfCtrlManual = 0,
} TfLiteQnnDelegateHtpPerfCtrlStrategy;
typedef enum TfLiteQnnDelegateHtpPrecision {
    kHtpQuantized = 0,
    kHtpFp16 = 1,
} TfLiteQnnDelegateHtpPrecision;
typedef enum TfLiteQnnDelegateHtpPdSession {
    kHtpUnsignedPd = 0,
} TfLiteQnnDelegateHtpPdSession;
typedef enum TfLiteQnnDelegateHtpOptimizationStrategy {
    kHtpOptimizeForInference = 0,
} TfLiteQnnDelegateHtpOptimizationStrategy;
typedef enum TfLiteQnnDelegatePerformanceAction {
    kPerformanceVote = 0,
    kPerformanceRelease = 1,
} TfLiteQnnDelegatePerformanceAction;
typedef enum TfLiteQnnDelegateCapability {
    kCapHtpRuntimeQuant = 0,
    kCapHtpRuntimeFp16 = 1,
} TfLiteQnnDelegateCapability;

typedef struct TfLiteQnnDelegateGpuBackendOptions {
    TfLiteQnnDelegateGpuPrecision precision;
    TfLiteQnnDelegateGpuPerformanceMode performance_mode;
    const char* kernel_repo_dir;
} TfLiteQnnDelegateGpuBackendOptions;
typedef struct TfLiteQnnDelegateHtpBackendOptions {
    TfLiteQnnDelegateHtpPerformanceMode performance_mode;
    TfLiteQnnDelegateHtpPerfCtrlStrategy perf_ctrl_strategy;
    TfLiteQnnDelegateHtpPrecision precision;
    TfLiteQnnDelegateHtpPdSession pd_session;
    TfLiteQnnDelegateHtpOptimizationStrategy optimization_strategy;
    bool useConvHmx;
    bool useFoldRelu;
    uint32_t vtcm_size;
    uint32_t num_hvx_threads;
    uint32_t device_id;
} TfLiteQnnDelegateHtpBackendOptions;
typedef struct TfLiteQnnDelegateDspBackendOptions {
    int performance_mode;
    int perf_ctrl_strategy;
    int pd_session;
    int encoding;
} TfLiteQnnDelegateDspBackendOptions;
typedef struct TfLiteQnnDelegateIrBackendOptions {
    const char* output_path;
} TfLiteQnnDelegateIrBackendOptions;
typedef struct TfLiteQnnDelegateOpPackageOptions {
    int num_op_package_infos;
    void* op_package_infos;
} TfLiteQnnDelegateOpPackageOptions;
typedef struct TfLiteQnnDelegateSkipOption {
    const int* skip_delegate_ops;
    uint32_t skip_delegate_ops_nr;
    const int* skip_delegate_node_ids;
    uint32_t skip_delegate_node_ids_nr;
} TfLiteQnnDelegateSkipOption;
typedef struct TfLiteQnnDelegateOptions {
    TfLiteQnnDelegateBackendType backend_type;
    const char* library_path;
    const char* skel_library_dir;
    TfLiteQnnDelegateGpuBackendOptions gpu_options;
    TfLiteQnnDelegateHtpBackendOptions htp_options;
    TfLiteQnnDelegateDspBackendOptions dsp_options;
    TfLiteQnnDelegateIrBackendOptions ir_options;
    TfLiteQnnDelegateLogLevel log_level;
    TfLiteQnnDelegateProfilingOptions profiling;
    TfLiteQnnDelegateOpPackageOptions op_package_options;
    const char* tensor_dump_output_path;
    const char* cache_dir;
    const char* model_token;
    TfLiteQnnDelegateSkipOption skip_options;
    TfLiteQnnDelegateGraphPriority graph_priority;
} TfLiteQnnDelegateOptions;
}

struct TfLiteApi {
    void* tflite = nullptr;
    void* qnn = nullptr;
    TfLiteModel* (*modelCreate)(const void*, size_t) = nullptr;
    TfLiteModel* (*modelCreateFromFile)(const char*) = nullptr;
    void (*modelDelete)(TfLiteModel*) = nullptr;
    TfLiteInterpreterOptions* (*optionsCreate)() = nullptr;
    void (*optionsDelete)(TfLiteInterpreterOptions*) = nullptr;
    void (*optionsSetNumThreads)(TfLiteInterpreterOptions*, int32_t) = nullptr;
    void (*optionsAddDelegate)(TfLiteInterpreterOptions*, TfLiteDelegate*) = nullptr;
    TfLiteInterpreter* (*interpreterCreate)(const TfLiteModel*, const TfLiteInterpreterOptions*) = nullptr;
    void (*interpreterDelete)(TfLiteInterpreter*) = nullptr;
    TfLiteStatus (*allocateTensors)(TfLiteInterpreter*) = nullptr;
    TfLiteStatus (*invoke)(TfLiteInterpreter*) = nullptr;
    TfLiteTensor* (*inputTensor)(const TfLiteInterpreter*, int32_t) = nullptr;
    const TfLiteTensor* (*outputTensor)(const TfLiteInterpreter*, int32_t) = nullptr;
    int32_t (*outputTensorCount)(const TfLiteInterpreter*) = nullptr;
    TfLiteStatus (*tensorCopyFromBuffer)(TfLiteTensor*, const void*, size_t) = nullptr;
    TfLiteStatus (*tensorCopyToBuffer)(const TfLiteTensor*, void*, size_t) = nullptr;
    TfLiteType (*tensorType)(const TfLiteTensor*) = nullptr;
    size_t (*tensorByteSize)(const TfLiteTensor*) = nullptr;
    int32_t (*tensorNumDims)(const TfLiteTensor*) = nullptr;
    int32_t (*tensorDim)(const TfLiteTensor*, int32_t) = nullptr;
    TfLiteQuantizationParams (*tensorQuantizationParams)(const TfLiteTensor*) = nullptr;
    TfLiteQnnDelegateOptions (*qnnOptionsDefault)() = nullptr;
    TfLiteDelegate* (*qnnCreate)(const TfLiteQnnDelegateOptions*) = nullptr;
    void (*qnnDelete)(TfLiteDelegate*) = nullptr;
    bool (*qnnSetPerf)(TfLiteDelegate*, TfLiteQnnDelegatePerformanceAction) = nullptr;
    int32_t (*qnnHasCapability)(TfLiteQnnDelegateCapability) = nullptr;

    template <typename T>
    bool loadSym(void* handle, T* target, const char* name) {
        *target = reinterpret_cast<T>(dlsym(handle, name));
        if (!*target) {
            logf("E", "missing symbol %s: %s", name, dlerror());
            return false;
        }
        return true;
    }

    bool load(const std::string& libDir) {
        const std::string tflitePath = libDir + "/libtensorflowlite_jni.so";
        const std::string qnnPath = libDir + "/libQnnTFLiteDelegate.so";
        tflite = dlopen(tflitePath.c_str(), RTLD_NOW | RTLD_GLOBAL);
        if (!tflite) {
            logf("E", "dlopen %s failed: %s", tflitePath.c_str(), dlerror());
            logf("E", "Install compatible runtime libraries as described in lib/arm64-v8a/README.md");
            return false;
        }
        qnn = dlopen(qnnPath.c_str(), RTLD_NOW | RTLD_GLOBAL);
        if (!qnn) {
            logf("E", "dlopen %s failed: %s", qnnPath.c_str(), dlerror());
            logf("E", "Install compatible runtime libraries as described in lib/arm64-v8a/README.md");
            return false;
        }
        return loadSym(tflite, &modelCreate, "TfLiteModelCreate") &&
               loadSym(tflite, &modelCreateFromFile, "TfLiteModelCreateFromFile") &&
               loadSym(tflite, &modelDelete, "TfLiteModelDelete") &&
               loadSym(tflite, &optionsCreate, "TfLiteInterpreterOptionsCreate") &&
               loadSym(tflite, &optionsDelete, "TfLiteInterpreterOptionsDelete") &&
               loadSym(tflite, &optionsSetNumThreads, "TfLiteInterpreterOptionsSetNumThreads") &&
               loadSym(tflite, &optionsAddDelegate, "TfLiteInterpreterOptionsAddDelegate") &&
               loadSym(tflite, &interpreterCreate, "TfLiteInterpreterCreate") &&
               loadSym(tflite, &interpreterDelete, "TfLiteInterpreterDelete") &&
               loadSym(tflite, &allocateTensors, "TfLiteInterpreterAllocateTensors") &&
               loadSym(tflite, &invoke, "TfLiteInterpreterInvoke") &&
               loadSym(tflite, &inputTensor, "TfLiteInterpreterGetInputTensor") &&
               loadSym(tflite, &outputTensor, "TfLiteInterpreterGetOutputTensor") &&
               loadSym(tflite, &outputTensorCount, "TfLiteInterpreterGetOutputTensorCount") &&
               loadSym(tflite, &tensorCopyFromBuffer, "TfLiteTensorCopyFromBuffer") &&
               loadSym(tflite, &tensorCopyToBuffer, "TfLiteTensorCopyToBuffer") &&
               loadSym(tflite, &tensorType, "TfLiteTensorType") &&
               loadSym(tflite, &tensorByteSize, "TfLiteTensorByteSize") &&
               loadSym(tflite, &tensorNumDims, "TfLiteTensorNumDims") &&
               loadSym(tflite, &tensorDim, "TfLiteTensorDim") &&
               loadSym(tflite, &tensorQuantizationParams, "TfLiteTensorQuantizationParams") &&
               loadSym(qnn, &qnnOptionsDefault, "TfLiteQnnDelegateOptionsDefault") &&
               loadSym(qnn, &qnnCreate, "TfLiteQnnDelegateCreate") &&
               loadSym(qnn, &qnnDelete, "TfLiteQnnDelegateDelete") &&
               loadSym(qnn, &qnnSetPerf, "TfLiteQnnDelegateSetPerf") &&
               loadSym(qnn, &qnnHasCapability, "TfLiteQnnDelegateHasCapability");
    }
};

struct Detection {
    float left = 0;
    float top = 0;
    float right = 0;
    float bottom = 0;
    float confidence = 0;
    int cls = 0;

    float cx() const { return (left + right) * 0.5f; }
    float cy() const { return (top + bottom) * 0.5f; }
    float width() const { return right - left; }
    float height() const { return bottom - top; }
};

struct DetectorTiming {
    double preprocessMs = 0.0;
    double inputCopyMs = 0.0;
    double invokeMs = 0.0;
    double outputCopyMs = 0.0;
    double postprocessMs = 0.0;
};

struct FrameTiming {
    double displayMs = 0.0;
    double captureMs = 0.0;
    double throttleMs = 0.0;
    DetectorTiming detector;
    double touchMs = 0.0;
    double overlayMs = 0.0;
    double totalMs = 0.0;
    size_t detections = 0;
    bool hasTarget = false;
};

class TimingAccumulator {
public:
    void add(const FrameTiming& timing) {
        ++frames_;
        displayMs_ += timing.displayMs;
        captureMs_ += timing.captureMs;
        throttleMs_ += timing.throttleMs;
        preprocessMs_ += timing.detector.preprocessMs;
        inputCopyMs_ += timing.detector.inputCopyMs;
        invokeMs_ += timing.detector.invokeMs;
        outputCopyMs_ += timing.detector.outputCopyMs;
        postprocessMs_ += timing.detector.postprocessMs;
        touchMs_ += timing.touchMs;
        overlayMs_ += timing.overlayMs;
        totalMs_ += timing.totalMs;
        maxTotalMs_ = std::max(maxTotalMs_, timing.totalMs);
        detections_ += timing.detections;
        if (timing.hasTarget) {
            ++targetFrames_;
        }
    }

    void maybeLog(double fps, const Options& opts, int frameWidth, int frameHeight) {
        const auto now = Clock::now();
        if (lastLog_.time_since_epoch().count() == 0) {
            lastLog_ = now;
            return;
        }
        const double elapsed = elapsedMs(lastLog_, now);
        if (elapsed < 1000.0 || frames_ <= 0) {
            return;
        }

        const double inv = 1.0 / static_cast<double>(frames_);
        logf("I",
             "perf frames=%d fps=%.1f avg_ms total=%.2f display=%.2f capture=%.2f throttle=%.2f prep=%.2f in=%.2f invoke=%.2f out=%.2f post=%.2f touch=%.2f overlay=%.2f max=%.2f det_avg=%.2f target=%d/%d frame=%dx%d screen=%dx%d rot=%d",
             frames_,
             fps,
             totalMs_ * inv,
             displayMs_ * inv,
             captureMs_ * inv,
             throttleMs_ * inv,
             preprocessMs_ * inv,
             inputCopyMs_ * inv,
             invokeMs_ * inv,
             outputCopyMs_ * inv,
             postprocessMs_ * inv,
             touchMs_ * inv,
             overlayMs_ * inv,
             maxTotalMs_,
             static_cast<double>(detections_) * inv,
             targetFrames_,
             frames_,
             frameWidth,
             frameHeight,
             opts.screenWidth,
             opts.screenHeight,
             opts.rotation);
        reset(now);
    }

    void reset(TimePoint now = Clock::now()) {
        frames_ = 0;
        displayMs_ = 0.0;
        captureMs_ = 0.0;
        throttleMs_ = 0.0;
        preprocessMs_ = 0.0;
        inputCopyMs_ = 0.0;
        invokeMs_ = 0.0;
        outputCopyMs_ = 0.0;
        postprocessMs_ = 0.0;
        touchMs_ = 0.0;
        overlayMs_ = 0.0;
        totalMs_ = 0.0;
        maxTotalMs_ = 0.0;
        detections_ = 0;
        targetFrames_ = 0;
        lastLog_ = now;
    }

private:
    TimePoint lastLog_ {};
    int frames_ = 0;
    double displayMs_ = 0.0;
    double captureMs_ = 0.0;
    double throttleMs_ = 0.0;
    double preprocessMs_ = 0.0;
    double inputCopyMs_ = 0.0;
    double invokeMs_ = 0.0;
    double outputCopyMs_ = 0.0;
    double postprocessMs_ = 0.0;
    double touchMs_ = 0.0;
    double overlayMs_ = 0.0;
    double totalMs_ = 0.0;
    double maxTotalMs_ = 0.0;
    size_t detections_ = 0;
    int targetFrames_ = 0;
};

float iou(const Detection& a, const Detection& b) {
    const float l = std::max(a.left, b.left);
    const float t = std::max(a.top, b.top);
    const float r = std::min(a.right, b.right);
    const float bt = std::min(a.bottom, b.bottom);
    const float inter = std::max(0.0f, r - l) * std::max(0.0f, bt - t);
    const float areaA = std::max(0.0f, a.width()) * std::max(0.0f, a.height());
    const float areaB = std::max(0.0f, b.width()) * std::max(0.0f, b.height());
    const float uni = areaA + areaB - inter;
    return uni > 0.0f ? inter / uni : 0.0f;
}

std::vector<Detection> nms(std::vector<Detection> detections) {
    std::sort(detections.begin(), detections.end(), [](const Detection& a, const Detection& b) {
        return a.confidence > b.confidence;
    });
    std::vector<Detection> out;
    std::vector<char> removed(detections.size(), 0);
    for (size_t i = 0; i < detections.size(); ++i) {
        if (removed[i]) continue;
        out.push_back(detections[i]);
        for (size_t j = i + 1; j < detections.size(); ++j) {
            if (!removed[j] && iou(detections[i], detections[j]) > kIouThreshold) {
                removed[j] = 1;
            }
        }
    }
    return out;
}

class Detector {
public:
    explicit Detector(const Options& options) : opts_(options) {}

    bool init() {
        if (opts_.modelFile.empty() || access(opts_.modelFile.c_str(), R_OK) != 0) {
            logf("E", "Model is missing or unreadable: %s. See lib/arm64-v8a/README.md",
                 opts_.modelFile.c_str());
            return false;
        }
        if (!api_.load(opts_.libDir)) return false;
        const bool quant = opts_.precision == "int8";
        const int cap = api_.qnnHasCapability(quant ? kCapHtpRuntimeQuant : kCapHtpRuntimeFp16);
        if (cap != 1) {
            logf("E", "QNN HTP capability is not supported: %s", quant ? "int8" : "fp16");
            return false;
        }
        model_ = api_.modelCreateFromFile(opts_.modelFile.c_str());
        if (!model_) {
            logf("E", "TfLiteModelCreate failed for %s; check model compatibility in lib/arm64-v8a/README.md",
                 opts_.modelFile.c_str());
            return false;
        }

        qnnOptions_ = api_.qnnOptionsDefault();
        qnnOptions_.backend_type = kHtpBackend;
        qnnOptions_.skel_library_dir = opts_.qnnSkelDir.c_str();
        qnnOptions_.cache_dir = opts_.qnnCacheDir.c_str();
        qnnOptions_.model_token = opts_.modelToken.c_str();
        qnnOptions_.log_level = kLogLevelError;
        qnnOptions_.htp_options.precision = quant ? kHtpQuantized : kHtpFp16;
        qnnOptions_.htp_options.performance_mode =
                opts_.perf == "burst" ? kHtpBurst : kHtpHighPerformance;
        qnnOptions_.htp_options.optimization_strategy = kHtpOptimizeForInference;
        qnnOptions_.htp_options.perf_ctrl_strategy = kHtpPerfCtrlManual;
        qnnDelegate_ = api_.qnnCreate(&qnnOptions_);
        if (!qnnDelegate_) {
            logf("E", "TfLiteQnnDelegateCreate failed");
            return false;
        }
        (void)api_.qnnSetPerf(qnnDelegate_, kPerformanceVote);

        interpreterOptions_ = api_.optionsCreate();
        api_.optionsSetNumThreads(interpreterOptions_, 4);
        api_.optionsAddDelegate(interpreterOptions_, qnnDelegate_);
        interpreter_ = api_.interpreterCreate(model_, interpreterOptions_);
        if (!interpreter_) {
            logf("E", "TfLiteInterpreterCreate failed");
            return false;
        }
        if (api_.allocateTensors(interpreter_) != 0) {
            logf("E", "TfLiteInterpreterAllocateTensors failed");
            return false;
        }
        if (!validateTensors()) return false;
        if (quant) {
            rebuildQuantLookup();
        }
        inputFloat_.resize(static_cast<size_t>(opts_.inputSize) * opts_.inputSize * 3);
        inputInt8_.resize(inputFloat_.size());
        output_.resize(static_cast<size_t>(4 + opts_.numClasses) * opts_.numBoxes);
        logf("I", "detector ready model=%s input=%d boxes=%d split=%d quant=%d",
             opts_.modelFile.c_str(), opts_.inputSize, opts_.numBoxes,
             opts_.splitOutput ? 1 : 0, quant ? 1 : 0);
        return true;
    }

    bool run(const std::vector<uint8_t>& rgb, int width, int height,
             int* cropLeft, int* cropTop, int* cropSide,
             std::vector<Detection>* detections,
             DetectorTiming* timing = nullptr) {
        DetectorTiming localTiming {};
        DetectorTiming& outTiming = timing ? *timing : localTiming;
        TfLiteTensor* input = api_.inputTensor(interpreter_, 0);
        if (!input) return false;
        auto stageStart = Clock::now();
        fillInput(rgb, width, height, cropLeft, cropTop, cropSide);
        outTiming.preprocessMs = elapsedMs(stageStart);
        stageStart = Clock::now();
        if (opts_.precision == "int8") {
            if (api_.tensorCopyFromBuffer(input, inputInt8_.data(), inputInt8_.size()) != 0) {
                logf("E", "copy int8 input failed");
                return false;
            }
        } else {
            const size_t bytes = inputFloat_.size() * sizeof(float);
            if (api_.tensorCopyFromBuffer(input, inputFloat_.data(), bytes) != 0) {
                logf("E", "copy float input failed");
                return false;
            }
        }
        outTiming.inputCopyMs = elapsedMs(stageStart);
        stageStart = Clock::now();
        if (api_.invoke(interpreter_) != 0) {
            logf("E", "TfLiteInterpreterInvoke failed");
            return false;
        }
        outTiming.invokeMs = elapsedMs(stageStart);
        stageStart = Clock::now();
        if (!copyOutputs()) return false;
        outTiming.outputCopyMs = elapsedMs(stageStart);
        stageStart = Clock::now();
        *detections = postprocess(*cropLeft, *cropTop, *cropSide);
        outTiming.postprocessMs = elapsedMs(stageStart);
        return true;
    }

    ~Detector() {
        if (qnnDelegate_) {
            (void)api_.qnnSetPerf(qnnDelegate_, kPerformanceRelease);
        }
        if (interpreter_) api_.interpreterDelete(interpreter_);
        if (interpreterOptions_) api_.optionsDelete(interpreterOptions_);
        if (qnnDelegate_) api_.qnnDelete(qnnDelegate_);
        if (model_) api_.modelDelete(model_);
    }

private:
    bool validateTensors() {
        TfLiteTensor* input = api_.inputTensor(interpreter_, 0);
        if (!input) {
            logf("E", "missing input tensor");
            return false;
        }
        const TfLiteType expected = opts_.precision == "int8" ? kTfLiteInt8 : kTfLiteFloat32;
        if (api_.tensorType(input) != expected) {
            logf("E", "input tensor dtype mismatch: got=%d expected=%d",
                 api_.tensorType(input), expected);
            return false;
        }
        if (api_.tensorNumDims(input) != 4 ||
            api_.tensorDim(input, 1) != opts_.inputSize ||
            api_.tensorDim(input, 2) != opts_.inputSize ||
            api_.tensorDim(input, 3) != 3) {
            logf("E", "input tensor shape mismatch");
            return false;
        }
        if (opts_.precision == "int8") {
            TfLiteQuantizationParams qp = api_.tensorQuantizationParams(input);
            if (qp.scale <= 0.0f) {
                logf("E", "missing int8 input quantization scale");
                return false;
            }
            quantScale_ = qp.scale;
            quantZeroPoint_ = qp.zero_point;
        }
        const int outCount = api_.outputTensorCount(interpreter_);
        if ((!opts_.splitOutput && outCount < 1) || (opts_.splitOutput && outCount < 2)) {
            logf("E", "unexpected output tensor count %d", outCount);
            return false;
        }
        if (opts_.splitOutput) {
            const TfLiteTensor* a = api_.outputTensor(interpreter_, 0);
            const TfLiteTensor* b = api_.outputTensor(interpreter_, 1);
            const int aChannels = api_.tensorNumDims(a) >= 3 ? api_.tensorDim(a, 1) : 0;
            const int bChannels = api_.tensorNumDims(b) >= 3 ? api_.tensorDim(b, 1) : 0;
            if (aChannels == 4 && bChannels == opts_.numClasses) {
                boxOutputIndex_ = 0;
                scoreOutputIndex_ = 1;
            } else if (aChannels == opts_.numClasses && bChannels == 4) {
                boxOutputIndex_ = 1;
                scoreOutputIndex_ = 0;
            } else {
                logf("E", "split output shape mismatch: ch0=%d ch1=%d", aChannels, bChannels);
                return false;
            }
        }
        return true;
    }

    void rebuildQuantLookup() {
        const float multiplier = 1.0f / (255.0f * quantScale_);
        for (int i = 0; i < 256; ++i) {
            int q = static_cast<int>(lroundf(i * multiplier + quantZeroPoint_));
            q = std::clamp(q, -128, 127);
            quantLookup_[i] = static_cast<int8_t>(q);
        }
    }

    void fillInput(const std::vector<uint8_t>& rgb, int width, int height,
                   int* cropLeft, int* cropTop, int* cropSide) {
        const int inputSize = opts_.inputSize;
        const int side = std::min({inputSize, width, height});
        const int left = std::max(0, (width - side) / 2);
        const int top = std::max(0, (height - side) / 2);
        *cropLeft = left;
        *cropTop = top;
        *cropSide = side;
        size_t out = 0;
        if (opts_.precision == "int8") {
            if (side == inputSize && left == 0 && top == 0) {
                const size_t count = static_cast<size_t>(inputSize) * inputSize * 3;
                for (size_t i = 0; i < count; ++i) {
                    inputInt8_[i] = quantLookup_[rgb[i]];
                }
                return;
            }
            for (int y = 0; y < inputSize; ++y) {
                int sy = top + std::min(side - 1, y * side / inputSize);
                for (int x = 0; x < inputSize; ++x) {
                    int sx = left + std::min(side - 1, x * side / inputSize);
                    const uint8_t* px = rgb.data() + (static_cast<size_t>(sy) * width + sx) * 3;
                    for (int c = 0; c < 3; ++c) {
                        inputInt8_[out++] = quantLookup_[px[c]];
                    }
                }
            }
        } else {
            constexpr float inv255 = 1.0f / 255.0f;
            for (int y = 0; y < inputSize; ++y) {
                int sy = top + std::min(side - 1, y * side / inputSize);
                for (int x = 0; x < inputSize; ++x) {
                    int sx = left + std::min(side - 1, x * side / inputSize);
                    const uint8_t* px = rgb.data() + (static_cast<size_t>(sy) * width + sx) * 3;
                    inputFloat_[out++] = px[0] * inv255;
                    inputFloat_[out++] = px[1] * inv255;
                    inputFloat_[out++] = px[2] * inv255;
                }
            }
        }
    }

    bool copyOutputs() {
        if (!opts_.splitOutput) {
            const TfLiteTensor* output = api_.outputTensor(interpreter_, 0);
            const size_t bytes = output_.size() * sizeof(float);
            if (api_.tensorByteSize(output) < bytes ||
                api_.tensorCopyToBuffer(output, output_.data(), bytes) != 0) {
                logf("E", "copy output failed");
                return false;
            }
            return true;
        }
        const TfLiteTensor* boxes = api_.outputTensor(interpreter_, boxOutputIndex_);
        const TfLiteTensor* scores = api_.outputTensor(interpreter_, scoreOutputIndex_);
        const size_t boxCount = static_cast<size_t>(4) * opts_.numBoxes;
        const size_t scoreCount = static_cast<size_t>(opts_.numClasses) * opts_.numBoxes;
        if (api_.tensorByteSize(boxes) < boxCount * sizeof(float) ||
            api_.tensorByteSize(scores) < scoreCount * sizeof(float)) {
            logf("E", "split output size mismatch");
            return false;
        }
        if (api_.tensorCopyToBuffer(boxes, output_.data(), boxCount * sizeof(float)) != 0 ||
            api_.tensorCopyToBuffer(scores, output_.data() + boxCount,
                                    scoreCount * sizeof(float)) != 0) {
            logf("E", "copy split outputs failed");
            return false;
        }
        return true;
    }

    std::vector<Detection> postprocess(int cropLeft, int cropTop, int cropSide) {
        std::vector<Detection> candidates;
        const float toScreen = cropSide / static_cast<float>(opts_.inputSize);
        for (int i = 0; i < opts_.numBoxes; ++i) {
            const float cx = output_[0 * opts_.numBoxes + i];
            const float cy = output_[1 * opts_.numBoxes + i];
            const float w = output_[2 * opts_.numBoxes + i];
            const float h = output_[3 * opts_.numBoxes + i];
            int bestClass = 0;
            float bestScore = 0.0f;
            for (int c = 0; c < opts_.numClasses; ++c) {
                float score = output_[(4 + c) * opts_.numBoxes + i];
                if (score > bestScore) {
                    bestScore = score;
                    bestClass = c;
                }
            }
            if (bestScore < kConfThreshold) continue;
            Detection d;
            d.left = cropLeft + std::clamp(cx - w * 0.5f, 0.0f, static_cast<float>(opts_.inputSize)) * toScreen;
            d.top = cropTop + std::clamp(cy - h * 0.5f, 0.0f, static_cast<float>(opts_.inputSize)) * toScreen;
            d.right = cropLeft + std::clamp(cx + w * 0.5f, 0.0f, static_cast<float>(opts_.inputSize)) * toScreen;
            d.bottom = cropTop + std::clamp(cy + h * 0.5f, 0.0f, static_cast<float>(opts_.inputSize)) * toScreen;
            d.confidence = bestScore;
            d.cls = bestClass;
            if (d.right > d.left && d.bottom > d.top) {
                candidates.push_back(d);
            }
        }
        return nms(std::move(candidates));
    }

    Options opts_;
    TfLiteApi api_;
    TfLiteModel* model_ = nullptr;
    TfLiteQnnDelegateOptions qnnOptions_ {};
    TfLiteDelegate* qnnDelegate_ = nullptr;
    TfLiteInterpreterOptions* interpreterOptions_ = nullptr;
    TfLiteInterpreter* interpreter_ = nullptr;
    std::vector<float> inputFloat_;
    std::vector<int8_t> inputInt8_;
    std::vector<float> output_;
    float quantScale_ = 1.0f;
    int quantZeroPoint_ = 0;
    int8_t quantLookup_[256] {};
    int boxOutputIndex_ = 0;
    int scoreOutputIndex_ = 1;
};

class ScreenCaptureProcess {
public:
    bool start(int screenWidth, int screenHeight, int inputSize,
               ScreenrecordRawCaptureBackend backend =
                       SCREENRECORD_RAW_CAPTURE_BACKEND_CPU_CONSUMER) {
        stop();
        screenWidth_ = screenWidth;
        screenHeight_ = screenHeight;
        cropSide_ = std::min({inputSize, screenWidth_, screenHeight_}) & ~1;
        if (cropSide_ <= 0) {
            logf("E", "screen capture invalid crop input=%d screen=%dx%d",
                 inputSize, screenWidth_, screenHeight_);
            return false;
        }
        offsetX_ = std::max(0, (screenWidth_ - cropSide_) / 2);
        offsetY_ = std::max(0, (screenHeight_ - cropSide_) / 2);
        width_ = cropSide_;
        height_ = cropSide_;
        frameBytes_ = static_cast<size_t>(width_) * height_ * 3;
        const int rc = screenrecord_raw_capture_create_crop_backend(
                width_,
                height_,
                offsetX_,
                offsetY_,
                cropSide_,
                cropSide_,
                backend,
                &capture_);
        if (rc != 0 || !capture_) {
            logf("E", "screen capture create failed: %d", rc);
            return false;
        }
        backend_ = backend == SCREENRECORD_RAW_CAPTURE_BACKEND_CPU_CONSUMER
                ? "CpuConsumer"
                : "AImageReader";
        frame_.resize(frameBytes_);
        logf("I", "screen capture ready pid=%d backend=%s frame=%dx%d crop=%d,%d,%d screen=%dx%d",
             getpid(), backend_.c_str(),
             width_, height_, offsetX_, offsetY_, cropSide_,
             screenWidth_, screenHeight_);
        return true;
    }

    bool readFrame(std::vector<uint8_t>* out) {
        if (!capture_) {
            return false;
        }
        while (!g_stop.load()) {
            const uint8_t* data = nullptr;
            size_t size = 0;
            int width = 0;
            int height = 0;
            const int rc = screenrecord_raw_capture_read(capture_, &data, &size, &width, &height);
            if (rc == ETIMEDOUT) {
                continue;
            }
            if (rc != 0) {
                logf("E", "screen capture read failed: %d", rc);
                return false;
            }
            if (!data || width != width_ || height != height_ || size != frameBytes_) {
                logf("E", "screen capture frame mismatch got=%dx%d bytes=%zu expected=%dx%d bytes=%zu",
                     width, height, size, width_, height_, frameBytes_);
                return false;
            }
            frame_.assign(data, data + size);
            *out = frame_;
            return true;
        }
        return false;
    }

    int width() const { return width_; }
    int height() const { return height_; }
    int screenWidth() const { return screenWidth_; }
    int screenHeight() const { return screenHeight_; }
    int offsetX() const { return offsetX_; }
    int offsetY() const { return offsetY_; }
    int cropSide() const { return cropSide_; }
    const std::string& backend() const { return backend_; }

    void stop() {
        if (capture_) {
            screenrecord_raw_capture_destroy(capture_);
            capture_ = nullptr;
        }
    }

    ~ScreenCaptureProcess() { stop(); }

private:
    ScreenrecordRawCapture* capture_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int screenWidth_ = 0;
    int screenHeight_ = 0;
    int offsetX_ = 0;
    int offsetY_ = 0;
    int cropSide_ = 0;
    std::string backend_ = "CpuConsumer";
    size_t frameBytes_ = 0;
    std::vector<uint8_t> frame_;
};

uint32_t sampleRgbChecksum(const std::vector<uint8_t>& frame) {
    uint32_t hash = 2166136261u;
    const size_t step = std::max<size_t>(1, frame.size() / 4096);
    for (size_t i = 0; i < frame.size(); i += step) {
        hash ^= frame[i];
        hash *= 16777619u;
    }
    return hash;
}

bool runCaptureSmokeTest(const Options& opts) {
    ScreenCaptureProcess capture;
    if (!capture.start(opts.screenWidth, opts.screenHeight, opts.inputSize)) {
        return false;
    }

    std::vector<uint8_t> frame;
    for (int i = 0; i < 5 && !g_stop.load(); ++i) {
        if (!capture.readFrame(&frame)) {
            logf("E", "capture smoke read failed frame=%d", i);
            return false;
        }
        logf("I", "capture smoke frame=%d backend=%s size=%dx%d bytes=%zu checksum=%08x",
             i + 1,
             capture.backend().c_str(),
             capture.width(),
             capture.height(),
             frame.size(),
             sampleRgbChecksum(frame));
        usleep(16000);
    }
    logf("I", "capture smoke ok backend=%s frames=5", capture.backend().c_str());
    return true;
}

bool captureBackendFromName(const std::string& name, ScreenrecordRawCaptureBackend* backend) {
    if (!backend) {
        return false;
    }
    if (name == "aimage" || name == "imagereader" || name == "AImageReader") {
        *backend = SCREENRECORD_RAW_CAPTURE_BACKEND_AIMAGE_READER;
        return true;
    }
    if (name == "cpu" || name == "cpuconsumer" || name == "CpuConsumer") {
        *backend = SCREENRECORD_RAW_CAPTURE_BACKEND_CPU_CONSUMER;
        return true;
    }
    return false;
}

double percentileMs(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) {
        return 0.0;
    }
    const double clamped = std::clamp(p, 0.0, 1.0);
    const size_t index = std::min(
            sorted.size() - 1,
            static_cast<size_t>(llround(clamped * static_cast<double>(sorted.size() - 1))));
    return sorted[index];
}

bool runCaptureBenchmark(const Options& opts,
                         ScreenrecordRawCaptureBackend backend,
                         int frames) {
    frames = std::clamp(frames, 1, 2000);
    ScreenCaptureProcess capture;

    auto stageStart = Clock::now();
    if (!capture.start(opts.screenWidth, opts.screenHeight, opts.inputSize, backend)) {
        return false;
    }
    const double startMs = elapsedMs(stageStart);

    std::vector<uint8_t> frame;
    constexpr int kWarmupFrames = 8;
    for (int i = 0; i < kWarmupFrames && !g_stop.load(); ++i) {
        if (!capture.readFrame(&frame)) {
            logf("E", "capture benchmark warmup failed backend=%s frame=%d",
                 capture.backend().c_str(), i);
            return false;
        }
        usleep(kFrameIntervalUs);
    }

    std::vector<double> samples;
    samples.reserve(static_cast<size_t>(frames));
    uint32_t checksum = 0;
    for (int i = 0; i < frames && !g_stop.load(); ++i) {
        stageStart = Clock::now();
        if (!capture.readFrame(&frame)) {
            logf("E", "capture benchmark read failed backend=%s frame=%d",
                 capture.backend().c_str(), i);
            return false;
        }
        samples.push_back(elapsedMs(stageStart));
        checksum ^= sampleRgbChecksum(frame) + static_cast<uint32_t>(i * 2654435761u);
        usleep(kFrameIntervalUs);
    }
    if (samples.empty()) {
        return false;
    }

    std::sort(samples.begin(), samples.end());
    const double totalMs = std::accumulate(samples.begin(), samples.end(), 0.0);
    const double avgMs = totalMs / static_cast<double>(samples.size());
    logf("I",
         "capture benchmark backend=%s frames=%zu start_ms=%.3f read_ms avg=%.3f min=%.3f p50=%.3f p90=%.3f p99=%.3f max=%.3f size=%dx%d bytes=%zu checksum=%08x",
         capture.backend().c_str(),
         samples.size(),
         startMs,
         avgMs,
         samples.front(),
         percentileMs(samples, 0.50),
         percentileMs(samples, 0.90),
         percentileMs(samples, 0.99),
         samples.back(),
         capture.width(),
         capture.height(),
         frame.size(),
         checksum);
    return true;
}

class TouchController {
public:
    bool start(const Options& opts) {
        stop();
        const int touchWidth = opts.touchScreenWidth > 0 ? opts.touchScreenWidth : opts.screenWidth;
        const int touchHeight = opts.touchScreenHeight > 0 ? opts.touchScreenHeight : opts.screenHeight;
        proxy_ = ktouchproxy_create(touchWidth, touchHeight, opts.rotation, 1);
        if (!proxy_) {
            logf("E", "ktouchproxy_create failed");
            return false;
        }
        running_.store(true);
        pollThread_ = std::thread([this]() { pollLoop(); });
        logf("I", "touch controller ready pid=%d size=%dx%d rotation=%d",
             getpid(), touchWidth, touchHeight, opts.rotation);
        return true;
    }

    void poll() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (proxy_) {
            ktouchproxy_poll(proxy_, 0);
        }
    }

    KTouchState touchState() {
        std::lock_guard<std::mutex> lock(mutex_);
        KTouchState state {};
        if (proxy_) {
            ktouchproxy_get_touch_state(proxy_, &state);
        }
        return state;
    }

    TouchAimStatus aimStatus() {
        std::lock_guard<std::mutex> lock(mutex_);
        return aimStatus_;
    }

    bool aim(int width, int height, const Detection& target,
             const TouchTuning& tuning, float dragScale) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!proxy_) return false;
        const auto now = Clock::now();
        if (!aiming_ && now < nextAimUpdate_) {
            return false;
        }
        const int minDim = std::max(1, std::min(width, height));
        const int anchorX = std::clamp(static_cast<int>(lroundf(width * kTouchAnchorXRatio)),
                                       0,
                                       width - 1);
        const int anchorY = std::clamp(static_cast<int>(lroundf(height * kTouchAnchorYRatio)),
                                       0,
                                       height - 1);
        const float rawDx = target.cx() - width * 0.5f;
        const float rawDy = target.cy() - height * 0.5f;
        const float rawDist = sqrtf(rawDx * rawDx + rawDy * rawDy);
        const float deadzone = tuning.deadzonePx;
        const float releaseDeadzone = deadzone * tuning.deadzoneReleaseScale;
        const float idleExitDeadzone = releaseDeadzone * kTouchDeadzoneIdleExitScale;
        const float fineDistance = std::max(deadzone * 2.0f, minDim * tuning.fineDistanceRatio);
        const float brakeDistance = std::max(deadzone * 3.0f, minDim * tuning.brakeDistanceRatio);
        bool overshootX = false;
        bool overshootY = false;
        if (!filterValid_) {
            filteredDx_ = rawDx;
            filteredDy_ = rawDy;
            filterValid_ = true;
        } else {
            filteredDx_ += (rawDx - filteredDx_) * tuning.smoothAlpha;
            filteredDy_ += (rawDy - filteredDy_) * tuning.smoothAlpha;
            if (fabsf(rawDx) < fabsf(filteredDx_)) {
                filteredDx_ = rawDx;
            }
            if (fabsf(rawDy) < fabsf(filteredDy_)) {
                filteredDy_ = rawDy;
            }
            overshootX = rawDx * filteredDx_ < 0.0f;
            overshootY = rawDy * filteredDy_ < 0.0f;
            if (overshootX) {
                filteredDx_ = rawDx * tuning.overshootFilterRatio;
            }
            if (overshootY) {
                filteredDy_ = rawDy * tuning.overshootFilterRatio;
            }
        }

        const bool crossedLastX = lastCommandValid_ &&
                rawDx * lastCommandDx_ < 0.0f &&
                fabsf(rawDx) >= deadzone &&
                (rawDist < brakeDistance || fabsf(rawDx) >= fabsf(rawDy) * 0.55f);
        const bool crossedLastY = lastCommandValid_ &&
                rawDy * lastCommandDy_ < 0.0f &&
                fabsf(rawDy) >= deadzone &&
                (rawDist < brakeDistance || fabsf(rawDy) >= fabsf(rawDx) * 0.55f);
        const bool snapBlocked = overshootX || overshootY || crossedLastX || crossedLastY;
        const float divergeBrake = lastRawDistValid_
                ? std::max(kTouchDivergeBrakePx, lastRawDist_ * kTouchDivergeBrakeRatio)
                : kTouchDivergeBrakePx;
        const bool diverged = aiming_ && lastRawDistValid_ &&
                rawDist > lastRawDist_ + divergeBrake &&
                lastRawDist_ > idleExitDeadzone;
        if (fabsf(rawDx) < deadzone) {
            filteredDx_ = 0.0f;
            residualDragX_ = 0.0f;
        }
        if (fabsf(rawDy) < deadzone) {
            filteredDy_ = 0.0f;
            residualDragY_ = 0.0f;
        }
        const float dx = filteredDx_;
        const float dy = filteredDy_;
        const float dist = sqrtf(dx * dx + dy * dy);
        const bool holdDeadzoneIdle = aiming_ && deadzoneIdle_ &&
                rawDist < idleExitDeadzone &&
                dist < idleExitDeadzone;
        if (rawDist < deadzone || dist < deadzone ||
            (aiming_ && rawDist < releaseDeadzone && dist < releaseDeadzone) ||
            holdDeadzoneIdle) {
            aimStatus_ = {
                true,
                TouchAimMode::Deadzone,
                rawDist,
                dist,
                0.0f,
                0.0f,
                anchorX,
                anchorY
            };
            if (aiming_) {
                if (!deadzoneIdle_) {
                    logf("I",
                         "touch aim deadzone idle cls=%d conf=%.3f bbox=%.1f,%.1f,%.1f,%.1f center=%.1f,%.1f raw_offset=%.1f,%.1f filt_offset=%.1f,%.1f raw_dist=%.2f dist=%.2f deadzone=%.2f release=%.2f exit=%.2f screen=%dx%d",
                         target.cls,
                         target.confidence,
                         target.left,
                         target.top,
                         target.right,
                         target.bottom,
                         target.cx(),
                         target.cy(),
                         rawDx,
                         rawDy,
                         dx,
                         dy,
                         rawDist,
                         dist,
                         deadzone,
                         releaseDeadzone,
                         idleExitDeadzone,
                         width,
                         height);
                }
                residualDragX_ = 0.0f;
                residualDragY_ = 0.0f;
                snapActive_ = false;
                const int rc = ktouchproxy_aim(proxy_, kTouchPointerId,
                                               anchorX, anchorY, anchorX, anchorY);
                if (rc == 1) {
                    deadzoneIdle_ = true;
                    lastCommandValid_ = false;
                    lastCommandDx_ = 0.0f;
                    lastCommandDy_ = 0.0f;
                    lastEndX_ = anchorX;
                    lastEndY_ = anchorY;
                    lastRawDistValid_ = false;
                    nextAimUpdate_ = now + std::chrono::milliseconds(tuning.aimMinIntervalMs);
                    return true;
                }
            } else {
                filterValid_ = false;
                filteredDx_ = 0.0f;
                filteredDy_ = 0.0f;
                lastCommandValid_ = false;
                residualDragX_ = 0.0f;
                residualDragY_ = 0.0f;
                snapActive_ = false;
                lastRawDistValid_ = false;
            }
            return false;
        }
        if (!aiming_ && rawDist < idleExitDeadzone) {
            aimStatus_ = {
                true,
                TouchAimMode::Deadzone,
                rawDist,
                dist,
                0.0f,
                0.0f,
                anchorX,
                anchorY
            };
            filterValid_ = false;
            filteredDx_ = 0.0f;
            filteredDy_ = 0.0f;
            lastCommandValid_ = false;
            residualDragX_ = 0.0f;
            residualDragY_ = 0.0f;
            snapActive_ = false;
            lastRawDistValid_ = false;
            return false;
        }
        if (diverged) {
            logf("I",
                 "touch aim diverge brake cls=%d conf=%.3f raw_offset=%.1f,%.1f raw_dist=%.2f last_raw_dist=%.2f brake=%.2f settle=%d screen=%dx%d",
                 target.cls,
                 target.confidence,
                 rawDx,
                 rawDy,
                 rawDist,
                 lastRawDist_,
                 divergeBrake,
                 tuning.overshootSettleMs,
                 width,
                 height);
            stopAimLocked(false);
            aimStatus_ = {
                true,
                TouchAimMode::Brake,
                rawDist,
                dist,
                0.0f,
                0.0f,
                anchorX,
                anchorY
            };
            nextAimUpdate_ = now + std::chrono::milliseconds(tuning.overshootSettleMs);
            return false;
        }
        const bool wasSnapActive = snapActive_;
        if (snapActive_ && (rawDist <= kTouchSnapExitPx || dist <= kTouchSnapExitPx || snapBlocked)) {
            snapActive_ = false;
        }
        if (!snapActive_ && !snapBlocked &&
            rawDist >= kTouchSnapEnterPx && dist >= kTouchSnapEnterPx) {
            snapActive_ = true;
        }
        if (snapActive_ != wasSnapActive) {
            residualDragX_ = 0.0f;
            residualDragY_ = 0.0f;
        }
        if ((overshootX || overshootY || crossedLastX || crossedLastY) && rawDist < fineDistance) {
            if (aiming_) {
                logf("I",
                     "touch aim brake cls=%d conf=%.3f center=%.1f,%.1f raw_offset=%.1f,%.1f filt_offset=%.1f,%.1f raw_dist=%.2f dist=%.2f fine=%.2f brake=%.2f overshoot=%d,%d crossed=%d,%d screen=%dx%d",
                     target.cls,
                     target.confidence,
                     target.cx(),
                     target.cy(),
                     rawDx,
                     rawDy,
                     dx,
                     dy,
                     rawDist,
                     dist,
                     fineDistance,
                     brakeDistance,
                     overshootX ? 1 : 0,
                     overshootY ? 1 : 0,
                     crossedLastX ? 1 : 0,
                     crossedLastY ? 1 : 0,
                     width,
                     height);
            }
        }
        if (aiming_ && now < nextAimUpdate_) {
            return false;
        }
        float maxDistance = std::max(tuning.minDragPx, minDim * tuning.maxDistanceRatio);
        float gain = std::clamp(dragScale, 0.005f, 0.400f);
        if (snapActive_) {
            maxDistance = std::max(maxDistance, minDim * kTouchSnapMaxDistanceRatio);
            gain = std::min(kTouchSnapGain,
                            std::clamp(gain * kTouchSnapGainScale, 0.005f, kTouchSnapGain));
        } else if (dist < fineDistance) {
            const float t = std::clamp((dist - deadzone) /
                                       std::max(fineDistance - deadzone, 1.0f), 0.0f, 1.0f);
            gain *= tuning.fineGainScale + (1.0f - tuning.fineGainScale) * t;
        }
        if (overshootX || overshootY || crossedLastX || crossedLastY) {
            gain *= tuning.overshootGainScale;
        }
        if (overshootX || crossedLastX || dx == 0.0f) {
            residualDragX_ = 0.0f;
        }
        if (overshootY || crossedLastY || dy == 0.0f) {
            residualDragY_ = 0.0f;
        }
        const float drag = std::clamp(dist * gain, tuning.minDragPx, maxDistance);
        const float scale = drag / std::max(dist, 1.0f);
        const float xScale = std::clamp(tuning.xAxisScale, 0.05f, 1.50f);
        const float yScale = std::clamp(tuning.yAxisScale, 0.05f, 1.50f);
        const float moveX = dx * scale * xScale;
        const float moveY = dy * scale * yScale;
        const float pendingMoveX = residualDragX_ + moveX;
        const float pendingMoveY = residualDragY_ + moveY;
        const int stepX = static_cast<int>(lroundf(pendingMoveX));
        const int stepY = static_cast<int>(lroundf(pendingMoveY));
        const int endX = std::clamp(anchorX + stepX, 0, width - 1);
        const int endY = std::clamp(anchorY + stepY, 0, height - 1);
        residualDragX_ = std::clamp(pendingMoveX - static_cast<float>(endX - anchorX),
                                    -0.49f,
                                    0.49f);
        residualDragY_ = std::clamp(pendingMoveY - static_cast<float>(endY - anchorY),
                                    -0.49f,
                                    0.49f);
        TouchAimMode mode = TouchAimMode::Normal;
        if (snapActive_) {
            mode = TouchAimMode::Snap;
        } else if (dist < fineDistance) {
            mode = TouchAimMode::Fine;
        }
        if (overshootX || overshootY || crossedLastX || crossedLastY) {
            mode = TouchAimMode::Brake;
        }
        aimStatus_ = {
            true,
            mode,
            rawDist,
            dist,
            drag,
            gain,
            anchorX,
            anchorY
        };
        const int rc = ktouchproxy_aim(proxy_, kTouchPointerId, anchorX, anchorY, endX, endY);
        logf("I",
             "touch aim rc=%d cls=%d conf=%.3f bbox=%.1f,%.1f,%.1f,%.1f center=%.1f,%.1f raw_offset=%.1f,%.1f filt_offset=%.1f,%.1f raw_dist=%.2f dist=%.2f drag=%.2f gain=%.3f snap=%d overshoot=%d,%d crossed=%d,%d scale=%.3f x_scale=%.2f y_scale=%.2f residual=%.2f,%.2f anchor=%d,%d end=%d,%d interval=%d screen=%dx%d",
             rc,
             target.cls,
             target.confidence,
             target.left,
             target.top,
             target.right,
             target.bottom,
             target.cx(),
             target.cy(),
             rawDx,
             rawDy,
             dx,
             dy,
             rawDist,
             dist,
             drag,
             gain,
             snapActive_ ? 1 : 0,
             overshootX ? 1 : 0,
             overshootY ? 1 : 0,
             crossedLastX ? 1 : 0,
             crossedLastY ? 1 : 0,
             scale,
             xScale,
             yScale,
             residualDragX_,
             residualDragY_,
             anchorX,
             anchorY,
             endX,
             endY,
             tuning.aimMinIntervalMs,
             width,
             height);
        if (rc == 1) {
            aiming_ = true;
            deadzoneIdle_ = false;
            lastCommandValid_ = true;
            lastCommandDx_ = rawDx;
            lastCommandDy_ = rawDy;
            lastEndX_ = endX;
            lastEndY_ = endY;
            lastRawDist_ = rawDist;
            lastRawDistValid_ = true;
            nextAimUpdate_ = now + std::chrono::milliseconds(tuning.aimMinIntervalMs);
            return true;
        }
        return false;
    }

    void resetAimFilter() {
        std::lock_guard<std::mutex> lock(mutex_);
        filterValid_ = false;
        filteredDx_ = 0.0f;
        filteredDy_ = 0.0f;
        lastCommandValid_ = false;
        lastCommandDx_ = 0.0f;
        lastCommandDy_ = 0.0f;
        lastEndX_ = -1;
        lastEndY_ = -1;
        deadzoneIdle_ = false;
        residualDragX_ = 0.0f;
        residualDragY_ = 0.0f;
        snapActive_ = false;
        lastRawDistValid_ = false;
        aimStatus_ = {};
        nextAimUpdate_ = Clock::now();
    }

    void stopAim() {
        std::lock_guard<std::mutex> lock(mutex_);
        stopAimLocked();
    }

    void stop() {
        running_.store(false);
        if (pollThread_.joinable()) {
            pollThread_.join();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        stopAimLocked();
        if (proxy_) {
            ktouchproxy_destroy(proxy_);
            proxy_ = nullptr;
        }
    }

    ~TouchController() { stop(); }

private:
    void pollLoop() {
        while (running_.load()) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (proxy_) {
                    ktouchproxy_poll(proxy_, 0);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
    }

    void stopAimLocked() {
        stopAimLocked(true);
    }

    void stopAimLocked(bool resetFilter) {
        if (aiming_ && proxy_) {
            ktouchproxy_stop_aim(proxy_, kTouchPointerId);
            logf("I", "touch aim stop pointer=%d", kTouchPointerId);
        }
        aiming_ = false;
        lastCommandValid_ = false;
        lastCommandDx_ = 0.0f;
        lastCommandDy_ = 0.0f;
        lastEndX_ = -1;
        lastEndY_ = -1;
        deadzoneIdle_ = false;
        residualDragX_ = 0.0f;
        residualDragY_ = 0.0f;
        snapActive_ = false;
        lastRawDistValid_ = false;
        aimStatus_ = {};
        nextAimUpdate_ = TimePoint {};
        if (resetFilter) {
            filterValid_ = false;
            filteredDx_ = 0.0f;
            filteredDy_ = 0.0f;
        }
    }

    std::mutex mutex_;
    std::thread pollThread_;
    std::atomic<bool> running_ { false };
    KTouchProxy* proxy_ = nullptr;
    bool aiming_ = false;
    bool filterValid_ = false;
    bool lastCommandValid_ = false;
    float filteredDx_ = 0.0f;
    float filteredDy_ = 0.0f;
    float lastCommandDx_ = 0.0f;
    float lastCommandDy_ = 0.0f;
    int lastEndX_ = -1;
    int lastEndY_ = -1;
    bool deadzoneIdle_ = false;
    bool snapActive_ = false;
    bool lastRawDistValid_ = false;
    float lastRawDist_ = 0.0f;
    float residualDragX_ = 0.0f;
    float residualDragY_ = 0.0f;
    TouchAimStatus aimStatus_ {};
    TimePoint nextAimUpdate_ {};
};

class OverlayRenderer {
public:
    bool start(JNIEnv* env, int screenWidth, int screenHeight, Font* font) {
        stop();
        env_ = env;
        screenWidth_ = screenWidth;
        screenHeight_ = screenHeight;
        font_ = font;
        if (!env_) {
            logf("E", "overlay missing JNIEnv");
            return false;
        }
        if (!surface_create(env_, surface_, screenWidth_, screenHeight_, "kaiagentd", true)) {
            logf("E", "surface_create failed");
            return false;
        }
        menuTouchCaptured_ = false;
        ready_ = true;
        return true;
    }

    bool render(const std::vector<Detection>& detections, const Detection* target,
                int cropLeft, int cropTop, int cropSide, double fps,
                const KTouchState& touchState, const TouchAimStatus& aimStatus,
                TouchTuning* tuning, ModelMenuState* modelMenu) {
        if (!ready_) return false;
        Canvas canvas;
        if (!canvas.lock(surface_.window)) {
            logf("W", "overlay lock failed");
            return false;
        }
        canvas.clear(rgba(0, 0, 0, 0));
        draw::rect(canvas, cropLeft, cropTop, cropSide, cropSide, rgba(60, 180, 255, 120));
        for (const Detection& d : detections) {
            const bool isTarget = target && fabsf(d.cx() - target->cx()) < 0.5f &&
                                  fabsf(d.cy() - target->cy()) < 0.5f;
            const uint32_t color = isTarget ? kTargetColor : kBoxColor;
            draw::rect(canvas, static_cast<int>(lroundf(d.left)), static_cast<int>(lroundf(d.top)),
                       std::max(1, static_cast<int>(lroundf(d.width()))),
                       std::max(1, static_cast<int>(lroundf(d.height()))), color);
        }
        char status[96];
        snprintf(status, sizeof(status), "kai %.1ffps det=%zu", fps, detections.size());
        const int statusWidth = measureText(status);
        const int statusX = std::max(0, (screenWidth_ - statusWidth) / 2);
        drawText(canvas, statusX, 8, status, kTextColor);
        menuTouchCaptured_ = drawTouchMenu(canvas, touchState, aimStatus, tuning, modelMenu);
        canvas.unlockAndPost();
        return menuTouchCaptured_;
    }

    bool menuTouchCaptured() const {
        return menuTouchCaptured_;
    }

    void stop() {
        if (ready_) {
            surface_destroy(env_, surface_);
            ready_ = false;
            menuTouchCaptured_ = false;
        }
        font_ = nullptr;
    }

    ~OverlayRenderer() { stop(); }

private:
    bool drawTouchMenu(Canvas& canvas, const KTouchState& touchState,
                       const TouchAimStatus& aimStatus, TouchTuning* tuning,
                       ModelMenuState* modelMenu) {
        if (!tuning) {
            return false;
        }
        MenuTouchState menuTouch {};
        menuTouch.x = touchState.x;
        menuTouch.y = touchState.y;
        menuTouch.dx = touchState.dx;
        menuTouch.dy = touchState.dy;
        menuTouch.down = touchState.down != 0;
        menuTouch.pressed = touchState.pressed != 0;
        menuTouch.released = touchState.released != 0;

        menu_ui::newFrame(canvas, menuTouch, font_);
        if (!menuVisible_) {
            return drawMenuHandle(canvas, menuTouch);
        }

        int menuWidth = std::clamp(screenWidth_ / 3, 360, 460);
        if (screenWidth_ > 40) {
            menuWidth = std::min(menuWidth, screenWidth_ - 24);
        }
        if (menu_ui::beginWindow("KaiAgentD 触控", 24, 84, menuWidth, 0, &menuVisible_) &&
            menuVisible_) {
            int activeTab = 0;
            if (menu_ui::beginTabBar("mainTabs")) {
                if (menu_ui::tabItem("模型")) activeTab = 0;
                if (menu_ui::tabItem("基础")) activeTab = 1;
                if (menu_ui::tabItem("移动")) activeTab = 2;
                if (menu_ui::tabItem("时序")) activeTab = 3;
                menu_ui::endTabBar();
            }
            menu_ui::separator();
            drawAimStatus(aimStatus);
            menu_ui::separator();

            if (activeTab == 0 && modelMenu) {
                menu_ui::text(modelMenu->status.c_str());
                if (modelMenu->switching) {
                    menu_ui::textColored(rgba(255, 210, 90, 255), "切换中...");
                } else if (modelMenu->available.empty()) {
                    menu_ui::textColored(rgba(255, 120, 120, 255), "未找到内置模型");
                } else {
                    for (int index : modelMenu->available) {
                        const char* label = modelVariantLabel(index);
                        if (index == modelMenu->active) {
                            char current[80];
                            snprintf(current, sizeof(current), "当前: %s", label);
                            menu_ui::textColored(rgba(86, 220, 150, 255), current);
                        } else {
                            char button[80];
                            snprintf(button, sizeof(button), "使用 %s", label);
                            if (menu_ui::button(button)) {
                                modelMenu->requested = index;
                            }
                        }
                    }
                }
            } else if (activeTab == 1) {
                if (menu_ui::button("恢复默认")) {
                    resetTouchTuning(tuning);
                }
                menu_ui::separator();
                menu_ui::sliderFloat("速度", &tuning->dragScale, 0.005f, 0.400f, 3);
                menu_ui::sliderFloat("起步速度", &tuning->warmupDragScale, 0.005f, 0.400f, 3);
                menu_ui::sliderFloat("横向比例", &tuning->xAxisScale, 0.05f, 1.50f, 2);
                menu_ui::sliderFloat("纵向比例", &tuning->yAxisScale, 0.05f, 1.50f, 2);
                menu_ui::sliderFloat("死区(px)", &tuning->deadzonePx, 0.5f, 20.0f, 1);
                menu_ui::sliderFloat("平滑", &tuning->smoothAlpha, 0.05f, 1.00f, 2);
            } else if (activeTab == 2) {
                menu_ui::sliderFloat("单步上限", &tuning->maxDistanceRatio, 0.001f, 0.300f, 3);
                menu_ui::sliderFloat("近距范围", &tuning->fineDistanceRatio, 0.001f, 0.300f, 3);
                menu_ui::sliderFloat("近距速度", &tuning->fineGainScale, 0.10f, 1.50f, 2);
                menu_ui::sliderFloat("反向刹车", &tuning->overshootGainScale, 0.05f, 1.00f, 2);
            } else if (activeTab == 3) {
                menu_ui::sliderInt("更新间隔(ms)", &tuning->aimMinIntervalMs, 0, 400);
                menu_ui::sliderInt("刹车冷却(ms)", &tuning->overshootSettleMs, 0, 300);
                menu_ui::sliderInt("起步帧", &tuning->warmupFrames, 0, 10);
            }
        }
        menu_ui::endWindow();
        menu_ui::render();
        if (!menuVisible_) {
            logf("I", "menu close requested; stopping kaiagentd");
            g_stop.store(true);
        }
        clampTouchTuning(tuning);
        return menu_ui::isTouchCaptured();
    }

    void drawAimStatus(const TouchAimStatus& status) {
        char line[128];
        if (!status.valid) {
            menu_ui::text("状态: 空闲");
            return;
        }
        snprintf(line, sizeof(line), "状态: %s  dist=%.1f  drag=%.2f",
                 touchAimModeLabel(status.mode),
                 status.dist,
                 status.drag);
        menu_ui::text(line);
        snprintf(line, sizeof(line), "gain=%.3f  raw=%.1f  anchor=%d,%d",
                 status.gain,
                 status.rawDist,
                 status.anchorX,
                 status.anchorY);
        menu_ui::text(line);
    }

    bool drawMenuHandle(Canvas& canvas, const MenuTouchState& touch) {
        constexpr int x = 24;
        constexpr int y = 84;
        constexpr int w = 86;
        constexpr int h = 36;
        const bool hit = touch.x >= x && touch.x < x + w &&
                         touch.y >= y && touch.y < y + h;
        if (hit && touch.pressed) {
            menuHandleActive_ = true;
        }
        if (touch.released) {
            if (menuHandleActive_ && hit) {
                menuVisible_ = true;
            }
            menuHandleActive_ = false;
        }
        if (!touch.down && !touch.released) {
            menuHandleActive_ = false;
        }

        const uint32_t bg = menuHandleActive_
                ? rgba(31, 84, 137, 245)
                : hit ? rgba(60, 125, 190, 245)
                      : rgba(34, 41, 50, 230);
        draw::fillRoundRect(canvas, x, y, w, h, 6, bg);
        draw::roundRect(canvas, x, y, w, h, 6, rgba(83, 95, 112, 230));
        const char* label = "菜单";
        const int textX = x + std::max(0, (w - measureText(label)) / 2);
        const int textY = y + std::max(0, (h - textHeight()) / 2);
        drawText(canvas, textX, textY, label, rgba(236, 241, 247, 255));
        return hit && (touch.down || touch.pressed || touch.released) || menuHandleActive_;
    }

    int measureText(const char* text) const {
        if (font_ && font_->isLoaded()) {
            return font_->measureText(text ? text : "");
        }
        return static_cast<int>(strlen(text ? text : "")) * 16;
    }

    int textHeight() const {
        if (font_ && font_->isLoaded()) {
            return font_->lineHeight();
        }
        return 16;
    }

    void drawText(Canvas& canvas, int x, int y, const char* text, uint32_t color) const {
        if (font_ && font_->isLoaded()) {
            font_->drawText(canvas, x, y, text ? text : "", color);
        } else {
            draw::text(canvas, x, y, text ? text : "", color, 2);
        }
    }

    JNIEnv* env_ = nullptr;
    SurfaceContext surface_ {};
    Font* font_ = nullptr;
    int screenWidth_ = 0;
    int screenHeight_ = 0;
    bool menuTouchCaptured_ = false;
    bool menuVisible_ = true;
    bool menuHandleActive_ = false;
    bool ready_ = false;
};

class TargetTracker {
public:
    const Detection* update(const std::vector<Detection>& detections, int width, int height,
                            const TouchTuning& tuning) {
        width_ = width;
        height_ = height;
        currentTouchWarmupFrames_ = tuning.warmupFrames;
        resetTouchFilterPending_ = false;
        ageLastLost();

        if (locked_) {
            std::vector<TargetCandidate> candidates =
                    buildCandidates(detections, kTargetTrackMinConfidence);
            const TargetCandidate* match = findTrackedGroup(candidates);
            if (match) {
                if (missedFrames_ > 0) {
                    resetTouchFilterPending_ = true;
                }
                updateLockedTarget(*match);
                missedFrames_ = 0;
                clearPending();
                return &stableAim_;
            }

            ++missedFrames_;
            if (missedFrames_ <= kTargetHoldFrames) {
                maybeLogNoTarget(detections, "hold_no_match");
                return nullptr;
            }
            markLost();
            return nullptr;
        }

        if (cooldownFrames_ > 0) {
            --cooldownFrames_;
            maybeLogNoTarget(detections, "cooldown");
            return nullptr;
        }

        std::vector<TargetCandidate> candidates =
                buildCandidates(detections, kTargetAcquireMinConfidence);
        const TargetCandidate* candidate = findAcquireTarget(candidates);
        if (!candidate) {
            clearPending();
            maybeLogNoTarget(detections, "no_candidate");
            return nullptr;
        }
        updatePending(*candidate);
        if (pendingFrames_ < kTargetAcquireFrames) {
            maybeLogNoTarget(detections, "pending");
            return nullptr;
        }
        commitAcquire(pending_);
        return &stableAim_;
    }

    bool consumeTouchReset() {
        const bool reset = resetTouchFilterPending_;
        resetTouchFilterPending_ = false;
        return reset;
    }

    float touchDragScale(const TouchTuning& tuning) const {
        if (touchWarmupFrames_ > 0) {
            return tuning.warmupDragScale;
        }
        return tuning.dragScale;
    }

    void onTouchAimSent() {
        if (touchWarmupFrames_ > 0) {
            --touchWarmupFrames_;
        }
    }

    void reset() {
        locked_ = false;
        stableGroupValid_ = false;
        missedFrames_ = 0;
        cooldownFrames_ = 0;
        lastLostValid_ = false;
        lastLostFrames_ = 0;
        headMissFrames_ = 0;
        lastHeadValid_ = false;
        touchWarmupFrames_ = 0;
        resetTouchFilterPending_ = true;
        clearPending();
        clearSwitchPending();
        stableGroup_ = Detection {};
        stableAim_ = Detection {};
        lastHead_ = Detection {};
        lastLostGroup_ = Detection {};
        lastLostAim_ = Detection {};
    }

private:
    struct TargetCandidate {
        Detection group {};
        Detection aim {};
        bool hasBody = false;
        bool hasHead = false;
        bool headInsideBody = false;
    };

    static void smoothDetection(Detection* out, const Detection& next, float alpha) {
        out->left += (next.left - out->left) * alpha;
        out->top += (next.top - out->top) * alpha;
        out->right += (next.right - out->right) * alpha;
        out->bottom += (next.bottom - out->bottom) * alpha;
        out->confidence = next.confidence;
        out->cls = next.cls;
    }

    static void adoptOrSmoothDetection(Detection* out, const Detection& next, float alpha) {
        if (out->cls != next.cls) {
            *out = next;
            return;
        }
        smoothDetection(out, next, alpha);
    }

    static bool isBody(const Detection& d) {
        return d.cls == kTargetBodyClass;
    }

    static bool isHead(const Detection& d) {
        return d.cls == kTargetHeadClass;
    }

    bool isCandidate(const Detection& d, float minConfidence) const {
        return (isBody(d) || isHead(d)) && d.confidence >= minConfidence;
    }

    float screenDistance2(const Detection& d) const {
        const float dx = d.cx() - width_ * 0.5f;
        const float dy = d.cy() - height_ * 0.5f;
        return dx * dx + dy * dy;
    }

    static float centerDistance2(const Detection& a, const Detection& b) {
        const float dx = a.cx() - b.cx();
        const float dy = a.cy() - b.cy();
        return dx * dx + dy * dy;
    }

    float containTolerance() const {
        return std::max(6.0f, minDimension() * kTargetContainToleranceRatio);
    }

    bool containsCenter(const Detection& body, const Detection& head) const {
        const float tolerance = containTolerance();
        const float x = head.cx();
        const float y = head.cy();
        return x >= body.left - tolerance &&
               x <= body.right + tolerance &&
               y >= body.top - tolerance &&
               y <= body.bottom + tolerance;
    }

    float targetBodyPenalty2() const {
        const float penalty = minDimension() * kTargetBodyFallbackPenaltyRatio;
        return penalty * penalty;
    }

    float targetTrackGate() const {
        return std::max(24.0f, minDimension() * kTargetGateRatio);
    }

    int minDimension() const {
        return std::max(1, std::min(width_, height_));
    }

    void ageLastLost() {
        if (!locked_ && lastLostValid_) {
            ++lastLostFrames_;
            if (lastLostFrames_ > kTargetLastLostMaxFrames) {
                logf("I",
                     "target last lost expired frames=%d last_group_cls=%d last_group_center=%.1f,%.1f",
                     lastLostFrames_,
                     lastLostGroup_.cls,
                     lastLostGroup_.cx(),
                     lastLostGroup_.cy());
                lastLostValid_ = false;
            }
        }
    }

    const Detection* findBestContainedHeadForBody(
            const Detection& body,
            const std::vector<Detection>& detections,
            float minConfidence) const {
        const Detection* best = nullptr;
        float bestScore = std::numeric_limits<float>::max();
        for (const Detection& d : detections) {
            if (!isHead(d) || d.confidence < minConfidence || !containsCenter(body, d)) {
                continue;
            }
            const float score = screenDistance2(d) + centerDistance2(body, d) * kTargetCenterWeight;
            if (score < bestScore) {
                bestScore = score;
                best = &d;
            }
        }
        return best;
    }

    const Detection* findContainingBodyForHead(
            const Detection& head,
            const std::vector<Detection>& detections,
            float minConfidence) const {
        const Detection* best = nullptr;
        float bestScore = std::numeric_limits<float>::max();
        for (const Detection& d : detections) {
            if (!isBody(d) || d.confidence < minConfidence || !containsCenter(d, head)) {
                continue;
            }
            const float score = centerDistance2(head, d) + screenDistance2(d) * kTargetCenterWeight;
            if (score < bestScore) {
                bestScore = score;
                best = &d;
            }
        }
        return best;
    }

    std::vector<TargetCandidate> buildCandidates(
            const std::vector<Detection>& detections,
            float minConfidence) const {
        std::vector<TargetCandidate> candidates;
        candidates.reserve(detections.size());

        for (const Detection& d : detections) {
            if (!isBody(d) || d.confidence < minConfidence) {
                continue;
            }
            TargetCandidate candidate;
            candidate.group = d;
            candidate.aim = d;
            candidate.hasBody = true;
            const Detection* head = findBestContainedHeadForBody(d, detections, minConfidence);
            if (head) {
                candidate.aim = *head;
                candidate.hasHead = true;
                candidate.headInsideBody = true;
            }
            candidates.push_back(candidate);
        }

        for (const Detection& d : detections) {
            if (!isHead(d) || d.confidence < minConfidence) {
                continue;
            }
            if (findContainingBodyForHead(d, detections, minConfidence)) {
                continue;
            }
            TargetCandidate candidate;
            candidate.group = d;
            candidate.aim = d;
            candidate.hasHead = true;
            candidates.push_back(candidate);
        }
        return candidates;
    }

    bool sameCandidateGroup(const TargetCandidate& a, const TargetCandidate& b) const {
        const float gate = targetTrackGate();
        const float gate2 = gate * gate;
        if (a.hasBody && b.hasBody) {
            return centerDistance2(a.group, b.group) <= gate2;
        }
        if (a.hasBody && b.hasHead) {
            return containsCenter(a.group, b.aim) ||
                   centerDistance2(a.group, b.aim) <= gate2;
        }
        if (b.hasBody && a.hasHead) {
            return containsCenter(b.group, a.aim) ||
                   centerDistance2(b.group, a.aim) <= gate2;
        }
        return centerDistance2(a.aim, b.aim) <= gate2;
    }

    bool isSameStableGroup(const TargetCandidate& candidate) const {
        if (!stableGroupValid_) {
            return false;
        }
        const float gate = targetTrackGate();
        const float gate2 = gate * gate;
        if (isBody(stableGroup_)) {
            if (candidate.hasBody && centerDistance2(candidate.group, stableGroup_) <= gate2) {
                return true;
            }
            if (candidate.hasHead &&
                (containsCenter(stableGroup_, candidate.aim) ||
                 centerDistance2(candidate.aim, stableAim_) <= gate2 ||
                 (lastHeadValid_ && centerDistance2(candidate.aim, lastHead_) <= gate2))) {
                return true;
            }
            return false;
        }

        if (candidate.hasBody && containsCenter(candidate.group, stableAim_)) {
            return true;
        }
        return centerDistance2(candidate.aim, stableAim_) <= gate2;
    }

    float sameStableGroupScore(const TargetCandidate& candidate) const {
        float continuity = centerDistance2(candidate.aim, stableAim_);
        if (isBody(stableGroup_) && candidate.hasBody) {
            continuity = std::min(continuity, centerDistance2(candidate.group, stableGroup_));
        }
        return continuity + screenDistance2(candidate.aim) * kTargetCenterWeight;
    }

    const TargetCandidate* findTrackedGroup(
            const std::vector<TargetCandidate>& candidates) const {
        const TargetCandidate* best = nullptr;
        float bestScore = std::numeric_limits<float>::max();
        for (const TargetCandidate& candidate : candidates) {
            if (!isSameStableGroup(candidate)) {
                continue;
            }
            const float score = sameStableGroupScore(candidate);
            if (score < bestScore) {
                bestScore = score;
                best = &candidate;
            }
        }
        return best;
    }

    float lastLostGroupDistance2(const TargetCandidate& candidate) const {
        return centerDistance2(candidate.group, lastLostGroup_);
    }

    float acquireScore(const TargetCandidate& candidate) const {
        float score = screenDistance2(candidate.aim);
        if (!candidate.hasHead) {
            score += targetBodyPenalty2();
        }
        if (lastLostValid_) {
            score += lastLostGroupDistance2(candidate) * kTargetLastLostWeight;
        }
        return score;
    }

    const TargetCandidate* findAcquireTarget(
            const std::vector<TargetCandidate>& candidates) {
        const TargetCandidate* best = nullptr;
        float bestScore = std::numeric_limits<float>::max();

        for (const TargetCandidate& candidate : candidates) {
            const float score = acquireScore(candidate);
            if (score < bestScore) {
                bestScore = score;
                best = &candidate;
            }
        }

        return best;
    }

    void updatePending(const TargetCandidate& candidate) {
        if (!pendingValid_ || !sameCandidateGroup(pending_, candidate)) {
            pending_ = candidate;
            pendingFrames_ = 1;
            pendingValid_ = true;
            return;
        }
        smoothCandidate(&pending_, candidate, 0.50f);
        ++pendingFrames_;
    }

    void smoothCandidate(TargetCandidate* out, const TargetCandidate& next, float alpha) const {
        if (out->group.cls == next.group.cls) {
            smoothDetection(&out->group, next.group, alpha);
        } else {
            out->group = next.group;
        }
        if (out->aim.cls == next.aim.cls) {
            smoothDetection(&out->aim, next.aim, alpha);
        } else {
            out->aim = next.aim;
        }
        out->hasBody = next.hasBody;
        out->hasHead = next.hasHead;
        out->headInsideBody = next.headInsideBody;
    }

    void clearPending() {
        pending_ = TargetCandidate {};
        pendingFrames_ = 0;
        pendingValid_ = false;
    }

    void clearSwitchPending() {
        switchPending_ = TargetCandidate {};
        switchPendingFrames_ = 0;
        switchPendingValid_ = false;
    }

    void updateLastHeadFromAim() {
        if (isHead(stableAim_)) {
            lastHead_ = stableAim_;
            lastHeadValid_ = true;
            headMissFrames_ = 0;
        }
    }

    void updateLockedTarget(const TargetCandidate& candidate) {
        if (candidate.hasBody || !stableGroupValid_ || !isBody(stableGroup_)) {
            if (stableGroupValid_) {
                adoptOrSmoothDetection(&stableGroup_, candidate.group, kTargetSmoothAlpha);
            } else {
                stableGroup_ = candidate.group;
                stableGroupValid_ = true;
            }
        }

        if (candidate.hasHead) {
            adoptOrSmoothDetection(&stableAim_, candidate.aim, kTargetSmoothAlpha);
            updateLastHeadFromAim();
            return;
        }

        if (lastHeadValid_ && headMissFrames_ < kTargetHeadHoldFrames) {
            ++headMissFrames_;
            stableAim_ = lastHead_;
            return;
        }
        adoptOrSmoothDetection(&stableAim_, candidate.aim, kTargetSmoothAlpha);
    }

    void commitAcquire(const TargetCandidate& candidate) {
        stableGroup_ = candidate.group;
        stableAim_ = candidate.aim;
        stableGroupValid_ = true;
        locked_ = true;
        missedFrames_ = 0;
        headMissFrames_ = 0;
        lastHeadValid_ = false;
        updateLastHeadFromAim();
        clearPending();
        clearSwitchPending();
        resetTouchFilterPending_ = true;
        touchWarmupFrames_ = currentTouchWarmupFrames_;

        const float screenDistance = sqrtf(screenDistance2(stableAim_));
        const float reacquireDistance = lastLostValid_
                ? sqrtf(centerDistance2(stableGroup_, lastLostGroup_))
                : -1.0f;
        logf("I",
             "target lock acquire group_cls=%d aim_cls=%d group_center=%.1f,%.1f aim_center=%.1f,%.1f aim_bbox=%.1f,%.1f,%.1f,%.1f screen_dist=%.1f reacquire_dist=%.1f has_head=%d c1_inside_c0=%d",
             stableGroup_.cls,
             stableAim_.cls,
             stableGroup_.cx(),
             stableGroup_.cy(),
             stableAim_.cx(),
             stableAim_.cy(),
             stableAim_.left,
             stableAim_.top,
             stableAim_.right,
             stableAim_.bottom,
             screenDistance,
             reacquireDistance,
             candidate.hasHead ? 1 : 0,
             candidate.headInsideBody ? 1 : 0);
        lastLostValid_ = false;
    }

    bool maybeCommitGroupSwitch(const std::vector<TargetCandidate>& candidates) {
        const float currentDistance2 = std::max(screenDistance2(stableAim_), 1.0f);
        const TargetCandidate* best = nullptr;
        float bestDistance2 = std::numeric_limits<float>::max();
        for (const TargetCandidate& candidate : candidates) {
            if (isSameStableGroup(candidate)) {
                continue;
            }
            const float distance2 = screenDistance2(candidate.aim);
            if (distance2 > currentDistance2 * kTargetGroupSwitchDistanceRatio) {
                continue;
            }
            if (distance2 < bestDistance2) {
                bestDistance2 = distance2;
                best = &candidate;
            }
        }

        if (!best) {
            clearSwitchPending();
            return false;
        }

        if (!switchPendingValid_ || !sameCandidateGroup(switchPending_, *best)) {
            switchPending_ = *best;
            switchPendingFrames_ = 1;
            switchPendingValid_ = true;
            logf("I",
                 "target lock switch pending frames=%d/%d group_cls=%d aim_cls=%d aim_center=%.1f,%.1f current_dist=%.1f candidate_dist=%.1f",
                 switchPendingFrames_,
                 kTargetGroupSwitchFrames,
                 best->group.cls,
                 best->aim.cls,
                 best->aim.cx(),
                 best->aim.cy(),
                 sqrtf(currentDistance2),
                 sqrtf(bestDistance2));
            return false;
        }

        smoothCandidate(&switchPending_, *best, 0.50f);
        ++switchPendingFrames_;
        if (switchPendingFrames_ < kTargetGroupSwitchFrames) {
            return false;
        }

        logf("I",
             "target lock switch commit group_cls=%d->%d aim_cls=%d->%d group_center=%.1f,%.1f->%.1f,%.1f aim_center=%.1f,%.1f->%.1f,%.1f frames=%d",
             stableGroup_.cls,
             switchPending_.group.cls,
             stableAim_.cls,
             switchPending_.aim.cls,
             stableGroup_.cx(),
             stableGroup_.cy(),
             switchPending_.group.cx(),
             switchPending_.group.cy(),
             stableAim_.cx(),
             stableAim_.cy(),
             switchPending_.aim.cx(),
             switchPending_.aim.cy(),
             switchPendingFrames_);
        stableGroup_ = switchPending_.group;
        stableAim_ = switchPending_.aim;
        stableGroupValid_ = true;
        missedFrames_ = 0;
        headMissFrames_ = 0;
        lastHeadValid_ = false;
        updateLastHeadFromAim();
        resetTouchFilterPending_ = true;
        touchWarmupFrames_ = currentTouchWarmupFrames_;
        clearSwitchPending();
        return true;
    }

    void markLost() {
        lastLostGroup_ = stableGroup_;
        lastLostAim_ = stableAim_;
        lastLostValid_ = true;
        lastLostFrames_ = 0;
        logf("I",
             "target lock lost group_cls=%d aim_cls=%d group_center=%.1f,%.1f aim_center=%.1f,%.1f miss=%d track_gate=%.1f",
             stableGroup_.cls,
             stableAim_.cls,
             stableGroup_.cx(),
             stableGroup_.cy(),
             stableAim_.cx(),
             stableAim_.cy(),
             missedFrames_,
             targetTrackGate());
        locked_ = false;
        stableGroupValid_ = false;
        missedFrames_ = 0;
        stableGroup_ = Detection {};
        stableAim_ = Detection {};
        headMissFrames_ = 0;
        lastHeadValid_ = false;
        clearPending();
        clearSwitchPending();
        cooldownFrames_ = kTargetReacquireCooldownFrames;
    }

    void maybeLogNoTarget(const std::vector<Detection>& detections, const char* reason) {
        if (detections.empty()) return;
        const auto now = Clock::now();
        if (now < nextDiagnosticLog_) return;
        nextDiagnosticLog_ = now + std::chrono::milliseconds(kTargetDiagnosticIntervalMs);

        int cls0Count = 0;
        int cls1Count = 0;
        int candidateCount = 0;
        const Detection* nearest = nullptr;
        const Detection* nearestCandidate = nullptr;
        float nearestDistance2 = std::numeric_limits<float>::max();
        float nearestCandidateDistance2 = std::numeric_limits<float>::max();
        for (const Detection& d : detections) {
            const float distance2 = screenDistance2(d);
            if (distance2 < nearestDistance2) {
                nearestDistance2 = distance2;
                nearest = &d;
            }
            if (d.cls == kTargetBodyClass) {
                ++cls0Count;
            } else if (d.cls == kTargetHeadClass) {
                ++cls1Count;
            }
            if (isCandidate(d, kTargetAcquireMinConfidence)) {
                ++candidateCount;
                if (distance2 < nearestCandidateDistance2) {
                    nearestCandidateDistance2 = distance2;
                    nearestCandidate = &d;
                }
            }
        }

        const float nearestDistance = nearest ? sqrtf(nearestDistance2) : -1.0f;
        const float candidateDistance = nearestCandidate ? sqrtf(nearestCandidateDistance2) : -1.0f;
        const float candidateLastDistance = (nearestCandidate && lastLostValid_)
                ? sqrtf(centerDistance2(*nearestCandidate, lastLostAim_))
                : -1.0f;
        const Detection* candidateContainingBody =
                (nearestCandidate && isHead(*nearestCandidate))
                        ? findContainingBodyForHead(*nearestCandidate, detections,
                                                    kTargetAcquireMinConfidence)
                        : nullptr;
        logf("I",
             "target no lock reason=%s det=%zu cls0=%d cls1=%d candidate=%d nearest_cls=%d nearest_conf=%.3f nearest_dist=%.1f candidate_cls=%d candidate_conf=%.3f candidate_dist=%.1f pending=%d/%d cooldown=%d last_valid=%d last_frames=%d candidate_last_dist=%.1f candidate_c1_inside_c0=%d",
             reason,
             detections.size(),
             cls0Count,
             cls1Count,
             candidateCount,
             nearest ? nearest->cls : -1,
             nearest ? nearest->confidence : 0.0f,
             nearestDistance,
             nearestCandidate ? nearestCandidate->cls : -1,
             nearestCandidate ? nearestCandidate->confidence : 0.0f,
             candidateDistance,
             pendingFrames_,
             kTargetAcquireFrames,
             cooldownFrames_,
             lastLostValid_ ? 1 : 0,
             lastLostFrames_,
             candidateLastDistance,
             candidateContainingBody ? 1 : 0);
    }

    TargetCandidate pending_ {};
    TargetCandidate switchPending_ {};
    Detection stableGroup_ {};
    Detection stableAim_ {};
    Detection lastHead_ {};
    Detection lastLostGroup_ {};
    Detection lastLostAim_ {};
    int width_ = 0;
    int height_ = 0;
    int missedFrames_ = 0;
    int pendingFrames_ = 0;
    int cooldownFrames_ = 0;
    int headMissFrames_ = 0;
    int lastLostFrames_ = 0;
    int switchPendingFrames_ = 0;
    int touchWarmupFrames_ = 0;
    int currentTouchWarmupFrames_ = kTouchWarmupFrames;
    bool locked_ = false;
    bool stableGroupValid_ = false;
    bool pendingValid_ = false;
    bool lastHeadValid_ = false;
    bool lastLostValid_ = false;
    bool switchPendingValid_ = false;
    bool resetTouchFilterPending_ = false;
    TimePoint nextDiagnosticLog_ {};
};

void translateDetections(std::vector<Detection>* detections, int dx, int dy) {
    if (!detections || (dx == 0 && dy == 0)) return;
    for (Detection& d : *detections) {
        d.left += dx;
        d.right += dx;
        d.top += dy;
        d.bottom += dy;
    }
}

void logKgslPagetableState(const char* phase) {
    char path[160];
    snprintf(path, sizeof(path), "/sys/devices/virtual/kgsl/kgsl/pagetables/%d", getpid());
    struct stat st {};
    if (stat(path, &st) == 0) {
        logf("I", "kgsl_pagetable=present phase=%s path=%s", phase, path);
        return;
    }
    logf("I", "kgsl_pagetable=absent phase=%s path=%s errno=%d", phase, path, errno);
}

void stopRuntimeComponents(
        ScreenCaptureProcess* capture,
        OverlayRenderer* overlay,
        std::unique_ptr<TouchController>* touch) {
    if (touch && *touch) {
        (*touch)->stop();
        touch->reset();
    }
    if (overlay) {
        overlay->stop();
    }
    if (capture) {
        capture->stop();
    }
}

bool startRuntimeComponents(
        JNIEnv* env,
        const Options& opts,
        Font* font,
        ScreenCaptureProcess* capture,
        OverlayRenderer* overlay,
        std::unique_ptr<TouchController>* touch) {
    if (!capture->start(opts.screenWidth, opts.screenHeight, opts.inputSize)) {
        return false;
    }
    if (!overlay->start(env, opts.screenWidth, opts.screenHeight, font)) {
        capture->stop();
        return false;
    }
    if (touch) {
        touch->reset();
        if (opts.touch) {
            auto nextTouch = std::make_unique<TouchController>();
            if (!nextTouch->start(opts)) {
                overlay->stop();
                capture->stop();
                return false;
            }
            *touch = std::move(nextTouch);
        }
    }
    logKgslPagetableState(capture->backend().c_str());
    return true;
}

bool reconfigureRuntimeForDisplay(
        JNIEnv* env,
        const DisplayState& state,
        Options* opts,
        Font* font,
        ScreenCaptureProcess* capture,
        OverlayRenderer* overlay,
        std::unique_ptr<TouchController>* touch) {
    const DisplayState previous = displayStateFromOptions(*opts);
    logf("I", "display changed old=%dx%d rot=%d new=%dx%d rot=%d",
         previous.width, previous.height, previous.rotation,
         state.width, state.height, state.rotation);
    stopRuntimeComponents(capture, overlay, touch);
    applyDisplayState(state, opts);
    if (!startRuntimeComponents(env, *opts, font, capture, overlay, touch)) {
        logf("E", "display reconfigure failed size=%dx%d rotation=%d",
             opts->screenWidth, opts->screenHeight, opts->rotation);
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const LaunchArgs launchArgs = parseLaunchArgs(argc, argv);

    if (!startDaemonProcess()) {
        return 1;
    }
    configureSignals();

    PidFileGuard pidFile(kPidFilePath);
    if (!pidFile.write()) {
        return 1;
    }

    Options opts;
    logf("I", "kaiagentd start pid=%d", getpid());
    applyModelVariant(kModelVariants[kDefaultModelVariantIndex], &opts);

    JvmContext jvm {};
    if (!jvm_create(jvm)) {
        logf("E", "jvm_create failed");
        return 6;
    }
    if (!resolveDisplayState(jvm.env, &opts)) {
        return 1;
    }
    if (launchArgs.captureSmokeTest) {
        const bool ok = runCaptureSmokeTest(opts);
        return ok ? 0 : 10;
    }
    if (launchArgs.captureBenchmark) {
        ScreenrecordRawCaptureBackend backend;
        if (!captureBackendFromName(launchArgs.captureBackend, &backend)) {
            logf("E", "unknown capture benchmark backend: %s",
                 launchArgs.captureBackend.c_str());
            return 10;
        }
        const bool ok = runCaptureBenchmark(opts, backend, launchArgs.captureFrames);
        return ok ? 0 : 10;
    }

    Font font;
    if (!font.loadFromFile("/system/fonts/NotoSansCJK-Regular.ttc", 18.0f) &&
        !font.loadFromFile("/system/fonts/NotoSerifCJK-Regular.ttc", 18.0f)) {
        logf("E", "failed to load required Chinese font");
        return 4;
    }

    OverlayRenderer overlay;
    auto detector = std::make_unique<Detector>(opts);
    if (!detector->init()) {
        return 2;
    }

    ScreenCaptureProcess capture;
    std::unique_ptr<TouchController> touch;
    if (!startRuntimeComponents(jvm.env, opts, &font, &capture, &overlay, &touch)) {
        return 5;
    }

    TargetTracker targetTracker;
    TouchTuning touchTuning;
    ModelMenuState modelMenu = buildModelMenuState(opts);
    DisplayState activeDisplay = displayStateFromOptions(opts);
    std::vector<uint8_t> frame;
    std::vector<Detection> detections;
    auto lastFrame = Clock::now();
    auto fpsStart = lastFrame;
    auto nextDisplayPoll = lastFrame + std::chrono::milliseconds(kDisplayPollIntervalMs);
    auto nextOverlay = lastFrame;
    TimingAccumulator perfStats;
    int fpsFrames = 0;
    double fps = 0.0;
    int exitCode = 0;

    auto resetTiming = [&]() {
        lastFrame = Clock::now();
        fpsStart = lastFrame;
        nextDisplayPoll = lastFrame + std::chrono::milliseconds(kDisplayPollIntervalMs);
        nextOverlay = lastFrame;
        perfStats.reset(lastFrame);
        fpsFrames = 0;
        fps = 0.0;
    };

    auto switchModel = [&](int requestedIndex) -> bool {
        modelMenu.requested = -1;
        if (requestedIndex < 0 || requestedIndex >= kModelVariantCount ||
            requestedIndex == modelMenu.active) {
            return false;
        }
        if (!isModelVariantReadable(opts, requestedIndex)) {
            modelMenu.status = std::string("缺失: ") + modelVariantLabel(requestedIndex);
            logf("W", "model switch unavailable index=%d label=%s",
                 requestedIndex, modelVariantLabel(requestedIndex));
            return false;
        }

        const Options previousOpts = opts;
        Options nextOpts = opts;
        applyModelVariant(kModelVariants[requestedIndex], &nextOpts);

        modelMenu.switching = true;
        modelMenu.status = std::string("切换中: ") + modelVariantLabel(requestedIndex);
        if (touch) {
            touch->stopAim();
        }
        logf("I", "model switch begin from=%s to=%s",
             modelVariantLabel(modelMenu.active), modelVariantLabel(requestedIndex));

        auto nextDetector = std::make_unique<Detector>(nextOpts);
        if (!nextDetector->init()) {
            modelMenu.switching = false;
            modelMenu.status = std::string("切换失败: ") + modelVariantLabel(requestedIndex);
            logf("E", "model switch detector init failed label=%s",
                 modelVariantLabel(requestedIndex));
            return false;
        }

        if (nextOpts.inputSize != previousOpts.inputSize) {
            if (!capture.start(nextOpts.screenWidth, nextOpts.screenHeight,
                               nextOpts.inputSize)) {
                logf("E", "model switch capture restart failed label=%s input=%d",
                     modelVariantLabel(requestedIndex), nextOpts.inputSize);
                if (!capture.start(previousOpts.screenWidth, previousOpts.screenHeight,
                                   previousOpts.inputSize)) {
                    logf("E", "model switch failed to restore previous capture input=%d",
                         previousOpts.inputSize);
                    exitCode = 8;
                    g_stop.store(true);
                }
                modelMenu.switching = false;
                modelMenu.status = std::string("采集失败: ") + modelVariantLabel(requestedIndex);
                return false;
            }
        }

        opts = nextOpts;
        detector = std::move(nextDetector);
        modelMenu.active = requestedIndex;
        modelMenu.switching = false;
        modelMenu.status = std::string("当前: ") + modelVariantLabel(requestedIndex);
        frame.clear();
        detections.clear();
        targetTracker.reset();
        if (touch) {
            touch->resetAimFilter();
        }
        resetTiming();
        logf("I", "model switch ok label=%s file=%s input=%d boxes=%d split=%d precision=%s",
             modelVariantLabel(requestedIndex),
             opts.modelFile.c_str(),
             opts.inputSize,
             opts.numBoxes,
             opts.splitOutput ? 1 : 0,
             opts.precision.c_str());
        return true;
    };

    auto pollDisplayState = [&](bool force, bool* changed) -> bool {
        if (changed) {
            *changed = false;
        }
        const auto now = Clock::now();
        if (!force && now < nextDisplayPoll) {
            return true;
        }
        nextDisplayPoll = now + std::chrono::milliseconds(kDisplayPollIntervalMs);

        DisplayState latest {};
        if (!readNativeDisplayState(jvm.env, &latest)) {
            return true;
        }
        if (sameDisplayState(latest, activeDisplay)) {
            return true;
        }
        if (!reconfigureRuntimeForDisplay(jvm.env, latest, &opts, &font,
                                          &capture, &overlay, &touch)) {
            return false;
        }
        activeDisplay = latest;
        frame.clear();
        detections.clear();
        targetTracker.reset();
        resetTiming();
        if (changed) {
            *changed = true;
        }
        return true;
    };

    while (!g_stop.load()) {
        FrameTiming timing;
        const auto frameStart = Clock::now();

        bool displayChanged = false;
        auto stageStart = Clock::now();
        if (!pollDisplayState(false, &displayChanged)) {
            exitCode = 7;
            break;
        }
        timing.displayMs = elapsedMs(stageStart);
        if (displayChanged) {
            continue;
        }

        stageStart = Clock::now();
        if (!capture.readFrame(&frame)) {
            if (!g_stop.load()) {
                bool changedAfterReadFailure = false;
                if (!pollDisplayState(true, &changedAfterReadFailure)) {
                    exitCode = 7;
                    break;
                }
                if (changedAfterReadFailure) {
                    continue;
                }
            }
            break;
        }
        timing.captureMs = elapsedMs(stageStart);

        auto now = Clock::now();
        const auto sinceLast = std::chrono::duration_cast<std::chrono::microseconds>(now - lastFrame);
        if (sinceLast.count() < kFrameIntervalUs) {
            stageStart = Clock::now();
            std::this_thread::sleep_for(
                    std::chrono::microseconds(kFrameIntervalUs - sinceLast.count()));
            timing.throttleMs = elapsedMs(stageStart);
        }
        lastFrame = Clock::now();

        int cropLeft = 0;
        int cropTop = 0;
        int cropSide = 0;
        DetectorTiming detectorTiming;
        if (!detector->run(frame, capture.width(), capture.height(),
                           &cropLeft, &cropTop, &cropSide, &detections, &detectorTiming)) {
            break;
        }
        timing.detector = detectorTiming;
        const int screenCropLeft = capture.offsetX() + cropLeft;
        const int screenCropTop = capture.offsetY() + cropTop;
        translateDetections(&detections, capture.offsetX(), capture.offsetY());

        KTouchState menuTouchState {};
        TouchAimStatus touchAimStatus {};
        if (touch) {
            menuTouchState = touch->touchState();
            touchAimStatus = touch->aimStatus();
        }

        stageStart = Clock::now();
        const Detection* target = targetTracker.update(
                detections, capture.screenWidth(), capture.screenHeight(), touchTuning);
        timing.touchMs = elapsedMs(stageStart);

        ++fpsFrames;
        auto fpsElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(lastFrame - fpsStart);
        if (fpsElapsed.count() >= 1000) {
            fps = fpsFrames * 1000.0 / fpsElapsed.count();
            fpsFrames = 0;
            fpsStart = lastFrame;
        }

        const auto overlayNow = Clock::now();
        bool menuCaptured = overlay.menuTouchCaptured();
        const bool touchChanged = menuTouchState.down != 0 ||
                                  menuTouchState.pressed != 0 ||
                                  menuTouchState.released != 0;
        stageStart = Clock::now();
        if (overlayNow >= nextOverlay || touchChanged) {
            menuCaptured = overlay.render(detections, target, screenCropLeft, screenCropTop,
                                          cropSide, fps, menuTouchState, touchAimStatus,
                                          &touchTuning, &modelMenu);
            nextOverlay = overlayNow + std::chrono::milliseconds(kOverlayIntervalMs);
            timing.overlayMs = elapsedMs(stageStart);
        }

        bool modelSwitched = false;
        if (modelMenu.requested >= 0) {
            modelSwitched = switchModel(modelMenu.requested);
            if (modelSwitched) {
                target = nullptr;
                menuCaptured = true;
            }
        }

        stageStart = Clock::now();
        if (touch) {
            if (menuCaptured || modelSwitched) {
                touch->stopAim();
            } else if (target) {
                if (targetTracker.consumeTouchReset()) {
                    touch->resetAimFilter();
                }
                const bool sent = touch->aim(capture.screenWidth(), capture.screenHeight(), *target,
                                             touchTuning, targetTracker.touchDragScale(touchTuning));
                if (sent) {
                    targetTracker.onTouchAimSent();
                }
            } else {
                touch->stopAim();
            }
        }
        timing.touchMs += elapsedMs(stageStart);
        timing.totalMs = elapsedMs(frameStart);
        timing.detections = detections.size();
        timing.hasTarget = target != nullptr;
        perfStats.add(timing);
        perfStats.maybeLog(fps, opts, capture.width(), capture.height());
    }

    logf("I", "kaiagentd stopping");
    stopRuntimeComponents(&capture, &overlay, &touch);
    logf("I", "kaiagentd stopped");
    return exitCode;
}
