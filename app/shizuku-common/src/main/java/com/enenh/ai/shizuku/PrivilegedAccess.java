package com.enenh.ai.shizuku;

import static android.content.pm.PackageManager.PERMISSION_GRANTED;

import android.app.Activity;
import android.os.Handler;
import android.os.Looper;

import rikka.shizuku.Shizuku;

public final class PrivilegedAccess {
    public static final String REQUIREMENT = "权限：Shizuku 或 su 任一可用";
    public static final String NO_ACCESS_MESSAGE = "需要 Shizuku 授权或 su root 授权";
    public static final String SHIZUKU_DENIED_MESSAGE = "Shizuku 未授权，可改用 su root";

    private static volatile SuState suState_ = SuState.Unknown;

    public interface Task {
        String run(PrivilegedClient client) throws Exception;
    }

    public interface Callback {
        void onComplete(String message);

        void onError(Throwable error);

        default void onPending(String message) {
        }
    }

    private enum ShizukuRequestState {
        Granted,
        Requested,
        Unavailable
    }

    private enum SuState {
        Unknown,
        Available,
        Unavailable
    }

    private PrivilegedAccess() {
    }

    public static boolean hasShizukuPermission() {
        try {
            return !Shizuku.isPreV11() && Shizuku.checkSelfPermission() == PERMISSION_GRANTED;
        } catch (Throwable e) {
            return false;
        }
    }

    public static boolean hasReusableSu() {
        return suState_ == SuState.Available;
    }

    public static void run(Activity activity, String tag, int requestCode,
                           Task task, Callback callback) {
        if (hasShizukuPermission()) {
            runWithShizuku(activity, tag, task, callback);
            return;
        }
        runWithSu(activity, true, task, new Callback() {
            @Override
            public void onComplete(String message) {
                callback.onComplete(message);
            }

            @Override
            public void onError(Throwable error) {
                ShizukuRequestState state = requestShizuku(activity, requestCode);
                if (state == ShizukuRequestState.Granted) {
                    runWithShizuku(activity, tag, task, callback);
                } else if (state == ShizukuRequestState.Requested) {
                    callback.onPending("请完成 Shizuku 授权");
                } else {
                    callback.onError(new IllegalStateException(formatSuError(error)));
                }
            }
        });
    }

    public static void runKnown(Activity activity, String tag, Task task, Callback callback) {
        if (hasShizukuPermission()) {
            runWithShizuku(activity, tag, task, callback);
            return;
        }
        if (suState_ == SuState.Available) {
            runWithSu(activity, false, task, callback);
            return;
        }
        if (suState_ == SuState.Unknown) {
            runWithSu(activity, true, task, callback);
            return;
        }
        callback.onError(new IllegalStateException(NO_ACCESS_MESSAGE));
    }

    private static void runWithShizuku(Activity activity, String tag,
                                       Task task, Callback callback) {
        ShizukuBridge.runWithService(activity, tag, service -> {
            PrivilegedClient client = new ShizukuPrivilegedClient(service);
            return task.run(client);
        }, new ShizukuBridge.Callback() {
            @Override
            public void onComplete(String message) {
                callback.onComplete(message);
            }

            @Override
            public void onError(Throwable error) {
                callback.onError(error);
            }
        });
    }

    private static void runWithSu(Activity activity, boolean probe,
                                  Task task, Callback callback) {
        Handler handler = new Handler(Looper.getMainLooper());
        new Thread(() -> {
            SuPrivilegedClient client = null;
            try {
                client = SuPrivilegedClient.open(activity, probe);
                suState_ = SuState.Available;
                String message = task.run(client);
                handler.post(() -> callback.onComplete(message));
            } catch (Throwable e) {
                if (client == null) {
                    suState_ = SuState.Unavailable;
                }
                handler.post(() -> callback.onError(e));
            } finally {
                if (client != null) {
                    client.close();
                }
            }
        }, "su-privileged-task").start();
    }

    private static ShizukuRequestState requestShizuku(Activity activity, int requestCode) {
        try {
            if (Shizuku.isPreV11()) {
                return ShizukuRequestState.Unavailable;
            }
            if (Shizuku.checkSelfPermission() == PERMISSION_GRANTED) {
                return ShizukuRequestState.Granted;
            }
            if (Shizuku.shouldShowRequestPermissionRationale()) {
                return ShizukuRequestState.Unavailable;
            }
            Shizuku.requestPermission(requestCode);
            return ShizukuRequestState.Requested;
        } catch (Throwable e) {
            return ShizukuRequestState.Unavailable;
        }
    }

    private static String formatSuError(Throwable error) {
        String detail = error == null ? "" : error.getMessage();
        if (detail == null) {
            detail = "";
        }
        String lower = detail.toLowerCase();
        if (lower.contains("timeout")) {
            return "su 授权超时，请检查 root 管理器弹窗";
        }
        if (lower.contains("permission denied") || lower.contains("not granted")
                || lower.contains("uid=0")) {
            return "su root 未授权" + appendDetail(detail);
        }
        if (detail.isEmpty()) {
            return NO_ACCESS_MESSAGE;
        }
        return "su root 探测失败: " + detail;
    }

    private static String appendDetail(String detail) {
        return detail.isEmpty() ? "" : ": " + detail;
    }
}
