#include "jvm_runtime.h"
#include "native_log.h"

#include <dlfcn.h>
#include <cstdlib>
#include <cstring>

#define ANDROID_RUNTIME_DSO "libandroid_runtime.so"

/* Function pointer types */
typedef jint (*JNI_CreateJavaVM_t)(JavaVM**, JNIEnv**, void*);
typedef int  (*StartReg_t)(JNIEnv*);

/* JniInvocation types (new API) */
struct JniInvocationImpl;
typedef JniInvocationImpl* (*JniInvocationCreate_t)();
typedef bool (*JniInvocationInit_t)(JniInvocationImpl*, const char*);

/* JniInvocation types (legacy mangled C++ API) */
typedef void (*JniInvocation_ctor_t)(void*);
typedef bool (*JniInvocation_Init_t)(void*, const char*);

bool jvm_create(JvmContext &ctx, const char **options, int num_options)
{
    void *runtime_dso = dlopen(ANDROID_RUNTIME_DSO, RTLD_NOW);
    if (!runtime_dso) {
        LOGE("dlopen(%s) failed: %s", ANDROID_RUNTIME_DSO, dlerror());
        return false;
    }

    /* Resolve JNI_CreateJavaVM */
    auto JNI_CreateJVM = (JNI_CreateJavaVM_t)dlsym(runtime_dso, "JNI_CreateJavaVM");
    if (!JNI_CreateJVM) {
        LOGE("Cannot find JNI_CreateJavaVM");
        dlclose(runtime_dso);
        return false;
    }

    /* Resolve startReg */
    auto startReg = (StartReg_t)dlsym(runtime_dso,
        "_ZN7android14AndroidRuntime8startRegEP7_JNIEnv");

    /* Initialize JniInvocation */
    auto JniInvocationCreate = (JniInvocationCreate_t)dlsym(runtime_dso, "JniInvocationCreate");
    auto JniInvocationInit   = (JniInvocationInit_t)dlsym(runtime_dso, "JniInvocationInit");

    if (JniInvocationCreate && JniInvocationInit) {
        ctx.invoc = JniInvocationCreate();
        JniInvocationInit((JniInvocationImpl*)ctx.invoc, ANDROID_RUNTIME_DSO);
    } else {
        /* Legacy path: mangled C++ symbols */
        auto ctor = (JniInvocation_ctor_t)dlsym(runtime_dso, "_ZN13JniInvocationC1Ev");
        auto init = (JniInvocation_Init_t)dlsym(runtime_dso, "_ZN13JniInvocation4InitEPKc");

        if (!ctor || !init) {
            LOGE("Cannot find JniInvocation symbols");
            dlclose(runtime_dso);
            return false;
        }

        ctx.invoc = calloc(1, 256);
        ctor(ctx.invoc);
        init(ctx.invoc, nullptr);
    }

    /* Set up JavaVM options */
    JavaVMOption *vm_options = nullptr;
    if (num_options > 0) {
        vm_options = new JavaVMOption[num_options];
        for (int i = 0; i < num_options; ++i)
            vm_options[i].optionString = const_cast<char*>(options[i]);
    }

    JavaVMInitArgs args;
    args.version            = JNI_VERSION_1_6;
    args.nOptions           = num_options;
    args.options            = vm_options;
    args.ignoreUnrecognized = JNI_TRUE;

    jint status = JNI_CreateJVM(&ctx.vm, &ctx.env, &args);
    delete[] vm_options;

    if (status == JNI_ERR) {
        LOGE("JNI_CreateJavaVM failed");
        return false;
    }

    LOGD("JavaVM created: vm=%p, env=%p", ctx.vm, ctx.env);

    /* Set AndroidRuntime::mJavaVM */
    void *mJavaVM = dlsym(runtime_dso, "_ZN7android14AndroidRuntime7mJavaVME");
    if (mJavaVM) {
        *(void**)mJavaVM = ctx.vm;
    }

    /* Register framework natives */
    if (startReg) {
        startReg(ctx.env);
    }

    return true;
}
