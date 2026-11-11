#include "raw_capture.h"

#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <errno.h>
#include <media/NdkImageReader.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace android {

using status_t = int32_t;
using nsecs_t = int64_t;

static constexpr status_t NO_ERROR = 0;
static constexpr status_t UNKNOWN_ERROR = 0x80000000;
static constexpr status_t INVALID_OPERATION = -38;

class IBinder;
class CpuConsumer;
class IGraphicBufferConsumer;
class IGraphicBufferProducer;
class MediaCodec;
class PersistentSurface;
class ProcessState;
class Surface;
class SurfaceComposerClient;
class SurfaceControl;

struct String8 {
    const char* mString = nullptr;
};

template <typename T>
class sp {
public:
    sp() = default;
    sp(const sp&) = default;
    sp& operator=(const sp&) = default;
    ~sp() {}

    T* get() const { return mPtr; }
    explicit operator bool() const { return mPtr != nullptr; }

private:
    T* mPtr = nullptr;
};

struct DisplayId {
    uint64_t value = 0;
    static constexpr DisplayId fromValue(uint64_t v) { return DisplayId{v}; }
};

struct PhysicalDisplayId : DisplayId {
    static constexpr PhysicalDisplayId fromValue(uint64_t v) {
        PhysicalDisplayId id;
        id.value = v;
        return id;
    }
};

struct Rect {
    int32_t left = 0;
    int32_t top = 0;
    int32_t right = 0;
    int32_t bottom = 0;

    Rect() = default;
    Rect(int32_t l, int32_t t, int32_t r, int32_t b)
        : left(l), top(t), right(r), bottom(b) {}

    int32_t getWidth() const { return right - left; }
    int32_t getHeight() const { return bottom - top; }
};

namespace ui {

struct LayerStack {
    uint32_t id = UINT32_MAX;
};

enum class Rotation : int32_t {
    Rotation0 = 0,
    Rotation90 = 1,
    Rotation180 = 2,
    Rotation270 = 3,
};

static constexpr Rotation ROTATION_0 = Rotation::Rotation0;
static constexpr Rotation ROTATION_90 = Rotation::Rotation90;

struct Size {
    int32_t width = -1;
    int32_t height = -1;

    int32_t getWidth() const { return width; }
    int32_t getHeight() const { return height; }
};

struct DisplayState {
    LayerStack layerStack;
    Rotation orientation = ROTATION_0;
    Size layerStackSpaceRect;
};

enum class Hdr : int32_t {};

using DisplayModeId = int32_t;

struct DisplayMode {
    DisplayModeId id = 0;
    Size resolution;
    float xDpi = 0.0f;
    float yDpi = 0.0f;
    std::vector<Hdr> supportedHdrTypes;
    float peakRefreshRate = 0.0f;
    float vsyncRate = 0.0f;
    nsecs_t appVsyncOffset = 0;
    nsecs_t sfVsyncOffset = 0;
    nsecs_t presentationDeadline = 0;
    int32_t group = -1;
};

}  // namespace ui
}  // namespace android

namespace {

static constexpr int32_t kHalPixelFormatRgba8888 = 1;
static constexpr int32_t kHalPixelFormatRgbx8888 = 2;
static constexpr int32_t kHalPixelFormatRgb888 = 3;
static constexpr int32_t kHalPixelFormatRgb565 = 4;
static constexpr int32_t kHalPixelFormatBgra8888 = 5;
static constexpr uint32_t kInvalidLayerStack = UINT32_MAX;
static constexpr size_t kCpuConsumerStorageBytes = 65536;
// AImageReader returns the ANativeWindow base subobject; libgui's Surface hooks
// adjust the pointer by this ABI offset before accessing Surface fields.
static constexpr ptrdiff_t kSurfaceNativeWindowOffsetBytes = 16;

struct Options {
    bool verbose = false;
    bool rotate = false;
    bool captureSecure = false;
    ScreenrecordRawCaptureBackend backend =
            SCREENRECORD_RAW_CAPTURE_BACKEND_CPU_CONSUMER;
    bool rawCropSpecified = false;
    uint32_t rawCropLeft = 0;
    uint32_t rawCropTop = 0;
    uint32_t rawCropWidth = 0;
    uint32_t rawCropHeight = 0;
    uint32_t videoWidth = 0;
    uint32_t videoHeight = 0;
    bool displayIdSpecified = false;
    android::PhysicalDisplayId displayId;
};

struct CpuConsumerLockedBuffer {
    uint8_t* data = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    int32_t format = 0;
    uint32_t stride = 0;
    android::Rect crop;
    uint32_t transform = 0;
    uint32_t scalingMode = 0;
    int64_t timestamp = 0;
    int32_t dataSpace = 0;
    uint64_t frameNumber = 0;
    int32_t flexFormat = 0;
    uint8_t* dataCb = nullptr;
    uint8_t* dataCr = nullptr;
    uint32_t chromaStride = 0;
    uint32_t chromaStep = 0;
};

static volatile sig_atomic_t gStopRequested = 0;

static uint32_t floorToEven(uint32_t value) {
    return value & ~1U;
}

struct GuiApi {
    void* libgui = nullptr;
    void* libutils = nullptr;

    using GetPhysicalDisplayIdsFn = std::vector<android::PhysicalDisplayId> (*)();
    using GetPhysicalDisplayTokenFn =
            android::sp<android::IBinder> (*)(android::PhysicalDisplayId);
    using GetActiveDisplayModeFn =
            android::status_t (*)(const android::sp<android::IBinder>&,
                                  android::ui::DisplayMode*);
    using GetDisplayStateFn =
            android::status_t (*)(const android::sp<android::IBinder>&,
                                  android::ui::DisplayState*);
    using CreateVirtualDisplay5Fn =
            android::sp<android::IBinder> (*)(
                    const std::string&, bool, bool, const std::string&, float);
    using CreateVirtualDisplay4Fn =
            android::sp<android::IBinder> (*)(
                    const std::string&, bool, const std::string&, float);
    using CreateDisplay3Fn =
            android::sp<android::IBinder> (*)(const android::String8&, bool, float);
    using CreateDisplay2Fn =
            android::sp<android::IBinder> (*)(const android::String8&, bool);
    using DestroyVirtualDisplayFn =
            android::status_t (*)(const android::sp<android::IBinder>&);
    using DestroyDisplayFn = void (*)(const android::sp<android::IBinder>&);
    using GetDefaultFn = android::sp<android::SurfaceComposerClient> (*)();
    using MirrorDisplayFn =
            android::sp<android::SurfaceControl> (*)(
                    android::SurfaceComposerClient*, android::DisplayId);
    using ConsumerBaseSetDefaultBufferSizeFn =
            android::status_t (*)(void*, uint32_t, uint32_t);
    using ConsumerBaseSetConsumerUsageBitsFn =
            android::status_t (*)(void*, uint64_t);
    using ConsumerBaseAbandonFn = void (*)(void*);
    using BufferQueueProducerSetMaxDequeuedBufferCountFn =
            android::status_t (*)(void*, int);
    using BufferQueueProducerSetMaxDequeuedBufferCount2Fn =
            android::status_t (*)(void*, int, int*);
    using BufferQueueCreateBufferQueueFn =
            void (*)(android::sp<android::IGraphicBufferProducer>*,
                     android::sp<android::IGraphicBufferConsumer>*,
                     bool);
    using SurfaceGetIGraphicBufferProducerFn =
            android::sp<android::IGraphicBufferProducer> (*)(const android::Surface*);
    using CpuConsumerCreateWithConsumerFn =
            android::sp<android::CpuConsumer> (*)(
                    const android::sp<android::IGraphicBufferConsumer>&,
                    size_t,
                    bool);
    using CpuConsumerCtorFn =
            void (*)(android::CpuConsumer*,
                     const android::sp<android::IGraphicBufferConsumer>&,
                     size_t,
                     bool);
    using RefBaseIncStrongFn = void (*)(const void*, const void*);
    using CpuConsumerLockNextBufferFn =
            android::status_t (*)(android::CpuConsumer*, CpuConsumerLockedBuffer*);
    using CpuConsumerUnlockBufferFn =
            android::status_t (*)(android::CpuConsumer*, const CpuConsumerLockedBuffer&);
    using ProcessStateSelfFn = android::sp<android::ProcessState> (*)();
    using ProcessStateStartThreadPoolFn = void (*)(android::ProcessState*);
    using TransactionCtorFn = void (*)(void*);
    using SetDisplaySurfaceFn =
            android::status_t (*)(void*, const android::sp<android::IBinder>&,
                                  const android::sp<android::IGraphicBufferProducer>&);
    using SetDisplayProjectionFn =
            void* (*)(void*, const android::sp<android::IBinder>&,
                      android::ui::Rotation, const android::Rect&,
                      const android::Rect&);
    using SetDisplayLayerStackFn =
            void* (*)(void*, const android::sp<android::IBinder>&,
                      android::ui::LayerStack);
    using SetLayerStackFn =
            void* (*)(void*, const android::sp<android::SurfaceControl>&,
                      android::ui::LayerStack);
    using ApplyFn = android::status_t (*)(void*, bool, bool);
    using String8CtorFn = void (*)(android::String8*, const char*);
    using String8DtorFn = void (*)(android::String8*);

