package com.enenh.ai.shizuku;

import static android.content.pm.PackageManager.PERMISSION_GRANTED;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Context;
import android.content.ServiceConnection;
import android.content.pm.ApplicationInfo;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.RemoteException;
import android.widget.Toast;

import rikka.shizuku.Shizuku;

public final class ShizukuBridge {
    public interface Task {
        String run(IPrivilegedService service) throws Exception;
    }

    public interface Callback {
        void onComplete(String message);
        void onError(Throwable error);
    }

    private ShizukuBridge() {
    }

    public static boolean ensurePermission(Activity activity, int requestCode) {
        try {
            if (Shizuku.isPreV11()) {
                toast(activity, "Shizuku pre-v11 is not supported");
                return false;
            }
            if (Shizuku.checkSelfPermission() == PERMISSION_GRANTED) {
                return true;
            }
            if (Shizuku.shouldShowRequestPermissionRationale()) {
                toast(activity, "Shizuku permission denied");
                return false;
            }
            Shizuku.requestPermission(requestCode);
            return false;
        } catch (Throwable e) {
            toast(activity, "Shizuku is not running");
            return false;
        }
    }

    public static void runWithService(Activity activity, String tag, Task task, Callback callback) {
        Handler handler = new Handler(Looper.getMainLooper());
        ComponentName componentName = new ComponentName(
                activity.getPackageName(), PrivilegedService.class.getName());
        Shizuku.UserServiceArgs args = new Shizuku.UserServiceArgs(componentName)
                .daemon(false)
                .processNameSuffix(tag)
                .debuggable((activity.getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0)
                .version(1);

        ServiceConnection connection = new ServiceConnection() {
            @Override
            public void onServiceConnected(ComponentName name, IBinder binder) {
                new Thread(() -> {
                    IPrivilegedService service = IPrivilegedService.Stub.asInterface(binder);
                    try {
                        String message = task.run(service);
                        safeDestroy(service);
                        unbind(args, this);
                        handler.post(() -> callback.onComplete(message));
                    } catch (Throwable e) {
                        safeDestroy(service);
                        unbind(args, this);
                        handler.post(() -> callback.onError(e));
                    }
                }, "shizuku-task").start();
            }

            @Override
            public void onServiceDisconnected(ComponentName name) {
            }
        };

        try {
            Shizuku.bindUserService(args, connection);
        } catch (Throwable e) {
            callback.onError(e);
        }
    }

    public static void toast(Context context, String message) {
        Toast.makeText(context, message, Toast.LENGTH_LONG).show();
    }

    private static void safeDestroy(IPrivilegedService service) {
        try {
            service.destroy();
        } catch (RemoteException ignored) {
        }
    }

    private static void unbind(Shizuku.UserServiceArgs args, ServiceConnection connection) {
        try {
            Shizuku.unbindUserService(args, connection, true);
        } catch (Throwable ignored) {
        }
    }
}
