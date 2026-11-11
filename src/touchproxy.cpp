#include "touchproxy.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr int kDefaultWidth = 1200;
constexpr int kDefaultHeight = 2608;
constexpr int kSyntheticPointerCount = 10;
constexpr int kSyntheticTrackingBase = 50000;
constexpr int kRealTrackingStride = 1000;
constexpr int kAimTickMs = 2;
constexpr float kAimMaxStepPx = 28.0f;
constexpr float kAimResetRadiusPx = 250.0f;
constexpr float kAimStepDeadzonePx = 0.2f;
constexpr const char* kVirtualName = "goodix_ts";

std::atomic<bool> g_running(true);

struct AbsInfo {
    int min = 0;
    int max = 0;
    int fuzz = 0;
    int flat = 0;
};

struct Point {
    int x = 0;
    int y = 0;
};

struct FloatPoint {
    float x = 0.0f;
    float y = 0.0f;
};

struct TouchDeviceInfo {
    std::string path;
    std::string name;
    std::string phys;
    std::string uniq;
    AbsInfo x;
    AbsInfo y;
    int slot_min = 0;
    int slot_max = 0;
};

struct Options {
    int screen_width = kDefaultWidth;
    int screen_height = kDefaultHeight;
    int rotation = 0;
    bool grab = true;
    std::string device_path;
};

struct SlotState {
    int raw_x = 0;
    int raw_y = 0;
    bool has_x = false;
    bool has_y = false;
    int tracking_id = -1;
};

struct SourceDevice {
    int fd = -1;
    TouchDeviceInfo info;
    int base_slot = 0;
    int slot_count = 1;
    int current_slot = 0;
    bool grabbed = false;
    std::vector<SlotState> slots;
};

struct SyntheticPointer {
    bool down = false;
    int tracking_id = -1;
    int x = 0;
    int y = 0;
};

struct AimState {
    bool active = false;
    bool command_pending = false;
    Point anchor;
    FloatPoint current;
    FloatPoint step;
};

struct TouchSnapshot {
    int x = 0;
    int y = 0;
    int dx = 0;
    int dy = 0;
    bool down = false;
    bool pressed = false;
    bool released = false;
};

bool test_bit(const unsigned long* bits, int bit) {
    return bit >= 0 &&
        ((bits[bit / (8 * static_cast<int>(sizeof(unsigned long)))] >>
          (bit % (8 * static_cast<int>(sizeof(unsigned long))))) &
         1UL) != 0;
}

template <size_t N>
bool ioctl_bits(int fd, unsigned long request, std::array<unsigned long, N>* out) {
    out->fill(0);
    return ioctl(fd, request, out->data()) >= 0;
}

template <size_t N, typename Fn>
void for_each_bit(const std::array<unsigned long, N>& bits, int max_code, Fn fn) {
    for (int code = 0; code <= max_code; ++code) {
        if (test_bit(bits.data(), code)) {
            fn(code);
        }
    }
}

bool get_abs(int fd, int code, AbsInfo* out) {
    input_absinfo info {};
    if (ioctl(fd, EVIOCGABS(code), &info) < 0) {
        return false;
    }
    out->min = info.minimum;
    out->max = info.maximum;
    out->fuzz = info.fuzz;
    out->flat = info.flat;
    return true;
}

std::string device_name(int fd) {
    char name[256] {};
    if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0) {
        return "";
    }
    return std::string(name);
}

std::string device_phys(int fd) {
    char phys[256] {};
    if (ioctl(fd, EVIOCGPHYS(sizeof(phys)), phys) < 0) {
        return "";
    }
    return std::string(phys);
}

std::string device_uniq(int fd) {
    char uniq[256] {};
    if (ioctl(fd, EVIOCGUNIQ(sizeof(uniq)), uniq) < 0) {
        return "";
    }
    return std::string(uniq);
}

std::string path_leaf(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash + 1 >= path.size()) {
        return path;
    }
    return path.substr(slash + 1);
}

std::string virtual_device_name(const TouchDeviceInfo& info) {
    return info.name.empty() ? std::string(kVirtualName) : info.name;
}

std::string virtual_device_phys(const TouchDeviceInfo& info) {
    if (!info.phys.empty()) {
        return info.phys;
    }
    return path_leaf(info.path);
}

