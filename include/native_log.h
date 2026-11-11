#ifndef REALTIME_NATIVE_LOG_H
#define REALTIME_NATIVE_LOG_H

#include <android/log.h>
#ifndef NDEBUG
#define LOG_TAG "kaiagentd_native"

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) ((void)0)
#define LOGW(...) ((void)0)
#define LOGD(...) ((void)0)
#define LOGE(...) ((void)0)
#endif
#endif  // REALTIME_NATIVE_LOG_H