    GetPhysicalDisplayIdsFn getPhysicalDisplayIds = nullptr;
    GetPhysicalDisplayTokenFn getPhysicalDisplayToken = nullptr;
    GetActiveDisplayModeFn getActiveDisplayMode = nullptr;
    GetDisplayStateFn getDisplayState = nullptr;
    CreateVirtualDisplay5Fn createVirtualDisplay5 = nullptr;
    CreateVirtualDisplay4Fn createVirtualDisplay4 = nullptr;
    CreateDisplay3Fn createDisplay3 = nullptr;
    CreateDisplay2Fn createDisplay2 = nullptr;
    DestroyVirtualDisplayFn destroyVirtualDisplay = nullptr;
    DestroyDisplayFn destroyDisplay = nullptr;
    GetDefaultFn getDefault = nullptr;
    MirrorDisplayFn mirrorDisplay = nullptr;
    ConsumerBaseSetDefaultBufferSizeFn consumerBaseSetDefaultBufferSize = nullptr;
    ConsumerBaseSetConsumerUsageBitsFn consumerBaseSetConsumerUsageBits = nullptr;
    ConsumerBaseAbandonFn consumerBaseAbandon = nullptr;
    BufferQueueProducerSetMaxDequeuedBufferCountFn
            bufferQueueProducerSetMaxDequeuedBufferCount = nullptr;
    BufferQueueProducerSetMaxDequeuedBufferCount2Fn
            bufferQueueProducerSetMaxDequeuedBufferCount2 = nullptr;
    BufferQueueCreateBufferQueueFn bufferQueueCreateBufferQueue = nullptr;
    SurfaceGetIGraphicBufferProducerFn surfaceGetIGraphicBufferProducer = nullptr;
    CpuConsumerCreateWithConsumerFn cpuConsumerCreateWithConsumer = nullptr;
    CpuConsumerCtorFn cpuConsumerCtor = nullptr;
    RefBaseIncStrongFn refBaseIncStrong = nullptr;
    CpuConsumerLockNextBufferFn cpuConsumerLockNextBuffer = nullptr;
    CpuConsumerUnlockBufferFn cpuConsumerUnlockBuffer = nullptr;
    ProcessStateSelfFn processStateSelf = nullptr;
    ProcessStateStartThreadPoolFn processStateStartThreadPool = nullptr;
    TransactionCtorFn transactionCtor = nullptr;
    SetDisplaySurfaceFn setDisplaySurface = nullptr;
    SetDisplayProjectionFn setDisplayProjection = nullptr;
    SetDisplayLayerStackFn setDisplayLayerStack = nullptr;
    SetLayerStackFn setLayerStack = nullptr;
    ApplyFn apply = nullptr;
    String8CtorFn string8Ctor = nullptr;
    String8DtorFn string8Dtor = nullptr;

    template <typename T>
    bool loadFrom(void* handle, const char* libName, T* target, const char* symbol) {
        dlerror();
        *target = reinterpret_cast<T>(dlsym(handle, symbol));
        if (*target == nullptr) {
            fprintf(stderr, "ERROR: missing %s symbol %s: %s\n",
                    libName, symbol, dlerror());
            return false;
        }
        return true;
    }

    template <typename T>
    bool load(T* target, const char* symbol) {
        return loadFrom(libgui, "libgui", target, symbol);
    }

    template <typename T>
    void loadOptionalFrom(void* handle, T* target, const char* symbol) {
        dlerror();
        *target = reinterpret_cast<T>(dlsym(handle, symbol));
    }

    template <typename T>
    void loadOptional(T* target, const char* symbol) {
        loadOptionalFrom(libgui, target, symbol);
    }

    template <typename T, size_t N>
    void loadOptionalAny(T* target, const char* const (&symbols)[N]) {
        *target = nullptr;
        for (const char* symbol : symbols) {
            loadOptional(target, symbol);
            if (*target) {
                return;
            }
        }
    }

    bool hasMirrorDisplaySupport() const {
        return getDefault != nullptr && mirrorDisplay != nullptr && setLayerStack != nullptr;
    }

    bool hasCpuFrameOutputSymbols() const {
        return bufferQueueCreateBufferQueue != nullptr &&
               (cpuConsumerCreateWithConsumer != nullptr ||
                (cpuConsumerCtor != nullptr && refBaseIncStrong != nullptr)) &&
               cpuConsumerLockNextBuffer != nullptr &&
               cpuConsumerUnlockBuffer != nullptr &&
               consumerBaseSetDefaultBufferSize != nullptr &&
               consumerBaseSetConsumerUsageBits != nullptr;
    }

    bool hasImageReaderFrameOutputSymbols() const {
        return surfaceGetIGraphicBufferProducer != nullptr;
    }

    bool hasProducerMaxDequeuedBufferCount() const {
        return bufferQueueProducerSetMaxDequeuedBufferCount != nullptr ||
               bufferQueueProducerSetMaxDequeuedBufferCount2 != nullptr;
    }

    android::status_t setProducerMaxDequeuedBufferCount(void* producer,
                                                        int maxDequeuedBuffers) const {
        if (!producer) {
            return android::UNKNOWN_ERROR;
        }
        if (bufferQueueProducerSetMaxDequeuedBufferCount) {
            return bufferQueueProducerSetMaxDequeuedBufferCount(
                    producer, maxDequeuedBuffers);
        }
        if (bufferQueueProducerSetMaxDequeuedBufferCount2) {
            int maxBufferCount = 0;
            return bufferQueueProducerSetMaxDequeuedBufferCount2(
                    producer, maxDequeuedBuffers, &maxBufferCount);
        }
        return android::NO_ERROR;
    }

    android::sp<android::IBinder> createDisplayCompat(
            const std::string& displayName, bool secure, bool* legacyDisplay) {
        const std::string empty;
        if (legacyDisplay) {
            *legacyDisplay = false;
        }
        if (createVirtualDisplay5 && destroyVirtualDisplay) {
            android::sp<android::IBinder> display =
                    createVirtualDisplay5(displayName, secure, true, empty, 0.0f);
            if (display) {
                return display;
            }
        }
        if (createVirtualDisplay4 && destroyVirtualDisplay) {
            android::sp<android::IBinder> display =
                    createVirtualDisplay4(displayName, secure, empty, 0.0f);
            if (display) {
                return display;
            }
        }
        if ((!createDisplay3 && !createDisplay2) || !destroyDisplay ||
            !string8Ctor || !string8Dtor) {
            return {};
        }
        if (legacyDisplay) {
            *legacyDisplay = true;
        }
        android::String8 name8;
        string8Ctor(&name8, displayName.c_str());
        android::sp<android::IBinder> display = createDisplay3
                ? createDisplay3(name8, secure, 0.0f)
                : createDisplay2(name8, secure);
        string8Dtor(&name8);
        return display;
    }