bool is_touch_device(int fd, const std::string& path, TouchDeviceInfo* out) {
    std::array<unsigned long, 8> props {};
    std::array<unsigned long, 16> ev_bits {};
    std::array<unsigned long, 16> abs_bits {};

    ioctl_bits(fd, EVIOCGPROP(sizeof(unsigned long) * props.size()), &props);
    if (!ioctl_bits(fd, EVIOCGBIT(0, sizeof(unsigned long) * ev_bits.size()), &ev_bits)) {
        return false;
    }
    if (!test_bit(ev_bits.data(), EV_ABS)) {
        return false;
    }
    if (!ioctl_bits(fd, EVIOCGBIT(EV_ABS, sizeof(unsigned long) * abs_bits.size()), &abs_bits)) {
        return false;
    }
    if (!test_bit(props.data(), INPUT_PROP_DIRECT)) {
        return false;
    }
    const bool has_mt = test_bit(abs_bits.data(), ABS_MT_SLOT) &&
        test_bit(abs_bits.data(), ABS_MT_TRACKING_ID) &&
        test_bit(abs_bits.data(), ABS_MT_POSITION_X) &&
        test_bit(abs_bits.data(), ABS_MT_POSITION_Y);
    if (!has_mt) {
        return false;
    }

    TouchDeviceInfo info;
    info.path = path;
    info.name = device_name(fd);
    info.phys = device_phys(fd);
    info.uniq = device_uniq(fd);
    if (!get_abs(fd, ABS_MT_POSITION_X, &info.x) ||
        !get_abs(fd, ABS_MT_POSITION_Y, &info.y)) {
        return false;
    }
    AbsInfo slot {};
    if (get_abs(fd, ABS_MT_SLOT, &slot)) {
        info.slot_min = slot.min;
        info.slot_max = slot.max;
    }
    if (info.slot_max < info.slot_min) {
        info.slot_min = 0;
        info.slot_max = 0;
    }
    *out = info;
    return true;
}

std::vector<TouchDeviceInfo> scan_touch_devices() {
    std::vector<TouchDeviceInfo> devices;
    DIR* dir = opendir("/dev/input");
    if (!dir) {
        return devices;
    }
    while (dirent* entry = readdir(dir)) {
        if (strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }
        std::string path = std::string("/dev/input/") + entry->d_name;
        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        TouchDeviceInfo info;
        if (is_touch_device(fd, path, &info)) {
            devices.push_back(info);
        }
        close(fd);
    }
    closedir(dir);
    std::sort(devices.begin(), devices.end(), [](const auto& a, const auto& b) {
        return a.path < b.path;
    });
    return devices;
}


bool choose_devices(const Options& options, std::vector<TouchDeviceInfo>* out) {
    if (!options.device_path.empty()) {
        int fd = open(options.device_path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            fprintf(stderr, "ERR cannot open %s: %s\n", options.device_path.c_str(), strerror(errno));
            return false;
        }
        TouchDeviceInfo info;
        const bool ok = is_touch_device(fd, options.device_path, &info);
        close(fd);
        if (!ok) {
            fprintf(stderr, "ERR %s is not a direct multitouch device\n", options.device_path.c_str());
            return false;
        }
        out->push_back(info);
        return true;
    }

    *out = scan_touch_devices();
    if (out->empty()) {
        fprintf(stderr, "ERR no direct multitouch device found\n");
        return false;
    }
    return true;
}

