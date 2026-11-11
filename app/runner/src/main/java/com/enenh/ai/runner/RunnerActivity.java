package com.enenh.ai.runner;

import android.app.Activity;
import android.os.Bundle;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import com.enenh.ai.shizuku.DeviceCompatibility;
import com.enenh.ai.shizuku.PrivilegedAccess;
import com.enenh.ai.shizuku.PrivilegedClient;
import com.enenh.ai.shizuku.ShizukuBridge;

import rikka.shizuku.Shizuku;

public class RunnerActivity extends Activity {
    private static final int REQUEST_SHIZUKU = 20;
    private static final String NATIVE_BINARY_NAME = "libkaiagentd.so";
    private static final String REMOTE_ROOT = "/data/local/tmp/com.enenh.ai";
    private static final String REMOTE_LIB_DIR = REMOTE_ROOT + "/lib";
    private static final String REMOTE_NATIVE_BINARY = REMOTE_LIB_DIR + "/" + NATIVE_BINARY_NAME;
    private static final String REMOTE_QNN_CACHE = REMOTE_ROOT + "/qnn_cache";
    private static final String PID_FILE = REMOTE_ROOT + "/kaiagentd.pid";

    private Button button_;
    private TextView deviceText_;
    private TextView statusText_;
    private TextView permissionText_;
    private DeviceCompatibility.Result compatibility_;
    private boolean busy_;
    private boolean running_;