    void destroyDisplayCompat(const android::sp<android::IBinder>& display,
                              bool legacyDisplay) {
        if (!display) {
            return;
        }
        if (legacyDisplay) {
            if (destroyDisplay) {
                destroyDisplay(display);
            }
            return;
        }
        if (destroyVirtualDisplay) {
            (void) destroyVirtualDisplay(display);
            return;
        }
        if (destroyDisplay) {
            destroyDisplay(display);
        }
    }

    bool open() {
        libgui = dlopen("libgui.so", RTLD_NOW | RTLD_LOCAL);
        if (!libgui) {
            fprintf(stderr, "ERROR: dlopen(libgui.so) failed: %s\n", dlerror());
            return false;
        }
        void* libbinder = dlopen("libbinder.so", RTLD_NOW | RTLD_LOCAL);
        if (!libbinder) {
            fprintf(stderr, "ERROR: dlopen(libbinder.so) failed: %s\n", dlerror());
            return false;
        }
        if (!loadFrom(libbinder, "libbinder", &processStateSelf,
                      "_ZN7android12ProcessState4selfEv") ||
            !loadFrom(libbinder, "libbinder", &processStateStartThreadPool,
                      "_ZN7android12ProcessState15startThreadPoolEv")) {
            return false;
        }
        if (!load(&getPhysicalDisplayIds,
                  "_ZN7android21SurfaceComposerClient21getPhysicalDisplayIdsEv") ||
            !load(&getPhysicalDisplayToken,
                  "_ZN7android21SurfaceComposerClient23getPhysicalDisplayTokenENS_17PhysicalDisplayIdE") ||
            !load(&getActiveDisplayMode,
                  "_ZN7android21SurfaceComposerClient20getActiveDisplayModeERKNS_2spINS_7IBinderEEEPNS_2ui11DisplayModeE") ||
            !load(&getDisplayState,
                  "_ZN7android21SurfaceComposerClient15getDisplayStateERKNS_2spINS_7IBinderEEEPNS_2ui12DisplayStateE") ||
            !load(&transactionCtor,
                  "_ZN7android21SurfaceComposerClient11TransactionC1Ev") ||
            !load(&setDisplaySurface,
                  "_ZN7android21SurfaceComposerClient11Transaction17setDisplaySurfaceERKNS_2spINS_7IBinderEEERKNS2_INS_22IGraphicBufferProducerEEE") ||
            !load(&setDisplayProjection,
                  "_ZN7android21SurfaceComposerClient11Transaction20setDisplayProjectionERKNS_2spINS_7IBinderEEENS_2ui8RotationERKNS_4RectESB_") ||
            !load(&setDisplayLayerStack,
                  "_ZN7android21SurfaceComposerClient11Transaction20setDisplayLayerStackERKNS_2spINS_7IBinderEEENS_2ui10LayerStackE") ||
            !load(&apply,
                  "_ZN7android21SurfaceComposerClient11Transaction5applyEbb")) {
            return false;
        }

        loadOptional(
                &createVirtualDisplay5,
                "_ZN7android21SurfaceComposerClient20createVirtualDisplayERKNSt3__112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEEbbS9_f");
        loadOptional(
                &createVirtualDisplay4,
                "_ZN7android21SurfaceComposerClient20createVirtualDisplayERKNSt3__112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEEbS9_f");
        loadOptional(&createDisplay3,
                     "_ZN7android21SurfaceComposerClient13createDisplayERKNS_7String8Ebf");
        loadOptional(&createDisplay2,
                     "_ZN7android21SurfaceComposerClient13createDisplayERKNS_7String8Eb");
        if (!createVirtualDisplay5 && !createVirtualDisplay4 &&
            !createDisplay3 && !createDisplay2) {
            fprintf(stderr, "ERROR: no supported display creation symbol found\n");
            return false;
        }

        loadOptional(&destroyVirtualDisplay,
                     "_ZN7android21SurfaceComposerClient21destroyVirtualDisplayERKNS_2spINS_7IBinderEEE");
        loadOptional(&destroyDisplay,
                     "_ZN7android21SurfaceComposerClient14destroyDisplayERKNS_2spINS_7IBinderEEE");
        if (!destroyVirtualDisplay && !destroyDisplay) {
            fprintf(stderr, "ERROR: no supported display destruction symbol found\n");
            return false;
        }

        const bool hasVirtualDisplayPair =
                destroyVirtualDisplay && (createVirtualDisplay5 || createVirtualDisplay4);
        const bool hasLegacyDisplayPair =
                destroyDisplay && (createDisplay3 || createDisplay2);
        if (!hasVirtualDisplayPair && !hasLegacyDisplayPair) {
            fprintf(stderr, "ERROR: no supported display create/destroy symbol pair found\n");
            return false;
        }

        const bool needsString8Display =
                !hasVirtualDisplayPair && hasLegacyDisplayPair;
        if (needsString8Display) {
            libutils = dlopen("libutils.so", RTLD_NOW | RTLD_LOCAL);
            if (!libutils) {
                fprintf(stderr, "ERROR: dlopen(libutils.so) failed: %s\n", dlerror());
                return false;
            }
            if (!loadFrom(libutils, "libutils", &string8Ctor,
                          "_ZN7android7String8C1EPKc") ||
                !loadFrom(libutils, "libutils", &string8Dtor,
                          "_ZN7android7String8D1Ev")) {
                return false;
            }
        }

        loadOptional(&getDefault, "_ZN7android21SurfaceComposerClient10getDefaultEv");
        loadOptional(&mirrorDisplay,
                     "_ZN7android21SurfaceComposerClient13mirrorDisplayENS_9DisplayIdE");
        loadOptional(&setLayerStack,
                     "_ZN7android21SurfaceComposerClient11Transaction13setLayerStackERKNS_2spINS_14SurfaceControlEEENS_2ui10LayerStackE");
        if (mirrorDisplay && !hasMirrorDisplaySupport()) {
            fprintf(stderr, "ERROR: incomplete mirror display symbol set\n");
            return false;
        }

        loadOptional(&consumerBaseSetDefaultBufferSize,
                     "_ZN7android12ConsumerBase20setDefaultBufferSizeEjj");
        const char* const consumerUsageSymbols[] = {
                "_ZN7android12ConsumerBase20setConsumerUsageBitsEm",
                "_ZN7android12ConsumerBase20setConsumerUsageBitsEy",
        };
        loadOptionalAny(&consumerBaseSetConsumerUsageBits, consumerUsageSymbols);
        loadOptional(&consumerBaseAbandon,
                     "_ZN7android12ConsumerBase7abandonEv");
        loadOptional(&bufferQueueProducerSetMaxDequeuedBufferCount,
                     "_ZN7android19BufferQueueProducer25setMaxDequeuedBufferCountEi");
        loadOptional(&bufferQueueProducerSetMaxDequeuedBufferCount2,
                     "_ZN7android19BufferQueueProducer25setMaxDequeuedBufferCountEiPi");
        loadOptional(&bufferQueueCreateBufferQueue,
                     "_ZN7android11BufferQueue17createBufferQueueEPNS_2spINS_22IGraphicBufferProducerEEEPNS1_INS_22IGraphicBufferConsumerEEEb");
        loadOptional(&surfaceGetIGraphicBufferProducer,
                     "_ZNK7android7Surface25getIGraphicBufferProducerEv");
        const char* const cpuConsumerCreateSymbols[] = {
                "_ZN7android11CpuConsumer6createERKNS_2spINS_22IGraphicBufferConsumerEEEmb",
                "_ZN7android11CpuConsumer6createERKNS_2spINS_22IGraphicBufferConsumerEEEjb",
        };
        loadOptionalAny(&cpuConsumerCreateWithConsumer, cpuConsumerCreateSymbols);
        const char* const cpuConsumerCtorSymbols[] = {
                "_ZN7android11CpuConsumerC1ERKNS_2spINS_22IGraphicBufferConsumerEEEmb",
                "_ZN7android11CpuConsumerC2ERKNS_2spINS_22IGraphicBufferConsumerEEEmb",
                "_ZN7android11CpuConsumerC1ERKNS_2spINS_22IGraphicBufferConsumerEEEjb",
                "_ZN7android11CpuConsumerC2ERKNS_2spINS_22IGraphicBufferConsumerEEEjb",
        };
        loadOptionalAny(&cpuConsumerCtor, cpuConsumerCtorSymbols);
        if (!cpuConsumerCreateWithConsumer && cpuConsumerCtor) {
            if (!libutils) {
                libutils = dlopen("libutils.so", RTLD_NOW | RTLD_LOCAL);
                if (!libutils) {
                    fprintf(stderr, "ERROR: dlopen(libutils.so) failed: %s\n", dlerror());
                    return false;
                }
            }
            if (!loadFrom(libutils, "libutils", &refBaseIncStrong,
                          "_ZNK7android7RefBase9incStrongEPKv")) {
                return false;
            }
        }
        loadOptional(&cpuConsumerLockNextBuffer,
                     "_ZN7android11CpuConsumer14lockNextBufferEPNS0_12LockedBufferE");
        loadOptional(&cpuConsumerUnlockBuffer,
                     "_ZN7android11CpuConsumer12unlockBufferERKNS0_12LockedBufferE");
        return true;
    }
};

struct VirtualDisplay {
    android::sp<android::IBinder> display;
    android::sp<android::SurfaceControl> mirrorRoot;
    bool legacyDisplay = false;
};

static android::Rect calculateDisplayRect(const Options& opts,
                                          const android::ui::DisplayState& displayState) {
    android::Rect layerStackRect(0, 0, displayState.layerStackSpaceRect.width,
                                 displayState.layerStackSpaceRect.height);
    float displayAspect =
            layerStackRect.getHeight() / static_cast<float>(layerStackRect.getWidth());

    uint32_t videoWidth = opts.rotate ? opts.videoHeight : opts.videoWidth;
    uint32_t videoHeight = opts.rotate ? opts.videoWidth : opts.videoHeight;

    uint32_t outWidth;
    uint32_t outHeight;
    if (videoHeight > static_cast<uint32_t>(videoWidth * displayAspect)) {
        outWidth = videoWidth;
        outHeight = static_cast<uint32_t>(videoWidth * displayAspect);
    } else {
        outHeight = videoHeight;
        outWidth = static_cast<uint32_t>(videoHeight / displayAspect);
    }
    uint32_t offX = (videoWidth - outWidth) / 2;
    uint32_t offY = (videoHeight - outHeight) / 2;
    return android::Rect(offX, offY, offX + outWidth, offY + outHeight);
}

static android::Rect calculateLayerStackRect(const Options& opts,
                                             const android::ui::DisplayState& displayState) {
    const int fullWidth = displayState.layerStackSpaceRect.width;
    const int fullHeight = displayState.layerStackSpaceRect.height;
    if (!opts.rawCropSpecified || fullWidth <= 0 || fullHeight <= 0) {
        return android::Rect(0, 0, fullWidth, fullHeight);
    }

    const int left = std::min(static_cast<int>(opts.rawCropLeft), fullWidth - 1);
    const int top = std::min(static_cast<int>(opts.rawCropTop), fullHeight - 1);
    const int cropWidth = std::min(std::max(1, static_cast<int>(opts.rawCropWidth)),
                                   fullWidth - left);
    const int cropHeight = std::min(std::max(1, static_cast<int>(opts.rawCropHeight)),
                                    fullHeight - top);
    return android::Rect(left, top, left + cropWidth, top + cropHeight);
}

static android::Rect calculateOutputDisplayRect(const Options& opts,
                                                const android::ui::DisplayState& displayState) {
    if (opts.rawCropSpecified) {
        return android::Rect(0, 0, opts.videoWidth, opts.videoHeight);
    }
    return calculateDisplayRect(opts, displayState);
}

static void printContentArea(const Options& opts, const android::Rect& displayRect) {
    if (!opts.verbose) {
        return;
    }

    uint32_t outWidth = static_cast<uint32_t>(displayRect.getWidth());
    uint32_t outHeight = static_cast<uint32_t>(displayRect.getHeight());
    if (opts.rotate) {
        printf("Rotated content area is %ux%u at offset x=%d y=%d\n",
               outHeight, outWidth, displayRect.top, displayRect.left);
    } else {
        printf("Content area is %ux%u at offset x=%d y=%d\n",
               outWidth, outHeight, displayRect.left, displayRect.top);
    }
    fflush(stdout);
}

static android::status_t prepareVirtualDisplay(
        GuiApi& gui, const Options& opts, android::PhysicalDisplayId physicalId,
        const android::ui::DisplayState& displayState,
        const android::sp<android::IGraphicBufferProducer>& producer,
        VirtualDisplay* out) {
    const std::string displayName = opts.displayIdSpecified
            ? "ScreenRecorder " + std::to_string(physicalId.value)
            : "ScreenRecorder";

    out->display = {};
    out->mirrorRoot = {};
    out->legacyDisplay = false;
    out->display = gui.createDisplayCompat(displayName, opts.captureSecure,
                                           &out->legacyDisplay);
    if (!out->display) {
        fprintf(stderr, "ERROR: no display\n");
        return android::UNKNOWN_ERROR;
    }
    auto destroyOnError = [&]() {
        gui.destroyDisplayCompat(out->display, out->legacyDisplay);
        out->display = {};
        out->mirrorRoot = {};
    };

    alignas(16) unsigned char txStorage[65536];
    memset(txStorage, 0, sizeof(txStorage));
    gui.transactionCtor(txStorage);

    android::Rect layerStackRect = calculateLayerStackRect(opts, displayState);
    android::Rect displayRect = calculateOutputDisplayRect(opts, displayState);
    printContentArea(opts, displayRect);

    const bool useMirrorDisplay = gui.hasMirrorDisplaySupport();
    android::ui::LayerStack layerStack = displayState.layerStack;
    if (useMirrorDisplay) {
        std::srand(static_cast<unsigned int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count()));
        layerStack = android::ui::LayerStack{static_cast<uint32_t>(std::rand())};
    }

