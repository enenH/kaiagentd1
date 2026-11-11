package com.enenh.ai.shizuku;

public interface PrivilegedClient extends AutoCloseable {
    long fileSize(String path) throws Exception;

    void openFile(String path, long expectedSize, boolean executable) throws Exception;

    void writeChunk(byte[] data, int length) throws Exception;

    void finishFile() throws Exception;

    String runCommand(String command, int timeoutMs) throws Exception;

    String startDaemon(String command, String pidFile) throws Exception;

    boolean isRunning(String pidFile) throws Exception;

    String stopDaemon(String pidFile) throws Exception;

    @Override
    void close();
}
