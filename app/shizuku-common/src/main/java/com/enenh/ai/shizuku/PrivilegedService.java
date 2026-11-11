package com.enenh.ai.shizuku;

import android.os.RemoteException;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.concurrent.TimeUnit;

public class PrivilegedService extends IPrivilegedService.Stub {
    private FileOutputStream currentStream_;
    private File currentTemp_;
    private File currentTarget_;
    private long currentExpectedSize_;
    private long currentWritten_;
    private boolean currentExecutable_;

    @Override
    public synchronized long fileSize(String path) {
        File file = new File(path);
        if (!file.isFile()) {
            return -1L;
        }
        return file.length();
    }

    @Override
    public synchronized void openFile(String path, long expectedSize, boolean executable)
            throws RemoteException {
        closeCurrentQuietly();
        try {
            currentTarget_ = new File(path);
            File parent = currentTarget_.getParentFile();
            if (parent == null) {
                throw new IOException("missing parent for " + path);
            }
            if (!parent.exists() && !parent.mkdirs()) {
                throw new IOException("cannot create " + parent);
            }
            currentTemp_ = new File(parent, currentTarget_.getName() + ".tmp." + android.os.Process.myPid());
            currentStream_ = new FileOutputStream(currentTemp_, false);
            currentExpectedSize_ = expectedSize;
            currentWritten_ = 0L;
            currentExecutable_ = executable;
        } catch (IOException e) {
            closeCurrentQuietly();
            throw remote(e);
        }
    }

    @Override
    public synchronized void writeChunk(byte[] data, int length) throws RemoteException {
        if (currentStream_ == null) {
            throw new RemoteException("no file is open");
        }
        if (data == null || length < 0 || length > data.length) {
            throw new RemoteException("invalid chunk length");
        }
        try {
            currentStream_.write(data, 0, length);
            currentWritten_ += length;
        } catch (IOException e) {
            closeCurrentQuietly();
            throw remote(e);
        }
    }

    @Override
    public synchronized void finishFile() throws RemoteException {
        if (currentStream_ == null || currentTemp_ == null || currentTarget_ == null) {
            throw new RemoteException("no file is open");
        }
        try {
            currentStream_.getFD().sync();
            currentStream_.close();
            currentStream_ = null;
            if (currentExpectedSize_ >= 0L && currentWritten_ != currentExpectedSize_) {
                throw new IOException("size mismatch for " + currentTarget_.getName()
                        + ": wrote " + currentWritten_ + ", expected " + currentExpectedSize_);
            }
            int mode = currentExecutable_ ? 0755 : 0644;
            chmod(currentTemp_, mode);
            Files.move(currentTemp_.toPath(), currentTarget_.toPath(), StandardCopyOption.REPLACE_EXISTING);
            chmod(currentTarget_, mode);
        } catch (IOException | ErrnoException e) {
            deleteQuietly(currentTemp_);
            throw remote(e);
        } finally {
            currentTemp_ = null;
            currentTarget_ = null;
            currentExpectedSize_ = -1L;
            currentWritten_ = 0L;
            currentExecutable_ = false;
        }
    }

    @Override
    public String runCommand(String command, int timeoutMs) throws RemoteException {
        Process process = null;
        try {
            process = new ProcessBuilder("/system/bin/sh", "-c", command)
                    .redirectErrorStream(true)
                    .start();
            boolean exited = process.waitFor(Math.max(timeoutMs, 1), TimeUnit.MILLISECONDS);
            if (!exited) {
                process.destroyForcibly();
                process.waitFor(200, TimeUnit.MILLISECONDS);
                String output = readAvailableLimited(process.getInputStream(), 64 * 1024);
                return "timeout\n" + output;
            }
            String output = readLimited(process.getInputStream(), 64 * 1024);
            return "exit=" + process.exitValue() + "\n" + output;
        } catch (IOException | InterruptedException e) {
            if (e instanceof InterruptedException) {
                Thread.currentThread().interrupt();
            }
            throw remote(e);
        } finally {
            if (process != null) {
                process.destroy();
            }
        }
    }

    @Override
    public String startDaemon(String command, String pidFile) throws RemoteException {
        File pid = new File(pidFile);
        File root = pid.getParentFile();
        String rootPath = root == null ? "/data/local/tmp" : root.getAbsolutePath();
        String shell = "mkdir -p " + quote(rootPath)
                + "; rm -f " + quote(pidFile)
                + "; " + command + " < /dev/null";
        String result = runCommand(shell, 5000);
        try {
            if (!waitUntilRunning(pidFile, 30, 100)) {
                return "start failed\n" + result;
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw remote(e);
        }
        return "started pid=" + readPid(pidFile) + "\n" + result;
    }

    private boolean waitUntilRunning(String pidFile, int attempts, int sleepMs)
            throws InterruptedException {
        for (int i = 0; i < attempts; ++i) {
            if (isRunning(pidFile)) {
                return true;
            }
            Thread.sleep(sleepMs);
        }
        return isRunning(pidFile);
    }

    @Override
    public boolean isRunning(String pidFile) {
        int pid = readPid(pidFile);
        if (pid <= 0) {
            return false;
        }
        try {
            Os.kill(pid, 0);
            return true;
        } catch (ErrnoException e) {
            return e.errno == OsConstants.EPERM;
        }
    }

    @Override
    public String stopDaemon(String pidFile) throws RemoteException {
        int pid = readPid(pidFile);
        if (pid <= 0) {
            deleteQuietly(new File(pidFile));
            return "not running";
        }
        try {
            Os.kill(pid, OsConstants.SIGTERM);
            for (int i = 0; i < 15; ++i) {
                if (!isRunning(pidFile)) {
                    deleteQuietly(new File(pidFile));
                    return "stopped pid=" + pid;
                }
                Thread.sleep(100);
            }
            Os.kill(pid, OsConstants.SIGKILL);
            deleteQuietly(new File(pidFile));
            return "killed pid=" + pid;
        } catch (ErrnoException | InterruptedException e) {
            if (e instanceof InterruptedException) {
                Thread.currentThread().interrupt();
            }
            throw remote(e);
        }
    }

    @Override
    public synchronized void destroy() {
        closeCurrentQuietly();
        System.exit(0);
    }

    private int readPid(String pidFile) {
        try {
            String text = new String(Files.readAllBytes(new File(pidFile).toPath()), StandardCharsets.UTF_8)
                    .trim();
            if (text.isEmpty()) {
                return -1;
            }
            return Integer.parseInt(text);
        } catch (IOException | NumberFormatException e) {
            return -1;
        }
    }

    private void closeCurrentQuietly() {
        if (currentStream_ != null) {
            try {
                currentStream_.close();
            } catch (IOException ignored) {
            }
            currentStream_ = null;
        }
        deleteQuietly(currentTemp_);
        currentTemp_ = null;
        currentTarget_ = null;
    }

    private static void chmod(File file, int mode) throws ErrnoException {
        Os.chmod(file.getAbsolutePath(), mode);
    }

    private static void deleteQuietly(File file) {
        if (file != null) {
            try {
                Files.deleteIfExists(file.toPath());
            } catch (IOException ignored) {
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

    private static String quote(String value) {
        return "'" + value.replace("'", "'\\''") + "'";
    }

    private static RemoteException remote(Exception e) {
        return new RemoteException(e.getClass().getSimpleName() + ": " + e.getMessage());
    }
}