    android::status_t err = gui.setDisplaySurface(txStorage, out->display, producer);
    if (err != android::NO_ERROR) {
        fprintf(stderr, "ERROR: unable to set display surface (err=%d)\n", err);
        destroyOnError();
        return err;
    }
    gui.setDisplayProjection(txStorage, out->display,
                             opts.rotate ? android::ui::ROTATION_90 : android::ui::ROTATION_0,
                             layerStackRect, displayRect);
    gui.setDisplayLayerStack(txStorage, out->display, layerStack);

    if (useMirrorDisplay) {
        android::sp<android::SurfaceComposerClient> client = gui.getDefault();
        if (!client) {
            fprintf(stderr, "ERROR: unable to get SurfaceComposerClient\n");
            destroyOnError();
            return android::UNKNOWN_ERROR;
        }

        out->mirrorRoot =
                gui.mirrorDisplay(client.get(), android::DisplayId::fromValue(physicalId.value));
        if (!out->mirrorRoot) {
            fprintf(stderr, "Failed to create a mirror for screenrecord\n");
            destroyOnError();
            return android::UNKNOWN_ERROR;
        }
        gui.setLayerStack(txStorage, out->mirrorRoot, layerStack);
    }

    err = gui.apply(txStorage, false, false);
    if (err != android::NO_ERROR) {
        fprintf(stderr, "ERROR: unable to apply display transaction (err=%d)\n", err);
        destroyOnError();
        return err;
    }
    return android::NO_ERROR;
}

static bool displayProjectionChanged(const android::ui::DisplayState& lhs,
                                     const android::ui::DisplayState& rhs) {
    return lhs.orientation != rhs.orientation ||
           lhs.layerStackSpaceRect.getWidth() != rhs.layerStackSpaceRect.getWidth() ||
           lhs.layerStackSpaceRect.getHeight() != rhs.layerStackSpaceRect.getHeight();
}