int clamp_int(int value, int lo, int hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

int scale_axis(int value, const AbsInfo& src, int dst_max) {
    if (src.max <= src.min || dst_max <= 0) {
        return 0;
    }
    const int64_t offset = static_cast<int64_t>(value - src.min);
    const int64_t range = static_cast<int64_t>(src.max - src.min);
    int mapped = static_cast<int>((offset * dst_max + range / 2) / range);
    return clamp_int(mapped, 0, dst_max);
}

float clamp_aim_step(float value) {
    if (std::fabs(value) < kAimStepDeadzonePx) {
        return 0.0f;
    }
    return std::clamp(value, -kAimMaxStepPx, kAimMaxStepPx);
}

bool write_event(int fd, uint16_t type, uint16_t code, int32_t value) {
    input_event ev {};
    ev.type = type;
    ev.code = code;
    ev.value = value;
    return write(fd, &ev, sizeof(ev)) == sizeof(ev);
}

bool write_syn(int fd) {
    return write_event(fd, EV_SYN, SYN_REPORT, 0);
}

bool set_abs_bit_required(int fd, int code) {
    if (ioctl(fd, UI_SET_ABSBIT, code) < 0) {
        fprintf(stderr, "ERR UI_SET_ABSBIT %d: %s\n", code, strerror(errno));
        return false;
    }
    return true;
}

class UInputDevice {
public:
    bool create(int base_fd, const TouchDeviceInfo& base_info, int width, int height, int max_slot) {
        fd_ = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd_ < 0) {
            fprintf(stderr, "ERR open /dev/uinput: %s\n", strerror(errno));
            return false;
        }

        if (ioctl(fd_, UI_SET_EVBIT, EV_SYN) < 0 ||
            ioctl(fd_, UI_SET_EVBIT, EV_KEY) < 0 ||
            ioctl(fd_, UI_SET_EVBIT, EV_ABS) < 0 ||
            ioctl(fd_, UI_SET_PROPBIT, INPUT_PROP_DIRECT) < 0) {
            fprintf(stderr, "ERR configure uinput event bits: %s\n", strerror(errno));
            return false;
        }

        copy_key_bits(base_fd);
        if (ioctl(fd_, UI_SET_KEYBIT, BTN_TOUCH) < 0 ||
            ioctl(fd_, UI_SET_KEYBIT, BTN_TOOL_FINGER) < 0) {
            fprintf(stderr, "ERR configure uinput touch keys: %s\n", strerror(errno));
            return false;
        }

        std::array<input_absinfo, ABS_MAX + 1> abs_defaults {};
        copy_abs_bits(base_fd, &abs_defaults);
        if (!set_abs_bit_required(fd_, ABS_X) ||
            !set_abs_bit_required(fd_, ABS_Y) ||
            !set_abs_bit_required(fd_, ABS_MT_SLOT) ||
            !set_abs_bit_required(fd_, ABS_MT_TRACKING_ID) ||
            !set_abs_bit_required(fd_, ABS_MT_POSITION_X) ||
            !set_abs_bit_required(fd_, ABS_MT_POSITION_Y) ||
            !set_abs_bit_required(fd_, ABS_MT_TOUCH_MAJOR) ||
            !set_abs_bit_required(fd_, ABS_MT_TOUCH_MINOR)) {
            return false;
        }

        const std::string phys = virtual_device_phys(base_info);
        if (!phys.empty() && ioctl(fd_, UI_SET_PHYS, phys.c_str()) < 0) {
            fprintf(stderr, "WARN UI_SET_PHYS %s: %s\n", phys.c_str(), strerror(errno));
        }

        uinput_user_dev uidev {};
        const std::string name = virtual_device_name(base_info);
        snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "%s", name.c_str());
        input_id id {};
        if (ioctl(base_fd, EVIOCGID, &id) == 0) {
            uidev.id = id;
        } else {
            uidev.id.bustype = BUS_VIRTUAL;
            uidev.id.vendor = 0x18d1;
            uidev.id.product = 0x4ee7;
            uidev.id.version = 1;
        }

        for (int code = 0; code <= ABS_MAX; ++code) {
            uidev.absmin[code] = abs_defaults[code].minimum;
            uidev.absmax[code] = abs_defaults[code].maximum;
            uidev.absfuzz[code] = abs_defaults[code].fuzz;
            uidev.absflat[code] = abs_defaults[code].flat;
        }

        const int x_max = width - 1;
        const int y_max = height - 1;
        configure_abs_range(&uidev, ABS_X, 0, x_max);
        configure_abs_range(&uidev, ABS_Y, 0, y_max);
        configure_abs_range(&uidev, ABS_MT_POSITION_X, 0, x_max);
        configure_abs_range(&uidev, ABS_MT_POSITION_Y, 0, y_max);
        configure_abs_range(&uidev, ABS_MT_SLOT, 0, max_slot);
        configure_abs_range(&uidev, ABS_MT_TRACKING_ID, 0, 65535);
        configure_abs_range(&uidev, ABS_MT_TOUCH_MAJOR, 0, 255);
        configure_abs_range(&uidev, ABS_MT_TOUCH_MINOR, 0, 255);

        if (write(fd_, &uidev, sizeof(uidev)) != sizeof(uidev)) {
            fprintf(stderr, "ERR write uinput_user_dev: %s\n", strerror(errno));
            return false;
        }
        if (ioctl(fd_, UI_DEV_CREATE) < 0) {
            fprintf(stderr, "ERR UI_DEV_CREATE: %s\n", strerror(errno));
            return false;
        }
        created_ = true;
        usleep(200000);
        return true;
    }

    int fd() const {
        return fd_;
    }

    ~UInputDevice() {
        if (fd_ >= 0) {
            if (created_) {
                ioctl(fd_, UI_DEV_DESTROY);
            }
            close(fd_);
        }
    }

private:
    void copy_key_bits(int base_fd) {
        std::array<unsigned long, 16> key_bits {};
        if (!ioctl_bits(base_fd, EVIOCGBIT(EV_KEY, sizeof(unsigned long) * key_bits.size()), &key_bits)) {
            return;
        }
        for_each_bit(key_bits, KEY_MAX, [&](int code) {
            ioctl(fd_, UI_SET_KEYBIT, code);
        });
    }

    void copy_abs_bits(int base_fd, std::array<input_absinfo, ABS_MAX + 1>* defaults) {
        std::array<unsigned long, 8> abs_bits {};
        if (!ioctl_bits(base_fd, EVIOCGBIT(EV_ABS, sizeof(unsigned long) * abs_bits.size()), &abs_bits)) {
            return;
        }
        for_each_bit(abs_bits, ABS_MAX, [&](int code) {
            ioctl(fd_, UI_SET_ABSBIT, code);
            input_absinfo info {};
            if (ioctl(base_fd, EVIOCGABS(code), &info) == 0) {
                (*defaults)[code] = info;
            }
        });
    }

    static void configure_abs_range(uinput_user_dev* uidev, int code, int min, int max) {
        uidev->absmin[code] = min;
        uidev->absmax[code] = max;
        uidev->absfuzz[code] = 0;
        uidev->absflat[code] = 0;
    }

    int fd_ = -1;
    bool created_ = false;
};

