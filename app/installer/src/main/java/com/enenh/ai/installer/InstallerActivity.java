package com.enenh.ai.installer;

import android.app.Activity;
import android.content.res.AssetFileDescriptor;
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

import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

import rikka.shizuku.Shizuku;

public class InstallerActivity extends Activity {
    private static final int REQUEST_SHIZUKU = 10;
    private static final int CHUNK_SIZE = 64 * 1024;
    private static final String REMOTE_ROOT = "/data/local/tmp/com.enenh.ai";
    private static final String REMOTE_LIB_DIR = REMOTE_ROOT + "/lib";
    private static final String REMOTE_QNN_CACHE = REMOTE_ROOT + "/qnn_cache";
    private static final String[] DEVICE_LIBRARY_DIRS = {
            "/vendor/lib64",
            "/system_ext/lib64",
            "/product/lib64",
            "/system/lib64",
            "/apex/com.android.runtime/lib64",
            "/apex/com.android.runtime/lib64/bionic"
    };
    private static final String[] SYSTEM_LINKER_DIRS = {
            "/system/lib64",
            "/apex/com.android.runtime/lib64",
            "/apex/com.android.runtime/lib64/bionic"
    };
    private static final List<RuntimeDependency> RUNTIME_DEPENDENCIES = Arrays.asList(
            requiredDependency("ld-android.so", SYSTEM_LINKER_DIRS),
            requiredDependency("libdl_android.so", SYSTEM_LINKER_DIRS),
            requiredDependency("libc++.so", DEVICE_LIBRARY_DIRS),
            requiredDependency("libbase.so", DEVICE_LIBRARY_DIRS),
            requiredDependency("libcutils.so", DEVICE_LIBRARY_DIRS),
            requiredDependency("libvndksupport.so", SYSTEM_LINKER_DIRS),
            requiredDependency("libutils.so", DEVICE_LIBRARY_DIRS),
            requiredDependency("libdmabufheap.so", DEVICE_LIBRARY_DIRS),
            requiredDependency("libhardware.so", DEVICE_LIBRARY_DIRS),
            requiredDependency("libhidlbase.so", DEVICE_LIBRARY_DIRS),
            optionalDependency("libhwbinder.so", DEVICE_LIBRARY_DIRS),
            optionalDependency("libhidltransport.so", DEVICE_LIBRARY_DIRS),
            optionalDependency("android.hidl.memory@1.0.so", DEVICE_LIBRARY_DIRS),
            optionalDependency("android.hidl.memory.token@1.0.so", DEVICE_LIBRARY_DIRS),
            optionalDependency("libhidlmemory.so", DEVICE_LIBRARY_DIRS),
            requiredDependency("android.hardware.common-V2-ndk.so", DEVICE_LIBRARY_DIRS),
            requiredDependency("vendor.qti.hardware.dsp-V1-ndk.so", DEVICE_LIBRARY_DIRS),
            optionalDependency("vendor.qti.hardware.dsp@1.0.so", DEVICE_LIBRARY_DIRS),
            requiredDependency("libvmmem.so", DEVICE_LIBRARY_DIRS),
            requiredDependency("libcdsprpc.so", DEVICE_LIBRARY_DIRS)
    );

    private Button button_;
    private TextView deviceText_;
    private TextView statusText_;
    private TextView permissionText_;
    private DeviceCompatibility.Result compatibility_;
    private boolean busy_;