static void updateDisplayProjection(GuiApi& gui, const Options& opts,
                                    android::PhysicalDisplayId physicalId,
                                    const android::sp<android::IBinder>& virtualDisplay,
                                    android::ui::DisplayState* displayState) {
    android::sp<android::IBinder> displayToken = gui.getPhysicalDisplayToken(physicalId);
    if (!displayToken) {
        return;
    }

    android::ui::DisplayState currentDisplayState;
    if (gui.getDisplayState(displayToken, &currentDisplayState) != android::NO_ERROR) {
        return;
    }
    if (!displayProjectionChanged(currentDisplayState, *displayState)) {
        return;
    }

    *displayState = currentDisplayState;
    alignas(16) unsigned char txStorage[65536];
    memset(txStorage, 0, sizeof(txStorage));
    gui.transactionCtor(txStorage);

    android::Rect layerStackRect = calculateLayerStackRect(opts, *displayState);
    android::Rect displayRect = calculateOutputDisplayRect(opts, *displayState);
    gui.setDisplayProjection(txStorage, virtualDisplay,
                             opts.rotate ? android::ui::ROTATION_90 : android::ui::ROTATION_0,
                             layerStackRect, displayRect);
    gui.apply(txStorage, false, false);
}

static const void* getVirtualRefBaseSubobject(void* object) {
    if (!object) {
        return nullptr;
    }
    const auto vtable = *reinterpret_cast<const std::ptrdiff_t* const*>(object);
    if (!vtable) {
        return nullptr;
    }
    const std::ptrdiff_t refBaseOffset = vtable[-3];
    if (refBaseOffset < 0 ||
        static_cast<size_t>(refBaseOffset) >= kCpuConsumerStorageBytes) {
        return nullptr;
    }
    return static_cast<uint8_t*>(object) + refBaseOffset;
}

struct FrameOutput {
    android::sp<android::IGraphicBufferProducer> inputProducer;
    android::sp<android::IGraphicBufferConsumer> cpuInputConsumer;
    android::sp<android::CpuConsumer> cpuConsumer;
    android::CpuConsumer* cpuConsumerRaw = nullptr;
    void* cpuConsumerStorage = nullptr;
    AImageReader* imageReader = nullptr;
    ANativeWindow* imageReaderWindow = nullptr;
    ScreenrecordRawCaptureBackend backend =
            SCREENRECORD_RAW_CAPTURE_BACKEND_CPU_CONSUMER;
    uint32_t width = 0;
    uint32_t height = 0;
    bool haveRgbFrame = false;
    std::vector<uint8_t> rgbBuffer;
};

static android::status_t prepareImageReaderFrameOutput(GuiApi& gui, const Options& opts,
                                                       FrameOutput* output) {
    if (!gui.hasImageReaderFrameOutputSymbols()) {
        fprintf(stderr, "ERROR: AImageReader output bridge is not supported on this Android build\n");
        return android::UNKNOWN_ERROR;
    }
    output->backend = SCREENRECORD_RAW_CAPTURE_BACKEND_AIMAGE_READER;
    output->width = opts.videoWidth;
    output->height = opts.videoHeight;

    AImageReader* reader = nullptr;
    media_status_t mediaErr = AImageReader_newWithUsage(
            static_cast<int32_t>(opts.videoWidth),
            static_cast<int32_t>(opts.videoHeight),
            AIMAGE_FORMAT_RGBA_8888,
            AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,
            4,
            &reader);
    if (mediaErr != AMEDIA_OK) {
        mediaErr = AImageReader_new(
                static_cast<int32_t>(opts.videoWidth),
                static_cast<int32_t>(opts.videoHeight),
                AIMAGE_FORMAT_RGBA_8888,
                4,
                &reader);
    }
    if (mediaErr != AMEDIA_OK || !reader) {
        fprintf(stderr, "ERROR: unable to create AImageReader err=%d size=%ux%u\n",
                mediaErr, opts.videoWidth, opts.videoHeight);
        return android::UNKNOWN_ERROR;
    }

    ANativeWindow* window = nullptr;
    mediaErr = AImageReader_getWindow(reader, &window);
    if (mediaErr != AMEDIA_OK || !window) {
        fprintf(stderr, "ERROR: unable to get AImageReader window err=%d\n", mediaErr);
        AImageReader_delete(reader);
        return android::UNKNOWN_ERROR;
    }

    const auto* surface = reinterpret_cast<const android::Surface*>(
            reinterpret_cast<const uint8_t*>(window) - kSurfaceNativeWindowOffsetBytes);
    android::sp<android::IGraphicBufferProducer> producer =
            gui.surfaceGetIGraphicBufferProducer(surface);
    if (!producer) {
        fprintf(stderr, "ERROR: unable to get AImageReader IGraphicBufferProducer\n");
        AImageReader_delete(reader);
        return android::UNKNOWN_ERROR;
    }

    output->inputProducer = producer;
    output->imageReader = reader;
    output->imageReaderWindow = window;
    output->rgbBuffer.resize(static_cast<size_t>(opts.videoWidth) * opts.videoHeight * 3);
    return android::NO_ERROR;
}

static android::status_t prepareCpuConsumerFrameOutput(GuiApi& gui, const Options& opts,
                                                       FrameOutput* output) {
    if (!gui.hasCpuFrameOutputSymbols()) {
        fprintf(stderr, "ERROR: CpuConsumer frame output is not supported on this Android build\n");
        return android::UNKNOWN_ERROR;
    }
    output->backend = SCREENRECORD_RAW_CAPTURE_BACKEND_CPU_CONSUMER;
    output->width = opts.videoWidth;
    output->height = opts.videoHeight;

    android::sp<android::IGraphicBufferProducer> producer;
    android::sp<android::IGraphicBufferConsumer> consumer;
    gui.bufferQueueCreateBufferQueue(&producer, &consumer, false);
    if (!producer || !consumer) {
        fprintf(stderr, "ERROR: unable to create CpuConsumer buffer queue\n");
        return android::UNKNOWN_ERROR;
    }

    if (gui.cpuConsumerCreateWithConsumer) {
        output->cpuConsumer = gui.cpuConsumerCreateWithConsumer(consumer, 3, true);
        output->cpuConsumerRaw = output->cpuConsumer.get();
    } else {
        void* storage = nullptr;
        if (posix_memalign(&storage, alignof(std::max_align_t),
                           kCpuConsumerStorageBytes) != 0 || !storage) {
            fprintf(stderr, "ERROR: unable to allocate CpuConsumer storage\n");
            return android::UNKNOWN_ERROR;
        }
        memset(storage, 0, kCpuConsumerStorageBytes);
        output->cpuConsumerStorage = storage;
        output->cpuConsumerRaw = reinterpret_cast<android::CpuConsumer*>(storage);
        gui.cpuConsumerCtor(output->cpuConsumerRaw, consumer, 3, true);
        const void* refBase = getVirtualRefBaseSubobject(output->cpuConsumerRaw);
        if (!refBase) {
            fprintf(stderr, "ERROR: unable to locate CpuConsumer RefBase subobject\n");
            return android::UNKNOWN_ERROR;
        }
        gui.refBaseIncStrong(refBase, output->cpuConsumerRaw);
    }
    if (!output->cpuConsumerRaw) {
        fprintf(stderr, "ERROR: unable to create CpuConsumer\n");
        return android::UNKNOWN_ERROR;
    }

    output->inputProducer = producer;
    output->cpuInputConsumer = consumer;
    if (gui.hasProducerMaxDequeuedBufferCount()) {
        android::status_t err = gui.setProducerMaxDequeuedBufferCount(
                output->inputProducer.get(), 4);
        if (err != android::NO_ERROR) {
            fprintf(stderr,
                    "WARNING: unable to set producer max dequeued buffer count (err=%d)\n",
                    err);
        }
    }
    gui.consumerBaseSetDefaultBufferSize(output->cpuConsumerRaw, opts.videoWidth,
                                         opts.videoHeight);
    gui.consumerBaseSetConsumerUsageBits(output->cpuConsumerRaw,
                                         AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN);
    output->rgbBuffer.resize(static_cast<size_t>(opts.videoWidth) * opts.videoHeight * 3);
    return android::NO_ERROR;
}