    private final Shizuku.OnRequestPermissionResultListener permissionListener_ =
            (requestCode, grantResult) -> {
                if (requestCode == REQUEST_SHIZUKU && grantResult == android.content.pm.PackageManager.PERMISSION_GRANTED) {
                    if (ensureDeviceSupported()) {
                        toggleRunner();
                    }
                } else if (requestCode == REQUEST_SHIZUKU) {
                    ShizukuBridge.toast(this, PrivilegedAccess.SHIZUKU_DENIED_MESSAGE);
                }
            };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER);
        int padding = dp(24);
        root.setPadding(padding, padding, padding, padding);

        TextView requirementText = new TextView(this);
        requirementText.setText(DeviceCompatibility.REQUIREMENT);
        requirementText.setTextSize(20.0f);
        requirementText.setGravity(Gravity.CENTER);
        root.addView(requirementText, textLayoutParams());

        deviceText_ = new TextView(this);
        deviceText_.setTextSize(15.0f);
        deviceText_.setGravity(Gravity.CENTER);
        root.addView(deviceText_, textLayoutParams());

        statusText_ = new TextView(this);
        statusText_.setTextSize(15.0f);
        statusText_.setGravity(Gravity.CENTER);
        root.addView(statusText_, textLayoutParams());

        permissionText_ = new TextView(this);
        permissionText_.setText(PrivilegedAccess.REQUIREMENT);
        permissionText_.setTextSize(15.0f);
        permissionText_.setGravity(Gravity.CENTER);
        root.addView(permissionText_, textLayoutParams());

        button_ = new Button(this);
        button_.setGravity(Gravity.CENTER);
        button_.setAllCaps(false);
        button_.setOnClickListener(v -> {
            if (busy_) {
                return;
            }
            if (!ensureDeviceSupported()) {
                return;
            }
            toggleRunner();
        });
        root.addView(button_, buttonLayoutParams());
        setContentView(root, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        Shizuku.addRequestPermissionResultListener(permissionListener_);
        refreshState();
    }

    @Override
    protected void onDestroy() {
        Shizuku.removeRequestPermissionResultListener(permissionListener_);
        super.onDestroy();
    }

    private void toggleRunner() {
        if (!ensureDeviceSupported()) {
            return;
        }
        busy_ = true;
        button_.setEnabled(false);
        button_.setText("Working...");
        PrivilegedAccess.run(this, "runner", REQUEST_SHIZUKU, this::toggleRemote,
                new PrivilegedAccess.Callback() {
            @Override
            public void onComplete(String message) {
                busy_ = false;
                button_.setEnabled(true);
                refreshState();
                ShizukuBridge.toast(RunnerActivity.this, message);
            }

            @Override
            public void onError(Throwable error) {
                busy_ = false;
                refreshState();
                ShizukuBridge.toast(RunnerActivity.this, error.getMessage());
            }

            @Override
            public void onPending(String message) {
                busy_ = false;
                refreshState();
                ShizukuBridge.toast(RunnerActivity.this, message);
            }
        });
    }

    private void refreshState() {
        if (!refreshCompatibilityState()) {
            return;
        }
        button_.setText("检测权限中...");
        PrivilegedAccess.runKnown(this, "state", service -> service.isRunning(PID_FILE)
                ? "running" : "stopped", new PrivilegedAccess.Callback() {
            @Override
            public void onComplete(String message) {
                if (refreshCompatibilityState()) {
                    running_ = "running".equals(message);
                    button_.setText(running_ ? "停止 kaiagentd" : "启动 kaiagentd");
                }
            }

            @Override
            public void onError(Throwable error) {
                if (refreshCompatibilityState()) {
                    button_.setText("启动/停止 kaiagentd");
                }
            }
        });
    }

    private String toggleRemote(PrivilegedClient service) throws Exception {
        DeviceCompatibility.Result compatibility = DeviceCompatibility.current();
        if (!compatibility.supported) {
            throw new IllegalStateException(compatibility.reason);
        }
        if (service.isRunning(PID_FILE)) {
            return service.stopDaemon(PID_FILE);
        }
        ensureAiEnvironmentReady(service);
        runOnUiThread(() -> button_.setText("Copying binary..."));
        copyNativeBinaryToRemote(service);
        runOnUiThread(() -> button_.setText("Checking binary..."));
        ensureNativeBinaryExecutable(service, REMOTE_NATIVE_BINARY);
        service.runCommand("mkdir -p " + quote(REMOTE_QNN_CACHE), 5000);
        runOnUiThread(() -> button_.setText("Starting kaiagentd..."));
        return service.startDaemon(startCommand(REMOTE_NATIVE_BINARY), PID_FILE);
    }

    private void ensureAiEnvironmentReady(PrivilegedClient service) throws Exception {
        String command = "test -d " + quote(REMOTE_ROOT)
                + " && test -d " + quote(REMOTE_LIB_DIR)
                + " && test -f " + quote(REMOTE_LIB_DIR + "/libtensorflowlite_jni.so")
                + " && test -f " + quote(REMOTE_LIB_DIR + "/libQnnTFLiteDelegate.so")
                + " && test -f " + quote(REMOTE_LIB_DIR + "/model_fast256_int8.tflite");
        String result = service.runCommand(command, 5000);
        if (!result.startsWith("exit=0")) {
            throw new IllegalStateException("缺少运行库或模型。请按 README 准备本地资源并构建 Kai Assets Installer，然后初始化 AI 环境");
        }
    }

    private void copyNativeBinaryToRemote(PrivilegedClient service) throws Exception {
        String command = "mkdir -p " + quote(REMOTE_LIB_DIR)
                + " && cp -f " + quote(nativeBinaryPath()) + " " + quote(REMOTE_NATIVE_BINARY)
                + " && chmod 755 " + quote(REMOTE_NATIVE_BINARY);
        String result = service.runCommand(command, 10000);
        if (!result.startsWith("exit=0")) {
            throw new IllegalStateException("复制 runner native binary 到远端失败: "
                    + result.trim());
        }
    }

    private void ensureNativeBinaryExecutable(PrivilegedClient service, String nativeBinary)
            throws Exception {
        String result = service.runCommand("test -x " + quote(nativeBinary), 5000);
        if (!result.startsWith("exit=0")) {
            throw new IllegalStateException("native binary is not executable: "
                    + nativeBinary + "\n" + result.trim());
        }
    }

    private String nativeBinaryPath() {
        return getApplicationInfo().nativeLibraryDir + "/" + NATIVE_BINARY_NAME;
    }

    private String startCommand(String nativeBinary) {
        String nativeLibDir = getApplicationInfo().nativeLibraryDir;
        return "export LD_LIBRARY_PATH=" + quote(REMOTE_LIB_DIR + ":" + nativeLibDir) + "; "
                + "export ADSP_LIBRARY_PATH=" + quote(REMOTE_LIB_DIR + ";/vendor/lib/rfsa/adsp") + "; "
                + "export CDSP_LIBRARY_PATH=" + quote(REMOTE_LIB_DIR) + "; "
                + "export QNN_SIGNED_PD=1; "
                + "cd " + quote(REMOTE_LIB_DIR) + "; "
                + "exec " + quote(nativeBinary);
    }

    private boolean ensureDeviceSupported() {
        if (refreshCompatibilityState()) {
            return true;
        }
        ShizukuBridge.toast(this, compatibility_.reason);
        return false;
    }

    private boolean refreshCompatibilityState() {
        compatibility_ = DeviceCompatibility.current();
        deviceText_.setText(compatibility_.deviceLine());
        statusText_.setText(compatibility_.statusLine());
        if (busy_) {
            button_.setEnabled(false);
            return compatibility_.supported;
        }
        button_.setEnabled(compatibility_.supported);
        if (!compatibility_.supported) {
            button_.setText("设备不支持");
        }
        return compatibility_.supported;
    }

    private LinearLayout.LayoutParams textLayoutParams() {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        params.setMargins(0, 0, 0, dp(12));
        return params;
    }

    private LinearLayout.LayoutParams buttonLayoutParams() {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        params.setMargins(0, dp(8), 0, 0);
        return params;
    }

    private int dp(int value) {
        return (int) (value * getResources().getDisplayMetrics().density + 0.5f);
    }

    private static String quote(String value) {
        return "'" + value.replace("'", "'\\''") + "'";
    }
}
