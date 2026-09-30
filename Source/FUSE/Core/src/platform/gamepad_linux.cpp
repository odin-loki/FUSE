// UNI-INPUT-1: Linux evdev gamepad backend.
//
// Pads are /dev/input/event* nodes that report gamepad buttons (BTN_SOUTH, the kernel's
// BTN_GAMEPAD) or a joystick (a BTN_JOYSTICK-range key plus ABS_X). Each open node takes one of
// kMaxGamepads slots. inotify on the input directory triggers a rescan when nodes appear or
// their permissions change (udev sets the ACL just after creation); a read error (ENODEV) or
// EOF closes the slot and reports Disconnected. After SYN_DROPPED the event stream is ignored
// up to the next SYN_REPORT and the state is re-read with EVIOCGABS / EVIOCGKEY.
//
// Rumble uses the force-feedback API (FF_RUMBLE via EVIOCSFF + an EV_FF play event) when the
// node could be opened read-write.

#include <fuse/platform/gamepad.hpp>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>

#include <fcntl.h>
#include <linux/input.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace fuse::platform {

namespace {

constexpr usize kLongBits = sizeof(unsigned long) * 8;

template <usize Bits>
struct BitSet {
    unsigned long words[(Bits + kLongBits - 1) / kLongBits]{};
    [[nodiscard]] bool test(usize bit) const {
        return bit < Bits && ((words[bit / kLongBits] >> (bit % kLongBits)) & 1ul) != 0;
    }
};

bool isGamepadNode(int fd) {
    BitSet<KEY_CNT> keys;
    BitSet<ABS_CNT> abs;
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys.words)), keys.words) < 0) {
        return false;
    }
    (void)ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs.words)), abs.words);
    if (keys.test(BTN_GAMEPAD)) {
        return true;
    }
    for (usize k = BTN_JOYSTICK; k < BTN_GAMEPAD; ++k) {
        if (keys.test(k) && abs.test(ABS_X)) {
            return true;
        }
    }
    return false;
}

} // namespace

EvdevGamepadBackend::EvdevGamepadBackend(std::string inputDir, bool watchHotplug) : m_dir(std::move(inputDir)) {
    if (watchHotplug && !m_dir.empty()) {
        m_inotify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (m_inotify >= 0 &&
            inotify_add_watch(m_inotify, m_dir.c_str(), IN_CREATE | IN_ATTRIB | IN_MOVED_TO) < 0) {
            close(m_inotify);
            m_inotify = -1;
        }
    }
}

EvdevGamepadBackend::~EvdevGamepadBackend() {
    for (Device& d : m_devices) {
        if (d.fd >= 0) {
            close(d.fd);
        }
    }
    if (m_inotify >= 0) {
        close(m_inotify);
    }
}

u32 EvdevGamepadBackend::deviceCount() const {
    u32 n = 0;
    for (const Device& d : m_devices) {
        n += d.fd >= 0 ? 1u : 0u;
    }
    return n;
}

std::string EvdevGamepadBackend::deviceName(u32 pad) const {
    return pad < kMaxGamepads && m_devices[pad].fd >= 0 ? m_devices[pad].name : std::string();
}

int EvdevGamepadBackend::adoptStream(int fd, const EvdevDeviceCaps& caps) {
    for (u32 i = 0; i < kMaxGamepads; ++i) {
        Device& d = m_devices[i];
        if (d.fd >= 0) {
            continue;
        }
        d = Device{};
        d.fd = fd;
        d.stream = true;
        d.name = caps.name;
        d.path = "stream:" + std::to_string(fd);
        for (const EvdevDeviceCaps::Abs& a : caps.absAxes) {
            d.mapper.setAbsRange(a.code, a.minimum, a.maximum);
        }
        return static_cast<int>(i);
    }
    return -1;
}

void EvdevGamepadBackend::rescan(std::vector<GamepadEvent>& out) {
    m_scanPending = false;
    if (m_dir.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::directory_iterator it(m_dir, ec);
    if (ec) {
        return;
    }
    for (const std::filesystem::directory_entry& entry : it) {
        const std::string file = entry.path().filename().string();
        if (file.rfind("event", 0) != 0) {
            continue;
        }
        const std::string path = entry.path().string();
        bool open = false;
        for (const Device& d : m_devices) {
            open = open || (d.fd >= 0 && d.path == path);
        }
        if (!open) {
            (void)openDevice(path, out);
        }
    }
}

bool EvdevGamepadBackend::openDevice(const std::string& path, std::vector<GamepadEvent>& out) {
    u32 slot = kMaxGamepads;
    for (u32 i = 0; i < kMaxGamepads; ++i) {
        if (m_devices[i].fd < 0) {
            slot = i;
            break;
        }
    }
    if (slot == kMaxGamepads) {
        return false;
    }
    int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    }
    if (fd < 0) {
        return false; // EACCES until udev grants access: IN_ATTRIB triggers another scan
    }
    if (!isGamepadNode(fd)) {
        close(fd);
        return false;
    }
    Device& d = m_devices[slot];
    d = Device{};
    d.fd = fd;
    d.path = path;
    char name[256] = {};
    if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0) {
        d.name = name;
    }
    BitSet<ABS_CNT> abs;
    (void)ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs.words)), abs.words);
    for (u16 code = 0; code < evdev_codes::AbsCount; ++code) {
        if (!abs.test(code)) {
            continue;
        }
        input_absinfo info{};
        if (ioctl(fd, EVIOCGABS(code), &info) >= 0) {
            d.mapper.setAbsRange(code, info.minimum, info.maximum);
        }
    }
    d.announced = true;
    out.push_back(GamepadEvent::connected(static_cast<u8>(slot)));
    resync(static_cast<u8>(slot), out);
    return true;
}