static android::status_t prepareFrameOutput(GuiApi& gui, const Options& opts,
                                            FrameOutput* output) {
    if (opts.backend == SCREENRECORD_RAW_CAPTURE_BACKEND_CPU_CONSUMER) {
        return prepareCpuConsumerFrameOutput(gui, opts, output);
    }
    return prepareImageReaderFrameOutput(gui, opts, output);
}

static void destroyFrameOutput(GuiApi& gui, FrameOutput* output) {
    if (output->cpuConsumerRaw && gui.consumerBaseAbandon) {
        gui.consumerBaseAbandon(output->cpuConsumerRaw);
    }
    if (output->imageReader) {
        AImageReader_delete(output->imageReader);
    }
    output->inputProducer = {};
    output->cpuInputConsumer = {};
    output->cpuConsumer = {};
    output->cpuConsumerRaw = nullptr;
    // The constructor fallback object has no stable exported delete path.
    output->cpuConsumerStorage = nullptr;
    output->imageReader = nullptr;
    output->imageReaderWindow = nullptr;
    output->rgbBuffer.clear();
    *output = FrameOutput();
}

static bool ensureGuiOpen(GuiApi& gui, bool* guiOpened) {
    if (*guiOpened) {
        return true;
    }
    if (!gui.open()) {
        return false;
    }
    *guiOpened = true;
    return true;
}

static android::status_t copyCpuLockedBufferToRgb(const CpuConsumerLockedBuffer& buffer,
                                                  FrameOutput& output) {
    if (!buffer.data || buffer.stride == 0 || output.width == 0 || output.height == 0) {
        return android::UNKNOWN_ERROR;
    }

    int32_t cropLeft = buffer.crop.left;
    int32_t cropTop = buffer.crop.top;
    int32_t cropRight = buffer.crop.right;
    int32_t cropBottom = buffer.crop.bottom;
    if (cropRight <= cropLeft || cropBottom <= cropTop) {
        cropLeft = 0;
        cropTop = 0;
        cropRight = static_cast<int32_t>(buffer.width);
        cropBottom = static_cast<int32_t>(buffer.height);
    }
    cropLeft = std::clamp(cropLeft, 0, static_cast<int32_t>(buffer.width));
    cropTop = std::clamp(cropTop, 0, static_cast<int32_t>(buffer.height));
    cropRight = std::clamp(cropRight, cropLeft, static_cast<int32_t>(buffer.width));
    cropBottom = std::clamp(cropBottom, cropTop, static_cast<int32_t>(buffer.height));

    const int32_t copyWidth = cropRight - cropLeft;
    const int32_t copyHeight = cropBottom - cropTop;
    if (copyWidth != static_cast<int32_t>(output.width) ||
        copyHeight != static_cast<int32_t>(output.height)) {
        fprintf(stderr,
                "ERROR: CPU frame crop mismatch fmt=%d buffer=%ux%u stride=%u crop=%d,%d,%d,%d output=%ux%u\n",
                buffer.format, buffer.width, buffer.height, buffer.stride,
                cropLeft, cropTop, cropRight, cropBottom, output.width, output.height);
        return android::UNKNOWN_ERROR;
    }

    uint8_t* dst = output.rgbBuffer.data();
    switch (buffer.format) {
        case kHalPixelFormatRgba8888:
        case kHalPixelFormatRgbx8888: {
            for (uint32_t y = 0; y < output.height; ++y) {
                const uint8_t* src = buffer.data +
                        (static_cast<size_t>(cropTop + y) * buffer.stride + cropLeft) * 4;
                for (uint32_t x = 0; x < output.width; ++x) {
                    *dst++ = src[0];
                    *dst++ = src[1];
                    *dst++ = src[2];
                    src += 4;
                }
            }
            return android::NO_ERROR;
        }
        case kHalPixelFormatBgra8888: {
            for (uint32_t y = 0; y < output.height; ++y) {
                const uint8_t* src = buffer.data +
                        (static_cast<size_t>(cropTop + y) * buffer.stride + cropLeft) * 4;
                for (uint32_t x = 0; x < output.width; ++x) {
                    *dst++ = src[2];
                    *dst++ = src[1];
                    *dst++ = src[0];
                    src += 4;
                }
            }
            return android::NO_ERROR;
        }
        case kHalPixelFormatRgb888: {
            for (uint32_t y = 0; y < output.height; ++y) {
                const uint8_t* src = buffer.data +
                        (static_cast<size_t>(cropTop + y) * buffer.stride + cropLeft) * 3;
                memcpy(dst, src, static_cast<size_t>(output.width) * 3);
                dst += static_cast<size_t>(output.width) * 3;
            }
            return android::NO_ERROR;
        }
        case kHalPixelFormatRgb565: {
            for (uint32_t y = 0; y < output.height; ++y) {
                const uint16_t* src = reinterpret_cast<const uint16_t*>(
                        buffer.data +
                        (static_cast<size_t>(cropTop + y) * buffer.stride + cropLeft) * 2);
                for (uint32_t x = 0; x < output.width; ++x) {
                    const uint16_t p = *src++;
                    *dst++ = static_cast<uint8_t>(((p >> 11) & 0x1f) * 255 / 31);
                    *dst++ = static_cast<uint8_t>(((p >> 5) & 0x3f) * 255 / 63);
                    *dst++ = static_cast<uint8_t>((p & 0x1f) * 255 / 31);
                }
            }
            return android::NO_ERROR;
        }
        default:
            fprintf(stderr,
                    "ERROR: unsupported CPU frame format=%d flex=%d buffer=%ux%u stride=%u\n",
                    buffer.format, buffer.flexFormat, buffer.width, buffer.height,
                    buffer.stride);
            return android::UNKNOWN_ERROR;
    }
}

static bool validateImagePlaneBounds(int32_t dataLength,
                                     int32_t rowStride,
                                     int32_t pixelStride,
                                     int32_t bytesPerPixel,
                                     int32_t cropLeft,
                                     int32_t cropTop,
                                     int32_t copyWidth,
                                     int32_t copyHeight) {
    if (dataLength <= 0 || rowStride <= 0 || pixelStride <= 0 ||
        bytesPerPixel <= 0 || copyWidth <= 0 || copyHeight <= 0) {
        return false;
    }
    const size_t lastByteOffset =
            static_cast<size_t>(cropTop + copyHeight - 1) * rowStride +
            static_cast<size_t>(cropLeft + copyWidth - 1) * pixelStride +
            static_cast<size_t>(bytesPerPixel);
    return lastByteOffset <= static_cast<size_t>(dataLength);
}