class TouchProxy {
public:
    TouchProxy(const Options& options, std::vector<TouchDeviceInfo> devices)
        : options_(options), device_infos_(std::move(devices)) {
        synthetic_.resize(kSyntheticPointerCount);
        aim_.resize(kSyntheticPointerCount);
    }

    bool start() {
        if (!open_sources()) {
            return false;
        }
        const int max_slot = total_real_slots_ + kSyntheticPointerCount - 1;
        if (!uinput_.create(
                sources_.front().fd,
                sources_.front().info,
                options_.screen_width,
                options_.screen_height,
                max_slot)) {
            return false;
        }
        if (options_.grab && !set_grab_all(true, nullptr)) {
            return false;
        }
        fprintf(stdout,
                "OK touchproxy devices=%zu primary=%s virtual=\"%s\" screen=%dx%d rotation=%d slots=%d grab=%d\n",
                sources_.size(),
                sources_.front().info.path.c_str(),
                virtual_device_name(sources_.front().info).c_str(),
                options_.screen_width,
                options_.screen_height,
                options_.rotation,
                max_slot + 1,
                is_grabbed() ? 1 : 0);
        fflush(stdout);
        return true;
    }

    bool poll_once(int timeout_ms) {
        timeout_ms = std::max(0, timeout_ms);
        if (has_active_aim()) {
            timeout_ms = std::min(timeout_ms, kAimTickMs);
        }

        std::vector<pollfd> fds;
        fds.reserve(sources_.size());
        for (const auto& source : sources_) {
            fds.push_back({source.fd, POLLIN, 0});
        }

        int rc = 0;
        if (!fds.empty()) {
            rc = poll(fds.data(), fds.size(), timeout_ms);
            if (rc < 0) {
                if (errno != EINTR) {
                    fprintf(stderr, "ERR touchproxy poll: %s\n", strerror(errno));
                    g_running.store(false);
                }
                tick_aim(std::chrono::steady_clock::now());
                return g_running.load();
            }
        } else if (timeout_ms > 0) {
            usleep(static_cast<useconds_t>(timeout_ms) * 1000);
        }

        if (rc > 0) {
            for (size_t i = 0; i < sources_.size(); ++i) {
                const short revents = fds[i].revents;
                if (revents & POLLIN) {
                    handle_source(&sources_[i]);
                }
                if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    fprintf(stderr, "ERR touch source disconnected: %s\n", sources_[i].info.path.c_str());
                    g_running.store(false);
                }
            }
        }
        tick_aim(std::chrono::steady_clock::now());
        return g_running.load();
    }

    bool aim(int pointer, int anchor_x, int anchor_y, int end_x, int end_y) {
        if (pointer < 0 || pointer >= kSyntheticPointerCount) {
            return false;
        }
        anchor_x = clamp_int(anchor_x, 0, logical_width() - 1);
        anchor_y = clamp_int(anchor_y, 0, logical_height() - 1);
        end_x = clamp_int(end_x, 0, logical_width() - 1);
        end_y = clamp_int(end_y, 0, logical_height() - 1);

        AimState& aim = aim_[static_cast<size_t>(pointer)];
        const Point anchor = logical_to_device(anchor_x, anchor_y);
        const Point target = logical_to_device(end_x, end_y);
        if (!aim.active || !synthetic_[static_cast<size_t>(pointer)].down) {
            aim.current = {
                static_cast<float>(anchor.x),
                static_cast<float>(anchor.y)
            };
        }
        aim.active = true;
        aim.command_pending = true;
        aim.anchor = anchor;
        aim.step = {
            clamp_aim_step(static_cast<float>(target.x - anchor.x)),
            clamp_aim_step(static_cast<float>(target.y - anchor.y))
        };
        return true;
    }

    void stop_aim_command(int pointer) {
        if (pointer < 0) {
            for (int i = 0; i < kSyntheticPointerCount; ++i) {
                stop_aim(i, true);
            }
            return;
        }
        if (pointer < kSyntheticPointerCount) {
            stop_aim(pointer, true);
        }
    }

    KTouchState consume_touch_state() {
        KTouchState out {};
        out.x = touch_state_.x;
        out.y = touch_state_.y;
        out.dx = touch_state_.dx;
        out.dy = touch_state_.dy;
        out.down = touch_state_.down ? 1 : 0;
        out.pressed = touch_state_.pressed ? 1 : 0;
        out.released = touch_state_.released ? 1 : 0;
        touch_state_.dx = 0;
        touch_state_.dy = 0;
        touch_state_.pressed = false;
        touch_state_.released = false;
        return out;
    }


    ~TouchProxy() {
        release_all_synthetic();
        for (auto& source : sources_) {
            if (source.fd >= 0) {
                if (source.grabbed) {
                    ioctl(source.fd, EVIOCGRAB, 0);
                }
                close(source.fd);
            }
        }
    }

