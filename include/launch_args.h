#ifndef KAIAGENTD_LAUNCH_ARGS_H
#define KAIAGENTD_LAUNCH_ARGS_H

#include <string>

struct LaunchArgs {
    std::string captureBackend = "cpuconsumer";
    int captureFrames = 120;
    bool captureSmokeTest = false;
    bool captureBenchmark = false;
};

LaunchArgs parseLaunchArgs(int argc, char* argv[]);

#endif  // KAIAGENTD_LAUNCH_ARGS_H
