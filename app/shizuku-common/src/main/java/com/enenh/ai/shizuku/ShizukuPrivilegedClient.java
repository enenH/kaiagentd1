package com.enenh.ai.shizuku;

final class ShizukuPrivilegedClient implements PrivilegedClient {
    private final IPrivilegedService service_;

    ShizukuPrivilegedClient(IPrivilegedService service) {
        service_ = service;
    }

    @Override
    public long fileSize(String path) throws Exception {
        return service_.fileSize(path);
    }

    @Override
    public void openFile(String path, long expectedSize, boolean executable) throws Exception {
        service_.openFile(path, expectedSize, executable);
    }

    @Override
    public void writeChunk(byte[] data, int length) throws Exception {
        service_.writeChunk(data, length);
    }

    @Override
    public void finishFile() throws Exception {
        service_.finishFile();
    }

    @Override
    public String runCommand(String command, int timeoutMs) throws Exception {
        return service_.runCommand(command, timeoutMs);
    }

    @Override
    public String startDaemon(String command, String pidFile) throws Exception {
        return service_.startDaemon(command, pidFile);
    }

    @Override
    public boolean isRunning(String pidFile) throws Exception {
        return service_.isRunning(pidFile);
    }

    @Override
    public String stopDaemon(String pidFile) throws Exception {
        return service_.stopDaemon(pidFile);
    }

    @Override
    public void close() {
    }
}