void EvdevGamepadBackend::resync(u8 pad, std::vector<GamepadEvent>& out) {
    Device& d = m_devices[pad];
    if (d.fd < 0 || d.stream) {
        return;
    }
    d.mapper.resetHats();
    // Release every mapped button first; the key-state read re-presses the held ones.
    // GamepadState ignores releases of buttons that are not down.
    for (u32 b = 0; b < static_cast<u32>(GamepadButton::COUNT); ++b) {
        out.push_back(GamepadEvent::button(pad, static_cast<GamepadButton>(b), false));
    }
    for (u16 code = 0; code < evdev_codes::AbsCount; ++code) {
        if (!d.mapper.hasAbs(code)) {
            continue;
        }
        input_absinfo info{};
        if (ioctl(d.fd, EVIOCGABS(code), &info) >= 0) {
            (void)d.mapper.translate(pad, EV_ABS, code, info.value, out);
        }
    }
    BitSet<KEY_CNT> keys;
    if (ioctl(d.fd, EVIOCGKEY(sizeof(keys.words)), keys.words) >= 0) {
        for (u16 code = BTN_JOYSTICK; code < KEY_CNT; ++code) {
            if (keys.test(code)) {
                (void)d.mapper.translate(pad, EV_KEY, code, 1, out);
            }
        }
    }
}

void EvdevGamepadBackend::closeDevice(u8 pad, std::vector<GamepadEvent>& out) {
    Device& d = m_devices[pad];
    if (d.fd >= 0) {
        close(d.fd);
    }
    if (d.announced) {
        out.push_back(GamepadEvent::disconnected(pad));
    }
    d = Device{};
}

u32 EvdevGamepadBackend::poll(std::vector<GamepadEvent>& out) {
    const usize before = out.size();
    if (m_inotify >= 0) {
        alignas(inotify_event) char buf[4096];
        for (;;) {
            const ssize_t n = read(m_inotify, buf, sizeof(buf));
            if (n <= 0) {
                break;
            }
            m_scanPending = true;
        }
    }
    if (m_scanPending) {
        rescan(out);
    }
    for (u32 i = 0; i < kMaxGamepads; ++i) {
        Device& d = m_devices[i];
        if (d.fd < 0) {
            continue;
        }
        const u8 pad = static_cast<u8>(i);
        if (!d.announced) {
            d.announced = true;
            out.push_back(GamepadEvent::connected(pad));
        }
        input_event events[64];
        for (;;) {
            const ssize_t n = read(d.fd, events, sizeof(events));
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    closeDevice(pad, out); // ENODEV: unplugged
                }
                break;
            }
            if (n == 0) {
                closeDevice(pad, out); // EOF (adopted stream closed)
                break;
            }
            const usize count = static_cast<usize>(n) / sizeof(input_event);
            for (usize e = 0; e < count; ++e) {
                const input_event& ev = events[e];
                if (ev.type == EV_SYN) {
                    if (ev.code == SYN_DROPPED) {
                        d.dropped = true;
                    } else if (ev.code == SYN_REPORT && d.dropped) {
                        d.dropped = false;
                        resync(pad, out);
                    }
                    continue;
                }
                if (!d.dropped) {
                    (void)d.mapper.translate(pad, ev.type, ev.code, ev.value, out);
                }
            }
            if (static_cast<usize>(n) < sizeof(events)) {
                break;
            }
        }
    }
    return static_cast<u32>(out.size() - before);
}

bool EvdevGamepadBackend::setRumble(u32 pad, f32 lowFrequency, f32 highFrequency) {
    if (pad >= kMaxGamepads) {
        return false;
    }
    Device& d = m_devices[pad];
    if (d.fd < 0 || d.stream) {
        return false;
    }
    const auto magnitude = [](f32 v) {
        const f32 c = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
        return static_cast<u16>(c * 65535.f);
    };
    input_event play{};
    play.type = EV_FF;
    if (lowFrequency <= 0.f && highFrequency <= 0.f) {
        if (d.rumbleEffect < 0) {
            return true;
        }
        play.code = static_cast<u16>(d.rumbleEffect);
        play.value = 0;
        return write(d.fd, &play, sizeof(play)) == static_cast<ssize_t>(sizeof(play));
    }
    ff_effect effect{};
    effect.type = FF_RUMBLE;
    effect.id = static_cast<std::int16_t>(d.rumbleEffect); // -1 uploads a new effect
    effect.u.rumble.strong_magnitude = magnitude(lowFrequency);
    effect.u.rumble.weak_magnitude = magnitude(highFrequency);
    effect.replay.length = 5000; // ms; games refresh or stop it
    if (ioctl(d.fd, EVIOCSFF, &effect) < 0) {
        return false;
    }
    d.rumbleEffect = effect.id;
    play.code = static_cast<u16>(effect.id);
    play.value = 1;
    return write(d.fd, &play, sizeof(play)) == static_cast<ssize_t>(sizeof(play));
}

std::unique_ptr<GamepadBackend> createPlatformGamepadBackend() {
    return std::make_unique<EvdevGamepadBackend>();
}

} // namespace fuse::platform