    private final Shizuku.OnRequestPermissionResultListener permissionListener_ =
            (requestCode, grantResult) -> {
                if (requestCode == REQUEST_SHIZUKU && grantResult == android.content.pm.PackageManager.PERMISSION_GRANTED) {
                    if (ensureDeviceSupported()) {
                        installPayload();
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
            installPayload();
        });
        root.addView(button_, buttonLayoutParams());
        setContentView(root, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        Shizuku.addRequestPermissionResultListener(permissionListener_);
        refreshCompatibilityState();
    }

    @Override
    protected void onDestroy() {
        Shizuku.removeRequestPermissionResultListener(permissionListener_);
        super.onDestroy();
    }

    private void installPayload() {
        if (!ensureDeviceSupported()) {
            return;
        }
        busy_ = true;
        button_.setEnabled(false);
        button_.setText("Preparing assets...");
        PrivilegedAccess.run(this, "installer", REQUEST_SHIZUKU, this::copyAssets,
                new PrivilegedAccess.Callback() {
            @Override
            public void onComplete(String message) {
                busy_ = false;
                refreshCompatibilityState();
                ShizukuBridge.toast(InstallerActivity.this, message);
            }

            @Override
            public void onError(Throwable error) {
                busy_ = false;
                refreshCompatibilityState();
                ShizukuBridge.toast(InstallerActivity.this, error.getMessage());
            }

            @Override
            public void onPending(String message) {
                busy_ = false;
                refreshCompatibilityState();
                ShizukuBridge.toast(InstallerActivity.this, message);
            }
        });
    }

    private String copyAssets(PrivilegedClient service) throws Exception {
        List<String> files = payloadFiles();
        runChecked(service, "mkdir -p " + quote(REMOTE_ROOT) + " "
                + quote(REMOTE_LIB_DIR) + " " + quote(REMOTE_QNN_CACHE), 5000,
                "创建远端运行目录失败");
        int copied = 0;
        int skipped = 0;
        long bytes = 0L;
        for (int i = 0; i < files.size(); ++i) {
            String name = files.get(i);
            long size = assetSize(name);
            String remotePath = REMOTE_LIB_DIR + "/" + name;
            int index = i + 1;
            runOnUiThread(() -> button_.setText("Pushing " + index + "/" + files.size()));
            if (size >= 0L && service.fileSize(remotePath) == size) {
                ++skipped;
                continue;
            }
            copyAsset(service, name, remotePath, size, name.endsWith(".so"));
            ++copied;
            if (size > 0L) {
                bytes += size;
            }
        }
        runOnUiThread(() -> button_.setText("Mirroring system libraries..."));
        DependencyStats stats = mirrorDeviceLibraries(service);
        runChecked(service, "chmod 755 " + quote(REMOTE_LIB_DIR) + "/*.so", 5000,
                "设置远端运行库权限失败");
        verifyRemoteRuntime(service, files);
        return "Assets ready: copied=" + copied + ", skipped=" + skipped + ", bytes=" + bytes
                + ", systemCopied=" + stats.copied + ", systemSkipped=" + stats.skipped;
    }

    private boolean ensureDeviceSupported() {
        refreshCompatibilityState();
        if (compatibility_.supported) {
            return true;
        }
        ShizukuBridge.toast(this, compatibility_.reason);
        return false;
    }

    private void refreshCompatibilityState() {
        compatibility_ = DeviceCompatibility.current();
        deviceText_.setText(compatibility_.deviceLine());
        statusText_.setText(compatibility_.statusLine());
        if (busy_) {
            button_.setEnabled(false);
            return;
        }
        button_.setEnabled(compatibility_.supported);
        button_.setText(compatibility_.supported ? "推送运行时资源" : "设备不支持");
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

    private List<String> payloadFiles() throws IOException {
        String[] names = getAssets().list("");
        List<String> result = new ArrayList<>();
        if (names != null) {
            for (String name : names) {
                if (name.endsWith(".so") || name.endsWith(".tflite")) {
                    result.add(name);
                }
            }
        }
        result.sort(String::compareTo);
        String[] requiredAssets = {
                "libtensorflowlite_jni.so", "libQnnTFLiteDelegate.so",
                "libQnnHtp.so", "libQnnSystem.so", "model_fast256_int8.tflite"
        };
        for (String required : requiredAssets) {
            if (!result.contains(required)) {
                throw new IOException("缺少运行资源 " + required
                        + "。请按 README 将兼容的运行库和模型放入 lib/arm64-v8a/ 后重新构建安装器。");
            }
        }
        return result;
    }

    private long assetSize(String name) {
        try (AssetFileDescriptor fd = getAssets().openFd(name)) {
            return fd.getLength();
        } catch (IOException e) {
            return -1L;
        }
    }

    private void copyAsset(PrivilegedClient service, String name, String remotePath,
                           long expectedSize, boolean executable) throws Exception {
        byte[] buffer = new byte[CHUNK_SIZE];
        service.openFile(remotePath, expectedSize, executable);
        try (InputStream input = getAssets().open(name)) {
            int read;
            while ((read = input.read(buffer)) != -1) {
                service.writeChunk(read == buffer.length ? buffer : Arrays.copyOf(buffer, read), read);
            }
            service.finishFile();
        }
    }

    private DependencyStats mirrorDeviceLibraries(PrivilegedClient service) throws Exception {
        DependencyStats stats = new DependencyStats();
        for (int i = 0; i < RUNTIME_DEPENDENCIES.size(); ++i) {
            RuntimeDependency dependency = RUNTIME_DEPENDENCIES.get(i);
            int index = i + 1;
            runOnUiThread(() -> button_.setText("Mirroring " + index + "/"
                    + RUNTIME_DEPENDENCIES.size()));
            String remotePath = REMOTE_LIB_DIR + "/" + dependency.name;
            if (service.fileSize(remotePath) > 0L) {
                ++stats.skipped;
                continue;
            }
            runChecked(service, mirrorCommand(dependency, remotePath), 10000,
                    "准备设备侧依赖失败: " + dependency.name);
            if (service.fileSize(remotePath) > 0L) {
                ++stats.copied;
            } else {
                ++stats.skipped;
            }
        }
        return stats;
    }

    private String mirrorCommand(RuntimeDependency dependency, String remotePath) {
        StringBuilder command = new StringBuilder();
        command.append("found=0; copied=0; for dir in ");
        for (String sourceDir : dependency.sourceDirs) {
            command.append(quote(sourceDir)).append(' ');
        }
        command.append("; do src=\"$dir/").append(dependency.name).append("\"; ");
        command.append("if [ -f \"$src\" ]; then found=1; ");
        command.append("if cp -f \"$src\" ").append(quote(remotePath))
                .append(" && chmod 755 ").append(quote(remotePath)).append("; then ");
        command.append("copied=1; break; else echo ")
                .append(quote("copy failed: " + dependency.name))
                .append(" >&2; exit 3; fi; fi; done; ");
        command.append("if [ \"$copied\" -eq 1 ]; then exit 0; fi; ");
        if (dependency.required) {
            command.append("if [ \"$found\" -ne 1 ]; then echo ")
                    .append(quote("missing required device library " + dependency.name))
                    .append(" >&2; exit 2; fi; ");
        }
        command.append("exit 0");
        return command.toString();
    }

    private void verifyRemoteRuntime(PrivilegedClient service, List<String> assets)
            throws Exception {
        StringBuilder command = new StringBuilder("missing=0");
        for (String name : assets) {
            command.append("; [ -f ").append(quote(REMOTE_LIB_DIR + "/" + name))
                    .append(" ] || { echo ").append(quote("missing asset " + name))
                    .append("; missing=1; }");
        }
        for (RuntimeDependency dependency : RUNTIME_DEPENDENCIES) {
            if (!dependency.required) {
                continue;
            }
            command.append("; [ -f ").append(quote(REMOTE_LIB_DIR + "/" + dependency.name))
                    .append(" ] || { echo ")
                    .append(quote("missing required device library " + dependency.name))
                    .append("; missing=1; }");
        }
        command.append("; exit $missing");
        runChecked(service, command.toString(), 5000, "远端运行库校验失败");
    }

    private String runChecked(PrivilegedClient service, String command, int timeoutMs,
                              String message) throws Exception {
        String result = service.runCommand(command, timeoutMs);
        if (!commandSucceeded(result)) {
            throw new IllegalStateException(message + ": " + commandSummary(result));
        }
        return result;
    }

    private boolean commandSucceeded(String result) {
        return result != null && result.startsWith("exit=0");
    }

    private String commandSummary(String result) {
        if (result == null) {
            return "no output";
        }
        String trimmed = result.trim();
        if (trimmed.length() <= 2000) {
            return trimmed;
        }
        return trimmed.substring(0, 2000) + "...";
    }

    private static RuntimeDependency requiredDependency(String name, String[] sourceDirs) {
        return new RuntimeDependency(name, sourceDirs, true);
    }

    private static RuntimeDependency optionalDependency(String name, String[] sourceDirs) {
        return new RuntimeDependency(name, sourceDirs, false);
    }

    private static String quote(String value) {
        return "'" + value.replace("'", "'\\''") + "'";
    }

    private static final class DependencyStats {
        int copied;
        int skipped;
    }

    private static final class RuntimeDependency {
        final String name;
        final String[] sourceDirs;
        final boolean required;

        RuntimeDependency(String name, String[] sourceDirs, boolean required) {
            this.name = name;
            this.sourceDirs = sourceDirs;
            this.required = required;
        }
    }
}