static android::status_t copyAImageToRgb(AImage* image, FrameOutput& output) {
    if (!image || output.width == 0 || output.height == 0) {
        return android::UNKNOWN_ERROR;
    }

    int32_t imageWidth = 0;
    int32_t imageHeight = 0;
    int32_t format = 0;
    if (AImage_getWidth(image, &imageWidth) != AMEDIA_OK ||
        AImage_getHeight(image, &imageHeight) != AMEDIA_OK ||
        AImage_getFormat(image, &format) != AMEDIA_OK ||
        imageWidth <= 0 || imageHeight <= 0) {
        fprintf(stderr, "ERROR: unable to query AImage metadata\n");
        return android::UNKNOWN_ERROR;
    }

    AImageCropRect crop {0, 0, imageWidth, imageHeight};
    AImageCropRect queriedCrop {};
    if (AImage_getCropRect(image, &queriedCrop) == AMEDIA_OK &&
        queriedCrop.right > queriedCrop.left &&
        queriedCrop.bottom > queriedCrop.top) {
        crop = queriedCrop;
    }
    crop.left = std::clamp(crop.left, 0, imageWidth);
    crop.top = std::clamp(crop.top, 0, imageHeight);
    crop.right = std::clamp(crop.right, crop.left, imageWidth);
    crop.bottom = std::clamp(crop.bottom, crop.top, imageHeight);

    int32_t copyWidth = crop.right - crop.left;
    int32_t copyHeight = crop.bottom - crop.top;
    if ((copyWidth != static_cast<int32_t>(output.width) ||
         copyHeight != static_cast<int32_t>(output.height)) &&
        imageWidth == static_cast<int32_t>(output.width) &&
        imageHeight == static_cast<int32_t>(output.height)) {
        crop = AImageCropRect {0, 0, imageWidth, imageHeight};
        copyWidth = imageWidth;
        copyHeight = imageHeight;
    }
    if (copyWidth != static_cast<int32_t>(output.width) ||
        copyHeight != static_cast<int32_t>(output.height)) {
        fprintf(stderr,
                "ERROR: AImage frame crop mismatch fmt=%d image=%dx%d crop=%d,%d,%d,%d output=%ux%u\n",
                format, imageWidth, imageHeight,
                crop.left, crop.top, crop.right, crop.bottom,
                output.width, output.height);
        return android::UNKNOWN_ERROR;
    }

    uint8_t* planeData = nullptr;
    int32_t dataLength = 0;
    int32_t rowStride = 0;
    int32_t pixelStride = 0;
    if (AImage_getPlaneData(image, 0, &planeData, &dataLength) != AMEDIA_OK ||
        AImage_getPlaneRowStride(image, 0, &rowStride) != AMEDIA_OK ||
        AImage_getPlanePixelStride(image, 0, &pixelStride) != AMEDIA_OK ||
        !planeData) {
        fprintf(stderr, "ERROR: unable to access AImage plane fmt=%d image=%dx%d\n",
                format, imageWidth, imageHeight);
        return android::UNKNOWN_ERROR;
    }

    uint8_t* dst = output.rgbBuffer.data();
    switch (format) {
        case AIMAGE_FORMAT_RGBA_8888:
        case AIMAGE_FORMAT_RGBX_8888: {
            if (!validateImagePlaneBounds(dataLength, rowStride, pixelStride, 4,
                                          crop.left, crop.top, copyWidth, copyHeight)) {
                fprintf(stderr,
                        "ERROR: invalid AImage RGBA plane bounds len=%d row=%d px=%d image=%dx%d crop=%d,%d,%d,%d\n",
                        dataLength, rowStride, pixelStride, imageWidth, imageHeight,
                        crop.left, crop.top, crop.right, crop.bottom);
                return android::UNKNOWN_ERROR;
            }
            for (uint32_t y = 0; y < output.height; ++y) {
                const uint8_t* src = planeData +
                        static_cast<size_t>(crop.top + static_cast<int32_t>(y)) * rowStride +
                        static_cast<size_t>(crop.left) * pixelStride;
                for (uint32_t x = 0; x < output.width; ++x) {
                    *dst++ = src[0];
                    *dst++ = src[1];
                    *dst++ = src[2];
                    src += pixelStride;
                }
            }
            return android::NO_ERROR;
        }
        case AIMAGE_FORMAT_RGB_888: {
            if (!validateImagePlaneBounds(dataLength, rowStride, pixelStride, 3,
                                          crop.left, crop.top, copyWidth, copyHeight)) {
                fprintf(stderr,
                        "ERROR: invalid AImage RGB plane bounds len=%d row=%d px=%d image=%dx%d crop=%d,%d,%d,%d\n",
                        dataLength, rowStride, pixelStride, imageWidth, imageHeight,
                        crop.left, crop.top, crop.right, crop.bottom);
                return android::UNKNOWN_ERROR;
            }
            for (uint32_t y = 0; y < output.height; ++y) {
                const uint8_t* src = planeData +
                        static_cast<size_t>(crop.top + static_cast<int32_t>(y)) * rowStride +
                        static_cast<size_t>(crop.left) * pixelStride;
                for (uint32_t x = 0; x < output.width; ++x) {
                    *dst++ = src[0];
                    *dst++ = src[1];
                    *dst++ = src[2];
                    src += pixelStride;
                }
            }
            return android::NO_ERROR;
        }
        case AIMAGE_FORMAT_RGB_565: {
            if (!validateImagePlaneBounds(dataLength, rowStride, pixelStride, 2,
                                          crop.left, crop.top, copyWidth, copyHeight)) {
                fprintf(stderr,
                        "ERROR: invalid AImage RGB565 plane bounds len=%d row=%d px=%d image=%dx%d crop=%d,%d,%d,%d\n",
                        dataLength, rowStride, pixelStride, imageWidth, imageHeight,
                        crop.left, crop.top, crop.right, crop.bottom);
                return android::UNKNOWN_ERROR;
            }
            for (uint32_t y = 0; y < output.height; ++y) {
                const uint8_t* src = planeData +
                        static_cast<size_t>(crop.top + static_cast<int32_t>(y)) * rowStride +
                        static_cast<size_t>(crop.left) * pixelStride;
                for (uint32_t x = 0; x < output.width; ++x) {
                    uint16_t p = 0;
                    memcpy(&p, src, sizeof(p));
                    *dst++ = static_cast<uint8_t>(((p >> 11) & 0x1f) * 255 / 31);
                    *dst++ = static_cast<uint8_t>(((p >> 5) & 0x3f) * 255 / 63);
                    *dst++ = static_cast<uint8_t>((p & 0x1f) * 255 / 31);
                    src += pixelStride;
                }
            }
            return android::NO_ERROR;
        }
        default:
            fprintf(stderr,
                    "ERROR: unsupported AImage format=%d image=%dx%d output=%ux%u\n",
                    format, imageWidth, imageHeight, output.width, output.height);
            return android::UNKNOWN_ERROR;
    }
}

static android::status_t captureImageReaderFrameToRgb(FrameOutput& output) {
    auto start = std::chrono::steady_clock::now();
    while (!gStopRequested) {
        AImage* image = nullptr;
        media_status_t mediaErr =
                AImageReader_acquireLatestImage(output.imageReader, &image);
        if (mediaErr == AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE) {
            if (output.haveRgbFrame) {
                return android::NO_ERROR;
            }
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start);
            if (elapsed.count() >= 250000) {
                return ETIMEDOUT;
            }
            usleep(1000);
            continue;
        }
        if (mediaErr != AMEDIA_OK) {
            return mediaErr;
        }

        const android::status_t copyErr = copyAImageToRgb(image, output);
        AImage_delete(image);
        if (copyErr != android::NO_ERROR) {
            return copyErr;
        }
        output.haveRgbFrame = true;
        return android::NO_ERROR;
    }
    return ETIMEDOUT;
}

static android::status_t captureCpuConsumerFrameToRgb(GuiApi& gui, FrameOutput& output) {
    auto start = std::chrono::steady_clock::now();
    while (!gStopRequested) {
        CpuConsumerLockedBuffer locked {};
        android::status_t err =
                gui.cpuConsumerLockNextBuffer(output.cpuConsumerRaw, &locked);
        if (err == -EINVAL) {
            if (output.haveRgbFrame) {
                return android::NO_ERROR;
            }
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start);
            if (elapsed.count() >= 250000) {
                return ETIMEDOUT;
            }
            usleep(1000);
            continue;
        }
        if (err != android::NO_ERROR) {
            return err;
        }

        const android::status_t copyErr = copyCpuLockedBufferToRgb(locked, output);
        const android::status_t unlockErr =
                gui.cpuConsumerUnlockBuffer(output.cpuConsumerRaw, locked);
        if (copyErr != android::NO_ERROR) {
            return copyErr;
        }
        if (unlockErr != android::NO_ERROR) {
            fprintf(stderr, "ERROR: CpuConsumer unlock failed err=%d\n", unlockErr);
            return unlockErr;
        }
        output.haveRgbFrame = true;
        return android::NO_ERROR;
    }
    return ETIMEDOUT;
}

