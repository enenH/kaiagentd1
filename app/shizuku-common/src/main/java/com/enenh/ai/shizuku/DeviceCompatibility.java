package com.enenh.ai.shizuku;

import android.os.Build;

import java.util.Locale;

public final class DeviceCompatibility {
    public static final int MIN_SDK = 35;
    public static final String REQUIREMENT =
            "仅支持 Android 15 及以上（API 35+）和 Qualcomm 处理器";

    private DeviceCompatibility() {
    }

    public static Result current() {
        int sdk = Build.VERSION.SDK_INT;
        boolean androidSupported = sdk >= MIN_SDK;
        String androidLabel = "Android " + Build.VERSION.RELEASE + " (API " + sdk + ")";

        String socLabel = readSocLabel();
        boolean qualcommSupported = isQualcommSoc();
        boolean supported = androidSupported && qualcommSupported;

        return new Result(
                supported,
                androidSupported,
                qualcommSupported,
                androidLabel,
                socLabel,
                reason(androidSupported, qualcommSupported));
    }

    private static String reason(boolean androidSupported, boolean qualcommSupported) {
        if (androidSupported && qualcommSupported) {
            return "设备符合要求";
        }
        if (!androidSupported && !qualcommSupported) {
            return "需要 Android 15 及以上系统，并且需要 Qualcomm 处理器";
        }
        if (!androidSupported) {
            return "需要 Android 15 及以上系统";
        }
        return "需要 Qualcomm 处理器";
    }

    private static boolean isQualcommSoc() {
        String haystack = (readSocManufacturer() + " " + readSocModel() + " "
                + safe(Build.HARDWARE) + " " + safe(Build.BOARD) + " "
                + safe(Build.DEVICE) + " " + safe(Build.PRODUCT))
                .toLowerCase(Locale.US);
        return haystack.contains("qualcomm")
                || haystack.contains("snapdragon")
                || haystack.contains("qcom")
                || haystack.contains("qti");
    }

    private static String readSocLabel() {
        String manufacturer = readSocManufacturer();
        String model = readSocModel();
        if (!manufacturer.isEmpty() && !model.isEmpty()) {
            return manufacturer + " " + model;
        }
        if (!manufacturer.isEmpty()) {
            return manufacturer;
        }
        if (!model.isEmpty()) {
            return model;
        }

        String fallback = firstNonEmpty(Build.HARDWARE, Build.BOARD, Build.DEVICE, Build.PRODUCT);
        return fallback.isEmpty() ? "未知" : fallback;
    }

    private static String readSocManufacturer() {
        if (Build.VERSION.SDK_INT >= 31) {
            return safe(Build.SOC_MANUFACTURER);
        }
        return "";
    }

    private static String readSocModel() {
        if (Build.VERSION.SDK_INT >= 31) {
            return safe(Build.SOC_MODEL);
        }
        return "";
    }

    private static String firstNonEmpty(String... values) {
        for (String value : values) {
            String safe = safe(value);
            if (!safe.isEmpty() && !"unknown".equalsIgnoreCase(safe)) {
                return safe;
            }
        }
        return "";
    }

    private static String safe(String value) {
        return value == null ? "" : value.trim();
    }

    public static final class Result {
        public final boolean supported;
        public final boolean androidSupported;
        public final boolean qualcommSupported;
        public final String androidLabel;
        public final String socLabel;
        public final String reason;

        private Result(boolean supported, boolean androidSupported, boolean qualcommSupported,
                       String androidLabel, String socLabel, String reason) {
            this.supported = supported;
            this.androidSupported = androidSupported;
            this.qualcommSupported = qualcommSupported;
            this.androidLabel = androidLabel;
            this.socLabel = socLabel;
            this.reason = reason;
        }

        public String deviceLine() {
            return "当前设备：" + androidLabel + "，SoC：" + socLabel;
        }

        public String statusLine() {
            return supported ? "兼容状态：可用" : "兼容状态：不可用 - " + reason;
        }
    }
}
