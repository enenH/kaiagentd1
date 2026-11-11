#include "overlay_surface.h"
#include "native_log.h"

#include <android/native_window_jni.h>
#include <android/hardware_buffer.h>
#include <algorithm>
#include <random>
#include <string>

namespace {

std::string makeRandomSurfaceName()
{
    constexpr char kSurfaceNameChars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    constexpr size_t kSurfaceNameLength = 12;

    std::string name;
    name.reserve(kSurfaceNameLength);

    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<size_t> dist(0, sizeof(kSurfaceNameChars) - 2);

    for (size_t i = 0; i < kSurfaceNameLength; ++i)
        name.push_back(kSurfaceNameChars[dist(rng)]);

    return name;
}

const char *resolveSurfaceName(const char *name)
{
    if (name && name[0] != '\0')
        return name;

    static const std::string s_defaultName = makeRandomSurfaceName();
    return s_defaultName.c_str();
}

} // namespace

/*
 * Helper: check and clear JNI exception, return true if one occurred.
 */
static bool check_exception(JNIEnv *env)
{
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        return true;
    }
    return false;
}

namespace {

constexpr int kTopLayer = 0x7FFFFFFF;

struct ClassCache {
    jclass cls = nullptr;
    bool lookedUp = false;
};

struct MethodCache {
    jmethodID id = nullptr;
    bool lookedUp = false;
};

struct FieldCache {
    jfieldID id = nullptr;
    bool lookedUp = false;
};

struct BuilderJni {
    ClassCache cls;
    MethodCache ctor;
    MethodCache setName;
    MethodCache setBufferSize;
    MethodCache setDefaultSize;
    MethodCache setFormat;
    MethodCache build;
};

struct TransactionJni {
    ClassCache cls;
    MethodCache ctor;
    MethodCache setLayer;
    MethodCache setTrustedOverlay;
    MethodCache setSkipScreenshot;
    MethodCache show;
    MethodCache apply;
};

struct SurfaceControlJni {
    ClassCache cls;
    MethodCache release;
};

struct SurfaceJni {
    ClassCache cls;
    MethodCache ctor;
    MethodCache release;
};

struct DisplayManagerJni {
    ClassCache cls;
    MethodCache getInstance;
    MethodCache getDisplayInfo;
};

struct DisplayInfoJni {
    ClassCache cls;
    FieldCache logicalWidth;
    FieldCache logicalHeight;
    FieldCache rotation;
};

class ScopedLocalFrame {
public:
    ScopedLocalFrame(JNIEnv *env, jint capacity) : env_(env)
    {
        active_ = env_->PushLocalFrame(capacity) == 0;
        if (!active_)
            check_exception(env_);
    }

    ~ScopedLocalFrame()
    {
        if (active_)
            env_->PopLocalFrame(nullptr);
    }

