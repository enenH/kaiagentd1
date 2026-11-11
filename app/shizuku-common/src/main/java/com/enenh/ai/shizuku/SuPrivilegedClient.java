package com.enenh.ai.shizuku;

import android.content.Context;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.concurrent.TimeUnit;

final class SuPrivilegedClient implements PrivilegedClient {
    private static final int OUTPUT_LIMIT = 64 * 1024;
    private static final int PROBE_TIMEOUT_MS = 10000;
    private static final int FILE_COPY_TIMEOUT_MS = 30000;
    private static final int STATE_TIMEOUT_MS = 5000;

    private final Context context_;
    private FileOutputStream currentStream_;
    private File currentTemp_;
    private String currentTarget_;
    private long currentExpectedSize_;
    private long currentWritten_;
    private boolean currentExecutable_;

    private SuPrivilegedClient(Context context) {
        context_ = context.getApplicationContext();
    }

    static SuPrivilegedClient open(Context context, boolean probe) throws Exception {
        SuPrivilegedClient client = new SuPrivilegedClient(context);
        if (probe) {
            client.probeRoot();
        }
        return client;
    }

    @Override
    public synchronized long fileSize(String path) throws Exception {
        CommandResult result = runSu("if [ -f " + quote(path) + " ]; then wc -c < "
                + quote(path) + "; else echo -1; fi", STATE_TIMEOUT_MS);
        if (!result.success()) {
            return -1L;
        }
        String text = result.output.trim();
        if (text.isEmpty()) {
            return -1L;
        }
        try {
            return Long.parseLong(text.split("\\s+")[0]);
        } catch (NumberFormatException e) {
            return -1L;
        }
    }

    @Override
    public synchronized void openFile(String path, long expectedSize, boolean executable)
            throws Exception {
        closeCurrentQuietly();
        File cacheDir = context_.getCacheDir();
        if (cacheDir == null) {
            throw new IOException("missing app cache dir");
        }
        currentTemp_ = File.createTempFile("kaiagentd-", ".tmp", cacheDir);
        currentStream_ = new FileOutputStream(currentTemp_, false);
        currentTarget_ = path;
        currentExpectedSize_ = expectedSize;
        currentWritten_ = 0L;
        currentExecutable_ = executable;
    }

    @Override
    public synchronized void writeChunk(byte[] data, int length) throws Exception {
        if (currentStream_ == null) {
            throw new IOException("no file is open");
        }
        if (data == null || length < 0 || length > data.length) {
            throw new IOException("invalid chunk length");
        }
        currentStream_.write(data, 0, length);
        currentWritten_ += length;
    }

    @Override
    public synchronized void finishFile() throws Exception {
        if (currentStream_ == null || currentTemp_ == null || currentTarget_ == null) {
            throw new IOException("no file is open");
        }
        String target = currentTarget_;
        File localTemp = currentTemp_;
        try {
            currentStream_.getFD().sync();
            currentStream_.close();
            currentStream_ = null;
            if (currentExpectedSize_ >= 0L && currentWritten_ != currentExpectedSize_) {
                throw new IOException("size mismatch for " + pathLeaf(target)
                        + ": wrote " + currentWritten_ + ", expected " + currentExpectedSize_);
            }

            String parent = parentPath(target);
            String remoteTemp = target + ".tmp." + android.os.Process.myPid();
            String mode = currentExecutable_ ? "0755" : "0644";
            String command = "mkdir -p " + quote(parent)
                    + " && rm -f " + quote(remoteTemp)
                    + " && cp " + quote(localTemp.getAbsolutePath()) + " " + quote(remoteTemp)
                    + " && chmod " + mode + " " + quote(remoteTemp)
                    + " && mv -f " + quote(remoteTemp) + " " + quote(target)
                    + " && chmod " + mode + " " + quote(target);
            CommandResult copy = runSu(command, FILE_COPY_TIMEOUT_MS);
            if (!copy.success()) {
                throw new IOException("root copy failed: " + copy.summary());
            }
            if (currentExpectedSize_ >= 0L) {
                long actualSize = fileSize(target);
                if (actualSize != currentExpectedSize_) {
                    throw new IOException("size mismatch for " + pathLeaf(target)
                            + ": copied " + actualSize + ", expected " + currentExpectedSize_);
                }
            }
        } finally {
            closeCurrentQuietly();
            Files.deleteIfExists(localTemp.toPath());
        }
    }

    @Override
    public String runCommand(String command, int timeoutMs) throws Exception {
        CommandResult result = runSu(command, timeoutMs);
        if (result.timedOut) {
            return "timeout\n" + result.output;
        }
        return "exit=" + result.exitCode + "\n" + result.output;
    }

    @Override
    public String startDaemon(String command, String pidFile) throws Exception {
        String rootPath = parentPath(pidFile);
        String shell = "mkdir -p " + quote(rootPath)
                + "; rm -f " + quote(pidFile)
                + "; " + command + " < /dev/null";
        String result = runCommand(shell, 5000);
        if (!waitUntilRunning(pidFile, 30, 100)) {
            return "start failed\n" + result;
        }
        return "started pid=" + readPid(pidFile) + "\n" + result;
    }

    @Override
    public boolean isRunning(String pidFile) throws Exception {
        String command = "pid=$(cat " + quote(pidFile) + " 2>/dev/null); "
                + "case \"$pid\" in ''|*[!0-9]*) exit 1;; esac; "
                + "kill -0 \"$pid\" 2>/dev/null";
        return runSu(command, STATE_TIMEOUT_MS).success();
    }

