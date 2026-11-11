#include "launch_args.h"

#if !defined(NDEBUG)
#include <algorithm>
#include <charconv>
#include <limits>
#include <string_view>

namespace {

int parseFrameCount(std::string_view value) {
    int count = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), count);
    if (result.ec == std::errc::result_out_of_range) {
        return value.starts_with('-') ? 1 : std::numeric_limits<int>::max();
    }
    return std::max(1, count);
}

}  // namespace
#endif

LaunchArgs parseLaunchArgs(int argc, char* argv[]) {
    LaunchArgs args;

#if !defined(NDEBUG)
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i] != nullptr ? argv[i] : "");
        if (arg == "--capture-smoke-test") {
            args.captureSmokeTest = true;
            continue;
        }
        if (arg == "--capture-benchmark") {
            args.captureBenchmark = true;
            continue;
        }
        if (arg == "--capture-backend" && i + 1 < argc && argv[i + 1] != nullptr) {
            args.captureBackend = argv[++i];
            continue;
        }
        constexpr std::string_view backendPrefix = "--capture-backend=";
        if (arg.starts_with(backendPrefix)) {
            args.captureBackend = std::string(arg.substr(backendPrefix.size()));
            continue;
        }
        if (arg == "--capture-frames" && i + 1 < argc && argv[i + 1] != nullptr) {
            args.captureFrames = parseFrameCount(argv[++i]);
            continue;
        }
        constexpr std::string_view framesPrefix = "--capture-frames=";
        if (arg.starts_with(framesPrefix)) {
            args.captureFrames = parseFrameCount(arg.substr(framesPrefix.size()));
        }
    }
#else
    (void)argc;
    (void)argv;
#endif

    return args;
}