    bool ok() const { return active_; }

private:
    JNIEnv *env_ = nullptr;
    bool active_ = false;
};


bool clear_exception(JNIEnv *env)
{
    if (!env->ExceptionCheck())
        return false;

    env->ExceptionClear();
    return true;
}

bool consume_exception(JNIEnv *env, bool describe)
{
    return describe ? check_exception(env) : clear_exception(env);
}

BuilderJni &builder_jni()
{
    static BuilderJni cache;
    return cache;
}

TransactionJni &transaction_jni()
{
    static TransactionJni cache;
    return cache;
}

SurfaceControlJni &surface_control_jni()
{
    static SurfaceControlJni cache;
    return cache;
}

SurfaceJni &surface_jni()
{
    static SurfaceJni cache;
    return cache;
}

DisplayManagerJni &display_manager_jni()
{
    static DisplayManagerJni cache;
    return cache;
}

DisplayInfoJni &display_info_jni()
{
    static DisplayInfoJni cache;
    return cache;
}

jclass cached_class(JNIEnv *env, ClassCache &cache, const char *name,
                    bool describe)
{
    if (cache.lookedUp)
        return cache.cls;

    cache.lookedUp = true;
    jclass local = env->FindClass(name);
    if (consume_exception(env, describe) || !local)
        return nullptr;

    auto global = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    if (consume_exception(env, describe) || !global)
        return nullptr;

    cache.cls = global;
    return cache.cls;
}

jclass cached_object_class(JNIEnv *env, jobject obj, ClassCache &cache,
                           bool describe)
{
    if (cache.lookedUp)
        return cache.cls;

    cache.lookedUp = true;
    jclass local = env->GetObjectClass(obj);
    if (consume_exception(env, describe) || !local)
        return nullptr;

    auto global = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    if (consume_exception(env, describe) || !global)
        return nullptr;

    cache.cls = global;
    return cache.cls;
}

jmethodID cached_method(JNIEnv *env, jclass cls, MethodCache &cache,
                        const char *name, const char *sig, bool describe)
{
    if (cache.lookedUp)
        return cache.id;

    cache.lookedUp = true;
    cache.id = env->GetMethodID(cls, name, sig);
    if (consume_exception(env, describe) || !cache.id)
        cache.id = nullptr;
    return cache.id;
}

jmethodID cached_static_method(JNIEnv *env, jclass cls, MethodCache &cache,
                               const char *name, const char *sig,
                               bool describe)
{
    if (cache.lookedUp)
        return cache.id;

    cache.lookedUp = true;
    cache.id = env->GetStaticMethodID(cls, name, sig);
    if (consume_exception(env, describe) || !cache.id)
        cache.id = nullptr;
    return cache.id;
}

jfieldID cached_field(JNIEnv *env, jclass cls, FieldCache &cache,
                      const char *name, const char *sig, bool describe)
{
    if (cache.lookedUp)
        return cache.id;

    cache.lookedUp = true;
    cache.id = env->GetFieldID(cls, name, sig);
    if (consume_exception(env, describe) || !cache.id)
        cache.id = nullptr;
    return cache.id;
}

jfieldID cached_static_field(JNIEnv *env, jclass cls, FieldCache &cache,
                             const char *name, const char *sig,
                             bool describe)
{
    if (cache.lookedUp)
        return cache.id;

    cache.lookedUp = true;
    cache.id = env->GetStaticFieldID(cls, name, sig);
    if (consume_exception(env, describe) || !cache.id)
        cache.id = nullptr;
    return cache.id;
}

unsigned long long monotonic_ms()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<unsigned long long>(ts.tv_sec) * 1000ULL +
           static_cast<unsigned long long>(ts.tv_nsec / 1000000ULL);
}

bool read_display_int_field(JNIEnv *env, jobject obj, FieldCache &cache,
                            const char *name, int &value, bool describe)
{
    auto &info = display_info_jni();
    jclass cls = cached_object_class(env, obj, info.cls, describe);
    if (!cls)
        return false;

    jfieldID field = cached_field(env, cls, cache, name, "I", describe);
    if (!field)
        return false;

    value = env->GetIntField(obj, field);
    return !consume_exception(env, describe);
}

void release_surface_control(JNIEnv *env, jobject surfaceControl)
{
    if (!surfaceControl)
        return;

    auto &sc = surface_control_jni();
    jclass scClass = cached_class(env, sc.cls, "android/view/SurfaceControl",
                                  false);
    if (!scClass)
        return;

    jmethodID release = cached_method(env, scClass, sc.release, "release",
                                      "()V", false);
    if (release)
        env->CallVoidMethod(surfaceControl, release);

    clear_exception(env);
}

void release_surface(JNIEnv *env, jobject surface)
{
    if (!surface)
        return;

    auto &surf = surface_jni();
    jclass surfaceClass = cached_class(env, surf.cls, "android/view/Surface",
                                       false);
    if (!surfaceClass)
        return;

    jmethodID release = cached_method(env, surfaceClass, surf.release,
                                      "release", "()V", false);
    if (release)
        env->CallVoidMethod(surface, release);

    clear_exception(env);
}

} // namespace