private:

    bool open_sources() {
        int base_slot = 0;
        for (const auto& info : device_infos_) {
            int fd = open(info.path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) {
                fprintf(stderr, "ERR open source %s: %s\n", info.path.c_str(), strerror(errno));
                return false;
            }
            SourceDevice source;
            source.fd = fd;
            source.info = info;
            source.base_slot = base_slot;
            source.slot_count = std::max(1, info.slot_max - info.slot_min + 1);
            source.current_slot = 0;
            source.slots.resize(static_cast<size_t>(source.slot_count));
            sources_.push_back(std::move(source));
            base_slot += sources_.back().slot_count;
        }
        total_real_slots_ = base_slot;
        synthetic_base_slot_ = total_real_slots_;
        return !sources_.empty();
    }

    bool set_grab_all(bool enable, std::string* error) {
        if (!enable) {
            release_real_contacts();
        }
        for (auto& source : sources_) {
            if (source.grabbed == enable) {
                continue;
            }
            if (ioctl(source.fd, EVIOCGRAB, enable ? 1 : 0) < 0) {
                if (error != nullptr) {
                    *error = std::string("EVIOCGRAB ") + source.info.path + ": " + strerror(errno);
                } else {
                    fprintf(stderr, "ERR EVIOCGRAB %s: %s\n", source.info.path.c_str(), strerror(errno));
                }
                return false;
            }
            source.grabbed = enable;
        }
        if (enable) {
            reset_real_contacts();
        }
        return true;
    }

    void release_real_contacts() {
        bool changed = false;
        for (auto& source : sources_) {
            for (int local = 0; local < source.slot_count; ++local) {
                SlotState& slot = source.slots[static_cast<size_t>(local)];
                if (slot.tracking_id < 0) {
                    continue;
                }
                write_event(uinput_.fd(), EV_ABS, ABS_MT_SLOT, virtual_slot(source, local));
                write_event(uinput_.fd(), EV_ABS, ABS_MT_TRACKING_ID, -1);
                slot.tracking_id = -1;
                changed = true;
            }
        }
        if (changed) {
            write_touch_keys();
            write_syn(uinput_.fd());
        }
        update_real_touch_state();
    }

    void reset_real_contacts() {
        for (auto& source : sources_) {
            source.current_slot = 0;
            for (auto& slot : source.slots) {
                slot = SlotState {};
            }
        }
        touch_state_.down = false;
        touch_state_.pressed = false;
        touch_state_.released = false;
        touch_state_.dx = 0;
        touch_state_.dy = 0;
    }

    bool is_grabbed() const {
        return std::any_of(sources_.begin(), sources_.end(), [](const SourceDevice& source) {
            return source.grabbed;
        });
    }

    void handle_source(SourceDevice* source) {
        input_event ev {};
        while (true) {
            ssize_t n = read(source->fd, &ev, sizeof(ev));
            if (n == sizeof(ev)) {
                forward_event(source, ev);
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;
            } else if (n == 0) {
                return;
            } else {
                fprintf(stderr, "ERR read source %s: %s\n", source->info.path.c_str(), strerror(errno));
                g_running.store(false);
                return;
            }
        }
    }

    void forward_event(SourceDevice* source, const input_event& ev) {
        if (!source->grabbed) {
            return;
        }
        uint16_t type = ev.type;
        uint16_t code = ev.code;
        int32_t value = ev.value;

        if (type == EV_ABS) {
            forward_abs(source, code, value);
        } else if (type == EV_KEY) {
            if (code == BTN_TOUCH || code == BTN_TOOL_FINGER) {
                write_touch_keys();
            } else {
                write_event(uinput_.fd(), type, code, value);
            }
        } else if (type == EV_SYN) {
            write_event(uinput_.fd(), type, code, value);
        }
    }

    void forward_abs(SourceDevice* source, uint16_t code, int32_t value) {
        switch (code) {
            case ABS_MT_SLOT: {
                source->current_slot = local_slot(*source, value);
                write_event(uinput_.fd(), EV_ABS, ABS_MT_SLOT, virtual_slot(*source, source->current_slot));
                break;
            }
            case ABS_MT_TRACKING_ID: {
                const int local = source->current_slot;
                SlotState& slot = source->slots[static_cast<size_t>(local)];
                const int mapped = value < 0 ? -1 : mapped_tracking_id(*source, local, value);
                slot.tracking_id = mapped;
                write_event(uinput_.fd(), EV_ABS, ABS_MT_SLOT, virtual_slot(*source, local));
                write_event(uinput_.fd(), EV_ABS, ABS_MT_TRACKING_ID, mapped);
                if (mapped >= 0 && slot.has_x && slot.has_y) {
                    emit_real_position(*source, local);
                }
                write_touch_keys();
                update_real_touch_state();
                break;
            }
            case ABS_MT_POSITION_X: {
                SlotState& slot = current_slot_state(source);
                slot.raw_x = value;
                slot.has_x = true;
                if (slot.has_y) {
                    emit_real_position(*source, source->current_slot);
                }
                update_real_touch_state();
                break;
            }
            case ABS_MT_POSITION_Y: {
                SlotState& slot = current_slot_state(source);
                slot.raw_y = value;
                slot.has_y = true;
                if (slot.has_x) {
                    emit_real_position(*source, source->current_slot);
                }
                update_real_touch_state();
                break;
            }
            case ABS_X:
            case ABS_Y:
                break;
            default:
                write_event(uinput_.fd(), EV_ABS, code, value);
                break;
        }
    }

    static int local_slot(const SourceDevice& source, int real_slot) {
        return clamp_int(real_slot - source.info.slot_min, 0, source.slot_count - 1);
    }

    static int virtual_slot(const SourceDevice& source, int local_slot_index) {
        return source.base_slot + local_slot_index;
    }

    int mapped_tracking_id(const SourceDevice& source, int local_slot_index, int real_tracking_id) const {
        const int slot = virtual_slot(source, local_slot_index);
        const int suffix = std::abs(real_tracking_id) % kRealTrackingStride;
        return clamp_int((slot + 1) * kRealTrackingStride + suffix, 0, kSyntheticTrackingBase - 1);
    }

    SlotState& current_slot_state(SourceDevice* source) {
        return source->slots[static_cast<size_t>(source->current_slot)];
    }

    void emit_real_position(const SourceDevice& source, int local_slot_index) {
        const SlotState& slot = source.slots[static_cast<size_t>(local_slot_index)];
        Point point = map_raw_to_device(source.info, slot.raw_x, slot.raw_y);
        write_event(uinput_.fd(), EV_ABS, ABS_MT_SLOT, virtual_slot(source, local_slot_index));
        write_event(uinput_.fd(), EV_ABS, ABS_MT_POSITION_X, point.x);
        write_event(uinput_.fd(), EV_ABS, ABS_MT_POSITION_Y, point.y);
        write_event(uinput_.fd(), EV_ABS, ABS_X, point.x);
        write_event(uinput_.fd(), EV_ABS, ABS_Y, point.y);
    }

    void update_real_touch_state() {
        bool down = false;
        Point logical { touch_state_.x, touch_state_.y };
        for (const auto& source : sources_) {
            for (const auto& slot : source.slots) {
                if (slot.tracking_id < 0 || !slot.has_x || !slot.has_y) {
                    continue;
                }
                logical = device_to_logical(map_raw_to_device(source.info, slot.raw_x, slot.raw_y));
                down = true;
                break;
            }
            if (down) {
                break;
            }
        }

        const bool was_down = touch_state_.down;
        if (down) {
            logical.x = clamp_int(logical.x, 0, logical_width() - 1);
            logical.y = clamp_int(logical.y, 0, logical_height() - 1);
            touch_state_.dx += was_down ? logical.x - touch_state_.x : 0;
            touch_state_.dy += was_down ? logical.y - touch_state_.y : 0;
            touch_state_.x = logical.x;
            touch_state_.y = logical.y;
            touch_state_.down = true;
        } else {
            touch_state_.down = false;
        }

        if (!was_down && down) {
            touch_state_.pressed = true;
        } else if (was_down && !down) {
            touch_state_.released = true;
        }
    }

    Point map_raw_to_device(const TouchDeviceInfo& info, int raw_x, int raw_y) const {
        return {
            scale_axis(raw_x, info.x, options_.screen_width - 1),
            scale_axis(raw_y, info.y, options_.screen_height - 1)
        };
    }

    Point logical_to_device(int logical_x, int logical_y) const {
        switch (options_.rotation) {
            case 1:
                return {
                    options_.screen_width - 1 - clamp_int(logical_y, 0, logical_height() - 1),
                    clamp_int(logical_x, 0, logical_width() - 1)
                };
            case 2:
                return {
                    options_.screen_width - 1 - clamp_int(logical_x, 0, logical_width() - 1),
                    options_.screen_height - 1 - clamp_int(logical_y, 0, logical_height() - 1)
                };
            case 3:
                return {
                    clamp_int(logical_y, 0, logical_height() - 1),
                    options_.screen_height - 1 - clamp_int(logical_x, 0, logical_width() - 1)
                };
            default:
                return {
                    clamp_int(logical_x, 0, logical_width() - 1),
                    clamp_int(logical_y, 0, logical_height() - 1)
                };
        }
    }

    Point device_to_logical(Point device) const {
        switch (options_.rotation) {
            case 1:
                return {
                    clamp_int(device.y, 0, logical_width() - 1),
                    clamp_int(options_.screen_width - 1 - device.x, 0, logical_height() - 1)
                };
            case 2:
                return {
                    clamp_int(options_.screen_width - 1 - device.x, 0, logical_width() - 1),
                    clamp_int(options_.screen_height - 1 - device.y, 0, logical_height() - 1)
                };
            case 3:
                return {
                    clamp_int(options_.screen_height - 1 - device.y, 0, logical_width() - 1),
                    clamp_int(device.x, 0, logical_height() - 1)
                };
            default:
                return {
                    clamp_int(device.x, 0, logical_width() - 1),
                    clamp_int(device.y, 0, logical_height() - 1)
                };
        }
    }

    int logical_width() const {
        return (options_.rotation == 1 || options_.rotation == 3)
            ? options_.screen_height
            : options_.screen_width;
    }

    int logical_height() const {
        return (options_.rotation == 1 || options_.rotation == 3)
            ? options_.screen_width
            : options_.screen_height;
    }

    void synthetic_down(int pointer, int x, int y) {
        SyntheticPointer& touch = synthetic_[static_cast<size_t>(pointer)];
        if (!touch.down) {
            touch.tracking_id = next_tracking_id_++;
            if (next_tracking_id_ > 65000) {
                next_tracking_id_ = kSyntheticTrackingBase;
            }
            touch.down = true;
            write_event(uinput_.fd(), EV_ABS, ABS_MT_SLOT, synthetic_slot(pointer));
            write_event(uinput_.fd(), EV_ABS, ABS_MT_TRACKING_ID, touch.tracking_id);
            write_event(uinput_.fd(), EV_ABS, ABS_MT_TOUCH_MAJOR, 8);
            write_event(uinput_.fd(), EV_ABS, ABS_MT_TOUCH_MINOR, 8);
        }
        touch.x = x;
        touch.y = y;
        emit_synthetic_position(pointer);
        write_touch_keys();
        write_syn(uinput_.fd());
    }

    void synthetic_move(int pointer, int x, int y) {
        SyntheticPointer& touch = synthetic_[static_cast<size_t>(pointer)];
        if (!touch.down) {
            synthetic_down(pointer, x, y);
            return;
        }
        touch.x = x;
        touch.y = y;
        emit_synthetic_position(pointer);
        write_syn(uinput_.fd());
    }

    void synthetic_up(int pointer) {
        SyntheticPointer& touch = synthetic_[static_cast<size_t>(pointer)];
        if (!touch.down) {
            return;
        }
        write_event(uinput_.fd(), EV_ABS, ABS_MT_SLOT, synthetic_slot(pointer));
        write_event(uinput_.fd(), EV_ABS, ABS_MT_TRACKING_ID, -1);
        touch.down = false;
        touch.tracking_id = -1;
        write_touch_keys();
        write_syn(uinput_.fd());
    }

    void release_all_synthetic() {
        bool changed = false;
        for (int pointer = 0; pointer < kSyntheticPointerCount; ++pointer) {
            aim_[static_cast<size_t>(pointer)] = AimState {};
            SyntheticPointer& touch = synthetic_[static_cast<size_t>(pointer)];
            if (!touch.down) {
                continue;
            }
            write_event(uinput_.fd(), EV_ABS, ABS_MT_SLOT, synthetic_slot(pointer));
            write_event(uinput_.fd(), EV_ABS, ABS_MT_TRACKING_ID, -1);
            touch.down = false;
            touch.tracking_id = -1;
            changed = true;
        }
        if (changed) {
            write_touch_keys();
            write_syn(uinput_.fd());
        }
    }

    int synthetic_slot(int pointer) const {
        return synthetic_base_slot_ + pointer;
    }

    int poll_timeout_ms() const {
        return has_active_aim() ? kAimTickMs : 250;
    }

    bool has_active_aim() const {
        return std::any_of(aim_.begin(), aim_.end(), [](const AimState& aim) {
            return aim.active;
        });
    }

    void stop_aim(int pointer, bool release_touch) {
        AimState& aim = aim_[static_cast<size_t>(pointer)];
        aim = AimState {};
        if (release_touch) {
            synthetic_up(pointer);
        }
    }

    void tick_aim(std::chrono::steady_clock::time_point now) {
        (void)now;
        for (int pointer = 0; pointer < kSyntheticPointerCount; ++pointer) {
            AimState& aim = aim_[static_cast<size_t>(pointer)];
            if (!aim.active || !aim.command_pending) {
                continue;
            }
            aim.command_pending = false;

            const bool hasStep =
                std::fabs(aim.step.x) >= kAimStepDeadzonePx ||
                std::fabs(aim.step.y) >= kAimStepDeadzonePx;
            SyntheticPointer& touch = synthetic_[static_cast<size_t>(pointer)];
            if (!touch.down && !hasStep) {
                aim = AimState {};
                continue;
            }
            if (!touch.down) {
                aim.current = {
                    static_cast<float>(aim.anchor.x),
                    static_cast<float>(aim.anchor.y)
                };
                synthetic_down(pointer, aim.anchor.x, aim.anchor.y);
            }
            if (!hasStep) {
                continue;
            }

            const float nextX = aim.current.x + aim.step.x;
            const float nextY = aim.current.y + aim.step.y;
            if (std::fabs(nextX - static_cast<float>(aim.anchor.x)) > kAimResetRadiusPx ||
                std::fabs(nextY - static_cast<float>(aim.anchor.y)) > kAimResetRadiusPx) {
                synthetic_up(pointer);
                aim.current = {
                    static_cast<float>(aim.anchor.x),
                    static_cast<float>(aim.anchor.y)
                };
                synthetic_down(pointer, aim.anchor.x, aim.anchor.y);
            }

            aim.current.x = std::clamp(aim.current.x + aim.step.x,
                                        0.0f,
                                        static_cast<float>(options_.screen_width - 1));
            aim.current.y = std::clamp(aim.current.y + aim.step.y,
                                        0.0f,
                                        static_cast<float>(options_.screen_height - 1));
            const int x = clamp_int(static_cast<int>(std::lround(aim.current.x)),
                                    0,
                                    options_.screen_width - 1);
            const int y = clamp_int(static_cast<int>(std::lround(aim.current.y)),
                                    0,
                                    options_.screen_height - 1);
            if (touch.x != x || touch.y != y) {
                synthetic_move(pointer, x, y);
            }
        }
    }

    void emit_synthetic_position(int pointer) {
        const SyntheticPointer& touch = synthetic_[static_cast<size_t>(pointer)];
        write_event(uinput_.fd(), EV_ABS, ABS_MT_SLOT, synthetic_slot(pointer));
        write_event(uinput_.fd(), EV_ABS, ABS_MT_POSITION_X, touch.x);
        write_event(uinput_.fd(), EV_ABS, ABS_MT_POSITION_Y, touch.y);
        write_event(uinput_.fd(), EV_ABS, ABS_X, touch.x);
        write_event(uinput_.fd(), EV_ABS, ABS_Y, touch.y);
    }

    bool any_real_down() const {
        for (const auto& source : sources_) {
            for (const auto& slot : source.slots) {
                if (slot.tracking_id >= 0) {
                    return true;
                }
            }
        }
        return false;
    }

    bool any_synthetic_down() const {
        return std::any_of(synthetic_.begin(), synthetic_.end(), [](const SyntheticPointer& touch) {
            return touch.down;
        });
    }

    void write_touch_keys() {
        const int value = (any_real_down() || any_synthetic_down()) ? 1 : 0;
        write_event(uinput_.fd(), EV_KEY, BTN_TOUCH, value);
        write_event(uinput_.fd(), EV_KEY, BTN_TOOL_FINGER, value);
    }


    Options options_;
    std::vector<TouchDeviceInfo> device_infos_;
    std::vector<SourceDevice> sources_;
    UInputDevice uinput_;
    int total_real_slots_ = 0;
    int synthetic_base_slot_ = 0;
    int next_tracking_id_ = kSyntheticTrackingBase;
    std::vector<SyntheticPointer> synthetic_;
    std::vector<AimState> aim_;
    TouchSnapshot touch_state_;
};

}  // namespace

