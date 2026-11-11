package com.enenh.ai.shizuku;

interface IPrivilegedService {
    long fileSize(String path) = 1;
    void openFile(String path, long expectedSize, boolean executable) = 2;
    void writeChunk(in byte[] data, int length) = 3;
    void finishFile() = 4;
    String runCommand(String command, int timeoutMs) = 5;
    String startDaemon(String command, String pidFile) = 6;
    boolean isRunning(String pidFile) = 7;
    String stopDaemon(String pidFile) = 8;
    void destroy() = 16777114;
}