bool surface_create(JNIEnv *env, SurfaceContext &ctx, int width, int height,
                    const char *name, bool setSkipScreenshot)
{
    ScopedLocalFrame frame(env, 64);
    if (!frame.ok())
        return false;

    const char *effectiveName = resolveSurfaceName(name);

    jobject scGlobal = nullptr;
    jobject surfaceGlobal = nullptr;
    ANativeWindow *window = nullptr;

    auto cleanup = [&]() {
        if (window) {
            ANativeWindow_release(window);
            window = nullptr;
        }
        if (surfaceGlobal) {
            release_surface(env, surfaceGlobal);
            env->DeleteGlobalRef(surfaceGlobal);
            surfaceGlobal = nullptr;
        }
        if (scGlobal) {
            release_surface_control(env, scGlobal);
            env->DeleteGlobalRef(scGlobal);
            scGlobal = nullptr;
        }
    };

    /* ---- SurfaceControl.Builder ---- */

    auto &builderJni = builder_jni();
    jclass builderClass = cached_class(env, builderJni.cls,
        "android/view/SurfaceControl$Builder", true);
    if (!builderClass) {
        LOGE("Cannot find SurfaceControl$Builder class");
        return false;
    }

    jmethodID builderCtor = cached_method(env, builderClass, builderJni.ctor,
                                          "<init>", "()V", true);
    if (!builderCtor) {
        LOGE("Cannot find SurfaceControl$Builder constructor");
        return false;
    }

    jobject builder = env->NewObject(builderClass, builderCtor);
    if (check_exception(env) || !builder) {
        LOGE("Cannot create SurfaceControl$Builder");
        return false;
    }

    /* setName(String) */
    jmethodID setName = cached_method(env, builderClass, builderJni.setName,
        "setName", "(Ljava/lang/String;)Landroid/view/SurfaceControl$Builder;",
        false);
    if (setName) {
        jstring jname = env->NewStringUTF(effectiveName);
        if (!check_exception(env) && jname) {
            env->CallObjectMethod(builder, setName, jname);
            check_exception(env);
            env->DeleteLocalRef(jname);
        }
    }

    /* setBufferSize(int, int) */
    jmethodID setBufferSize = cached_method(env, builderClass,
        builderJni.setBufferSize, "setBufferSize",
        "(II)Landroid/view/SurfaceControl$Builder;", false);
    if (!setBufferSize) {
        /* API 31+: try setDefaultSize */
        setBufferSize = cached_method(env, builderClass,
            builderJni.setDefaultSize, "setDefaultSize",
            "(II)Landroid/view/SurfaceControl$Builder;", false);
    }
    if (setBufferSize) {
        env->CallObjectMethod(builder, setBufferSize, width, height);
        check_exception(env);
    }

    /* setFormat(int) - RGBA_8888 = 1 */
    jmethodID setFormat = cached_method(env, builderClass,
        builderJni.setFormat, "setFormat",
        "(I)Landroid/view/SurfaceControl$Builder;", false);
    if (setFormat) {
        env->CallObjectMethod(builder, setFormat, 1 /* RGBA_8888 */);
        check_exception(env);
    }

    /* build() -> SurfaceControl */
    jmethodID buildMethod = cached_method(env, builderClass, builderJni.build,
        "build", "()Landroid/view/SurfaceControl;", true);
    if (!buildMethod) {
        LOGE("Cannot find SurfaceControl$Builder.build()");
        env->DeleteLocalRef(builder);
        return false;
    }

    jobject sc = env->CallObjectMethod(builder, buildMethod);
    if (check_exception(env) || !sc) {
        LOGE("SurfaceControl.Builder.build() failed");
        env->DeleteLocalRef(builder);
        return false;
    }
    env->DeleteLocalRef(builder);

    scGlobal = env->NewGlobalRef(sc);
    if (check_exception(env) || !scGlobal) {
        LOGE("Cannot retain SurfaceControl");
        release_surface_control(env, sc);
        env->DeleteLocalRef(sc);
        return false;
    }
    env->DeleteLocalRef(sc);

    /* ---- SurfaceControl.Transaction ---- */

    auto &txnJni = transaction_jni();
    jclass txnClass = cached_class(env, txnJni.cls,
        "android/view/SurfaceControl$Transaction", true);
    if (!txnClass) {
        LOGE("Cannot find SurfaceControl$Transaction class");
        cleanup();
        return false;
    }

    jmethodID txnCtor = cached_method(env, txnClass, txnJni.ctor, "<init>",
                                      "()V", true);
    if (!txnCtor) {
        LOGE("Cannot find SurfaceControl$Transaction constructor");
        cleanup();
        return false;
    }

    jobject txn = env->NewObject(txnClass, txnCtor);
    if (check_exception(env) || !txn) {
        LOGE("Cannot create Transaction");
        cleanup();
        return false;
    }

    /* setLayer(SurfaceControl, int) */
    jmethodID setLayer = cached_method(env, txnClass, txnJni.setLayer,
        "setLayer",
        "(Landroid/view/SurfaceControl;I)Landroid/view/SurfaceControl$Transaction;",
        false);
    if (setLayer) {
        env->CallObjectMethod(txn, setLayer, scGlobal, kTopLayer);
        check_exception(env);
    }

    /* setTrustedOverlay(SurfaceControl, boolean) */
    jmethodID setTrustedOverlay = cached_method(env, txnClass,
        txnJni.setTrustedOverlay, "setTrustedOverlay",
        "(Landroid/view/SurfaceControl;Z)Landroid/view/SurfaceControl$Transaction;",
        false);
    if (setTrustedOverlay) {
        env->CallObjectMethod(txn, setTrustedOverlay, scGlobal, JNI_TRUE);
        check_exception(env);
    }

    if (setSkipScreenshot) {
        /* setSkipScreenshot(SurfaceControl, boolean) */
        jmethodID setSkipScreenshotMethod = cached_method(env, txnClass,
            txnJni.setSkipScreenshot, "setSkipScreenshot",
            "(Landroid/view/SurfaceControl;Z)Landroid/view/SurfaceControl$Transaction;",
            false);
        if (setSkipScreenshotMethod) {
            env->CallObjectMethod(txn, setSkipScreenshotMethod, scGlobal, JNI_TRUE);
            check_exception(env);
        }
    }

    /* show(SurfaceControl) */
    jmethodID show = cached_method(env, txnClass, txnJni.show, "show",
        "(Landroid/view/SurfaceControl;)Landroid/view/SurfaceControl$Transaction;",
        false);
    if (show) {
        env->CallObjectMethod(txn, show, scGlobal);
        check_exception(env);
    }

    /* apply() */
    jmethodID apply = cached_method(env, txnClass, txnJni.apply, "apply",
                                    "()V", false);
    if (apply) {
        env->CallVoidMethod(txn, apply);
        check_exception(env);
    }

    env->DeleteLocalRef(txn);

    /* ---- Surface from SurfaceControl ---- */

    auto &surfJni = surface_jni();
    jclass surfaceClass = cached_class(env, surfJni.cls, "android/view/Surface",
                                       true);
    if (!surfaceClass) {
        LOGE("Cannot find Surface class");
        cleanup();
        return false;
    }

    /* Create Surface(SurfaceControl) */
    jmethodID surfCtor = cached_method(env, surfaceClass, surfJni.ctor,
        "<init>", "(Landroid/view/SurfaceControl;)V", true);
    if (!surfCtor) {
        LOGE("Cannot find Surface(SurfaceControl) constructor");
        cleanup();
        return false;
    }

    jobject surf = env->NewObject(surfaceClass, surfCtor, scGlobal);
    if (check_exception(env) || !surf) {
        LOGE("Cannot create Surface from SurfaceControl");
        cleanup();
        return false;
    }

    surfaceGlobal = env->NewGlobalRef(surf);
    if (check_exception(env) || !surfaceGlobal) {
        LOGE("Cannot retain Surface");
        release_surface(env, surf);
        env->DeleteLocalRef(surf);
        cleanup();
        return false;
    }
    env->DeleteLocalRef(surf);

    /* ---- ANativeWindow ---- */

    window = ANativeWindow_fromSurface(env, surfaceGlobal);
    if (!window) {
        LOGE("ANativeWindow_fromSurface failed");
        cleanup();
        return false;
    }

    const int geometryResult = ANativeWindow_setBuffersGeometry(
        window, width, height, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
    if (geometryResult != 0)
        LOGW("ANativeWindow_setBuffersGeometry failed: %d", geometryResult);

    ctx.surfaceControl = scGlobal;
    ctx.surface = surfaceGlobal;
    ctx.window = window;
    ctx.width = width;
    ctx.height = height;

    scGlobal = nullptr;
    surfaceGlobal = nullptr;
    window = nullptr;

    LOGD("Surface created: %dx%d, window=%p", width, height, ctx.window);
    return true;
}

bool surface_get_display_size(JNIEnv *env, int &width, int &height, int *rotation)
{
    ScopedLocalFrame frame(env, 24);
    if (!frame.ok())
        return false;

    /*
     * Use DisplayManagerGlobal.getInstance().getDisplayInfo(0)
     * to get the real display resolution without needing a Context.
     *
     * Equivalent Java:
     *   DisplayManagerGlobal dmg = DisplayManagerGlobal.getInstance();
     *   DisplayInfo info = dmg.getDisplayInfo(Display.DEFAULT_DISPLAY);
     *   width  = info.logicalWidth;
     *   height = info.logicalHeight;
     */
    auto &dmgJni = display_manager_jni();
    jclass dmgClass = cached_class(env, dmgJni.cls,
        "android/hardware/display/DisplayManagerGlobal", true);
    if (!dmgClass) {
        LOGE("Cannot find DisplayManagerGlobal class");
        return false;
    }

    jmethodID getInstance = cached_static_method(env, dmgClass,
        dmgJni.getInstance, "getInstance",
        "()Landroid/hardware/display/DisplayManagerGlobal;", true);
    if (!getInstance) {
        LOGE("Cannot find DisplayManagerGlobal.getInstance()");
        return false;
    }

    jobject dmg = env->CallStaticObjectMethod(dmgClass, getInstance);
    if (check_exception(env) || !dmg) {
        LOGE("DisplayManagerGlobal.getInstance() failed");
        return false;
    }

    jmethodID getDisplayInfo = cached_method(env, dmgClass,
        dmgJni.getDisplayInfo, "getDisplayInfo", "(I)Landroid/view/DisplayInfo;",
        true);
    if (!getDisplayInfo) {
        LOGE("Cannot find getDisplayInfo()");
        env->DeleteLocalRef(dmg);
        return false;
    }

    jobject info = env->CallObjectMethod(dmg, getDisplayInfo, 0 /* DEFAULT_DISPLAY */);
    env->DeleteLocalRef(dmg);
    if (check_exception(env) || !info) {
        LOGE("getDisplayInfo(0) failed");
        return false;
    }

    auto &infoJni = display_info_jni();
    if (!read_display_int_field(env, info, infoJni.logicalWidth,
                                "logicalWidth", width, true) ||
        !read_display_int_field(env, info, infoJni.logicalHeight,
                                "logicalHeight", height, true)) {
        LOGE("Cannot find DisplayInfo width/height fields");
        env->DeleteLocalRef(info);
        return false;
    }

    if (rotation) {
        if (!read_display_int_field(env, info, infoJni.rotation, "rotation",
                                    *rotation, false)) {
            *rotation = 0;
        }
    }

    env->DeleteLocalRef(info);

    int rotValue = rotation ? *rotation : -1;
    static int lastWidth = -1;
    static int lastHeight = -1;
    static int lastRotation = -2;
    if (width != lastWidth || height != lastHeight || rotValue != lastRotation) {
        LOGD("Display size: %dx%d rotation=%d", width, height, rotValue);
        lastWidth = width;
        lastHeight = height;
        lastRotation = rotValue;
    }
    return true;
}

void surface_destroy(JNIEnv *env, SurfaceContext &ctx)
{
    ScopedLocalFrame frame(env, 16);

    if (ctx.window) {
        ANativeWindow_release(ctx.window);
        ctx.window = nullptr;
    }

    if (ctx.surface) {
        release_surface(env, ctx.surface);
        env->DeleteGlobalRef(ctx.surface);
        ctx.surface = nullptr;
    }

    if (ctx.surfaceControl) {
        release_surface_control(env, ctx.surfaceControl);
        env->DeleteGlobalRef(ctx.surfaceControl);
        ctx.surfaceControl = nullptr;
    }
}