struct KTouchProxy {
    Options options;
    TouchProxy* proxy = nullptr;
};

extern "C" KTouchProxy* ktouchproxy_create(
        int width, int height, int rotation, int grab) {
    if (width <= 0 || height <= 0 || rotation < 0 || rotation > 3) {
        return nullptr;
    }

    g_running.store(true);
    Options options;
    options.screen_width = width;
    options.screen_height = height;
    options.rotation = rotation;
    options.grab = grab != 0;

    std::vector<TouchDeviceInfo> devices;
    if (!choose_devices(options, &devices)) {
        return nullptr;
    }

    auto* handle = new KTouchProxy();
    handle->options = options;
    handle->proxy = new TouchProxy(options, std::move(devices));
    if (!handle->proxy->start()) {
        delete handle->proxy;
        delete handle;
        return nullptr;
    }
    return handle;
}

extern "C" void ktouchproxy_poll(KTouchProxy* handle, int timeout_ms) {
    if (!handle || !handle->proxy) {
        return;
    }
    handle->proxy->poll_once(timeout_ms);
}

extern "C" int ktouchproxy_get_touch_state(KTouchProxy* handle, KTouchState* out) {
    if (!handle || !handle->proxy || !out) {
        return 0;
    }
    *out = handle->proxy->consume_touch_state();
    return 1;
}

extern "C" int ktouchproxy_aim(
        KTouchProxy* handle,
        int pointer,
        int anchor_x,
        int anchor_y,
        int end_x,
        int end_y) {
    if (!handle || !handle->proxy) {
        return 0;
    }
    return handle->proxy->aim(pointer, anchor_x, anchor_y, end_x, end_y) ? 1 : 0;
}

extern "C" void ktouchproxy_stop_aim(KTouchProxy* handle, int pointer) {
    if (!handle || !handle->proxy) {
        return;
    }
    handle->proxy->stop_aim_command(pointer);
}

extern "C" void ktouchproxy_destroy(KTouchProxy* handle) {
    if (!handle) {
        return;
    }
    g_running.store(false);
    delete handle->proxy;
    delete handle;
}