static android::status_t captureFrameToRgb(GuiApi& gui, FrameOutput& output) {
    if (output.backend == SCREENRECORD_RAW_CAPTURE_BACKEND_CPU_CONSUMER) {
        return captureCpuConsumerFrameToRgb(gui, output);
    }
    return captureImageReaderFrameToRgb(output);
}

}  // namespace

struct ScreenrecordRawCapture {
    Options opts;
    GuiApi gui;
    bool guiOpened = false;
    android::PhysicalDisplayId physicalId;
    android::ui::DisplayState displayState;
    FrameOutput frameOutput;
    VirtualDisplay virtualDisplay;
    bool virtualDisplayReady = false;
};

static void screenrecord_raw_capture_cleanup(ScreenrecordRawCapture* capture) {
    if (!capture) {
        return;
    }
    if (capture->virtualDisplayReady) {
        capture->gui.destroyDisplayCompat(
                capture->virtualDisplay.display,
                capture->virtualDisplay.legacyDisplay);
        capture->virtualDisplayReady = false;
    }
    destroyFrameOutput(capture->gui, &capture->frameOutput);
}

static int screenrecord_raw_capture_create_internal(
        int width, int height,
        bool rawCropSpecified,
        int rawCropLeft,
        int rawCropTop,
        int rawCropWidth,
        int rawCropHeight,
        ScreenrecordRawCaptureBackend backend,
        ScreenrecordRawCapture** out) {
    if (!out) {
        return EINVAL;
    }
    *out = nullptr;
    if (width <= 0 || height <= 0 ||
        (rawCropSpecified &&
         (rawCropLeft < 0 || rawCropTop < 0 || rawCropWidth <= 0 || rawCropHeight <= 0))) {
        return EINVAL;
    }

    auto* capture = new ScreenrecordRawCapture();
    gStopRequested = 0;
    capture->opts.backend = backend;
    capture->opts.videoWidth = floorToEven(static_cast<uint32_t>(width));
    capture->opts.videoHeight = floorToEven(static_cast<uint32_t>(height));
    if (capture->opts.videoWidth == 0 || capture->opts.videoHeight == 0) {
        delete capture;
        return EINVAL;
    }
    capture->opts.rawCropSpecified = rawCropSpecified;
    capture->opts.rawCropLeft = static_cast<uint32_t>(rawCropLeft);
    capture->opts.rawCropTop = static_cast<uint32_t>(rawCropTop);
    capture->opts.rawCropWidth = floorToEven(static_cast<uint32_t>(rawCropWidth));
    capture->opts.rawCropHeight = floorToEven(static_cast<uint32_t>(rawCropHeight));
    if (rawCropSpecified &&
        (capture->opts.rawCropWidth == 0 || capture->opts.rawCropHeight == 0)) {
        delete capture;
        return EINVAL;
    }

    if (!ensureGuiOpen(capture->gui, &capture->guiOpened)) {
        screenrecord_raw_capture_cleanup(capture);
        delete capture;
        return android::UNKNOWN_ERROR;
    }

    android::sp<android::ProcessState> processState = capture->gui.processStateSelf();
    if (processState) {
        capture->gui.processStateStartThreadPool(processState.get());
    }

    std::vector<android::PhysicalDisplayId> ids = capture->gui.getPhysicalDisplayIds();
    if (ids.empty()) {
        fprintf(stderr, "ERROR: Failed to get physical display ids\n");
        screenrecord_raw_capture_cleanup(capture);
        delete capture;
        return android::INVALID_OPERATION;
    }
    android::sp<android::IBinder> displayToken;
    for (const android::PhysicalDisplayId id : ids) {
        android::sp<android::IBinder> candidateToken =
                capture->gui.getPhysicalDisplayToken(id);
        if (!candidateToken) {
            continue;
        }
        android::ui::DisplayState candidateState;
        android::status_t stateErr =
                capture->gui.getDisplayState(candidateToken, &candidateState);
        if (stateErr != android::NO_ERROR ||
            candidateState.layerStack.id == kInvalidLayerStack) {
            continue;
        }
        capture->physicalId = id;
        displayToken = candidateToken;
        capture->displayState = candidateState;
        break;
    }
    if (!displayToken) {
        fprintf(stderr, "ERROR: failed to find display with a valid layer stack\n");
        screenrecord_raw_capture_cleanup(capture);
        delete capture;
        return android::UNKNOWN_ERROR;
    }

    android::ui::DisplayMode displayMode;
    android::status_t err =
            capture->gui.getActiveDisplayMode(displayToken, &displayMode);
    if (err != android::NO_ERROR) {
        fprintf(stderr, "ERROR: unable to get display config\n");
        screenrecord_raw_capture_cleanup(capture);
        delete capture;
        return err;
    }

    err = prepareFrameOutput(capture->gui, capture->opts, &capture->frameOutput);
    if (err != android::NO_ERROR) {
        screenrecord_raw_capture_cleanup(capture);
        delete capture;
        return err;
    }

    err = prepareVirtualDisplay(
            capture->gui,
            capture->opts,
            capture->physicalId,
            capture->displayState,
            capture->frameOutput.inputProducer,
            &capture->virtualDisplay);
    if (err != android::NO_ERROR) {
        screenrecord_raw_capture_cleanup(capture);
        delete capture;
        return err;
    }
    capture->virtualDisplayReady = true;
    *out = capture;
    return android::NO_ERROR;
}

extern "C" int screenrecord_raw_capture_create(
        int width, int height, ScreenrecordRawCapture** out) {
    return screenrecord_raw_capture_create_internal(
            width, height, false, 0, 0, 0, 0,
            SCREENRECORD_RAW_CAPTURE_BACKEND_CPU_CONSUMER, out);
}

extern "C" int screenrecord_raw_capture_create_crop(
        int width, int height,
        int cropLeft, int cropTop, int cropWidth, int cropHeight,
        ScreenrecordRawCapture** out) {
    return screenrecord_raw_capture_create_internal(
            width, height, true, cropLeft, cropTop, cropWidth, cropHeight,
            SCREENRECORD_RAW_CAPTURE_BACKEND_CPU_CONSUMER, out);
}

extern "C" int screenrecord_raw_capture_create_crop_backend(
        int width, int height,
        int cropLeft, int cropTop, int cropWidth, int cropHeight,
        ScreenrecordRawCaptureBackend backend,
        ScreenrecordRawCapture** out) {
    if (backend != SCREENRECORD_RAW_CAPTURE_BACKEND_AIMAGE_READER &&
        backend != SCREENRECORD_RAW_CAPTURE_BACKEND_CPU_CONSUMER) {
        return EINVAL;
    }
    return screenrecord_raw_capture_create_internal(
            width, height, true, cropLeft, cropTop, cropWidth, cropHeight, backend, out);
}

extern "C" int screenrecord_raw_capture_read(
        ScreenrecordRawCapture* capture,
        const uint8_t** data,
        size_t* size,
        int* width,
        int* height) {
    if (!capture || !data || !size || !width || !height) {
        return EINVAL;
    }

    updateDisplayProjection(
            capture->gui,
            capture->opts,
            capture->physicalId,
            capture->virtualDisplay.display,
            &capture->displayState);

    android::status_t err = captureFrameToRgb(capture->gui, capture->frameOutput);
    if (err == ETIMEDOUT) {
        return ETIMEDOUT;
    }
    if (err != android::NO_ERROR) {
        return err;
    }

    *data = capture->frameOutput.rgbBuffer.data();
    *size = capture->frameOutput.rgbBuffer.size();
    *width = static_cast<int>(capture->frameOutput.width);
    *height = static_cast<int>(capture->frameOutput.height);
    return android::NO_ERROR;
}

extern "C" void screenrecord_raw_capture_destroy(ScreenrecordRawCapture* capture) {
    if (!capture) {
        return;
    }
    gStopRequested = 1;
    screenrecord_raw_capture_cleanup(capture);
    delete capture;
}
