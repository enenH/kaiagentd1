#ifndef REALTIME_JVM_RUNTIME_H
#define REALTIME_JVM_RUNTIME_H

#include <jni.h>

struct JvmContext {
    JavaVM *vm   = nullptr;
    JNIEnv *env  = nullptr;
    void   *invoc = nullptr;
};

bool jvm_create(JvmContext &ctx, const char **options = nullptr, int num_options = 0);

#endif  // REALTIME_JVM_RUNTIME_H