    @Override
    public String stopDaemon(String pidFile) throws Exception {
        String command = "pid=$(cat " + quote(pidFile) + " 2>/dev/null); "
                + "case \"$pid\" in ''|*[!0-9]*) rm -f " + quote(pidFile)
                + "; echo not running; exit 0;; esac; "
                + "kill -TERM \"$pid\" 2>/dev/null || true; "
                + "for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do "
                + "if ! kill -0 \"$pid\" 2>/dev/null; then rm -f " + quote(pidFile)
                + "; echo stopped pid=$pid; exit 0; fi; sleep 0.1; done; "
                + "kill -KILL \"$pid\" 2>/dev/null || true; rm -f " + quote(pidFile)
                + "; echo killed pid=$pid";
        CommandResult result = runSu(command, 5000);
        if (!result.success()) {
            throw new IOException("stop failed: " + result.summary());
        }
        return result.output.trim().isEmpty() ? "stopped" : result.output.trim();
    }

    @Override
    public synchronized void close() {
        closeCurrentQuietly();
    }

    private void probeRoot() throws Exception {
        CommandResult result = runSu("id", PROBE_TIMEOUT_MS);
        if (!result.success() || !result.output.contains("uid=0")) {
            throw new IOException("su root not granted: " + result.summary());
        }
    }

    private boolean waitUntilRunning(String pidFile, int attempts, int sleepMs)
            throws Exception {
        for (int i = 0; i < attempts; ++i) {
            if (isRunning(pidFile)) {
                return true;
            }
            Thread.sleep(sleepMs);
        }
        return isRunning(pidFile);
    }

    private int readPid(String pidFile) throws Exception {
        CommandResult result = runSu("cat " + quote(pidFile) + " 2>/dev/null", STATE_TIMEOUT_MS);
        if (!result.success()) {
            return -1;
        }
        String text = result.output.trim();
        if (text.isEmpty()) {
            return -1;
        }
        try {
            return Integer.parseInt(text.split("\\s+")[0]);
        } catch (NumberFormatException e) {
            return -1;
        }
    }

    private synchronized void closeCurrentQuietly() {
        if (currentStream_ != null) {
            try {
                currentStream_.close();
            } catch (IOException ignored) {
            }
            currentStream_ = null;
        }
        if (currentTemp_ != null) {
            try {
                Files.deleteIfExists(currentTemp_.toPath());
            } catch (IOException ignored) {
            }
        }
        currentTemp_ = null;
        currentTarget_ = null;
        currentExpectedSize_ = -1L;
        currentWritten_ = 0L;
        currentExecutable_ = false;
    }

    private static CommandResult runSu(String command, int timeoutMs)
            throws IOException, InterruptedException {
        java.lang.Process process = null;
        try {
            process = new ProcessBuilder("su", "-c", command)
                    .redirectErrorStream(true)
                    .start();
            boolean exited = process.waitFor(Math.max(timeoutMs, 1), TimeUnit.MILLISECONDS);
            if (!exited) {
                process.destroyForcibly();
                process.waitFor(200, TimeUnit.MILLISECONDS);
                String output = readAvailableLimited(process.getInputStream(), OUTPUT_LIMIT);
                return new CommandResult(-1, true, output);
            }
            String output = readLimited(process.getInputStream(), OUTPUT_LIMIT);
            return new CommandResult(process.exitValue(), false, output);
        } finally {
            if (process != null) {
                process.destroy();
            }
        }
    }

    private static String readLimited(InputStream input, int limit) throws IOException {
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        byte[] buffer = new byte[4096];
        int total = 0;
        int read;
        while ((read = input.read(buffer)) != -1) {
            int keep = Math.min(read, limit - total);
            if (keep > 0) {
                output.write(buffer, 0, keep);
                total += keep;
            }
            if (total >= limit) {
                break;
            }
        }
        return output.toString(StandardCharsets.UTF_8.name());
    }

    private static String readAvailableLimited(InputStream input, int limit) throws IOException {
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        byte[] buffer = new byte[4096];
        int total = 0;
        while (total < limit) {
            int available = input.available();
            if (available <= 0) {
                break;
            }
            int read = input.read(buffer, 0,
                    Math.min(buffer.length, Math.min(available, limit - total)));
            if (read <= 0) {
                break;
            }
            output.write(buffer, 0, read);
            total += read;
        }
        return output.toString(StandardCharsets.UTF_8.name());
    }

    private static String parentPath(String path) {
        int index = path.lastIndexOf('/');
        if (index <= 0) {
            return "/data/local/tmp";
        }
        return path.substring(0, index);
    }

    private static String pathLeaf(String path) {
        int index = path.lastIndexOf('/');
        return index >= 0 ? path.substring(index + 1) : path;
    }

    private static String quote(String value) {
        return "'" + value.replace("'", "'\\''") + "'";
    }

    private static final class CommandResult {
        final int exitCode;
        final boolean timedOut;
        final String output;

        CommandResult(int exitCode, boolean timedOut, String output) {
            this.exitCode = exitCode;
            this.timedOut = timedOut;
            this.output = output == null ? "" : output;
        }

        boolean success() {
            return !timedOut && exitCode == 0;
        }

        String summary() {
            if (timedOut) {
                return "timeout " + output.trim();
            }
            return "exit=" + exitCode + " " + output.trim();
        }
    }
}
