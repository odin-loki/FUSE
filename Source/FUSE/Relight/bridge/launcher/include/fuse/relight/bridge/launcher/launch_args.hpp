// FUSE Relight: launcher command-line arguments (--game, --headless, --size WxH).
// Copyright (c) 2026 FUSE contributors (AGPL-3.0).
#pragma once

#include <limits>
#include <string>
#include <string_view>

namespace fuse::relight::bridge::launcher {

struct LaunchArgs {
    std::string game;
    bool headless = false;
    int width = 1280;
    int height = 720;
    bool ok = true;
};

// argv[0] is the program name and is ignored. Defaults: empty game, windowed 1280x720, ok.
// --game <name> and --size WxH require a following value (W and H are positive integers).
// An unknown flag, or a missing value after --game or --size, sets ok to false.
[[nodiscard]] inline LaunchArgs parse_launch_args(int argc, char** argv) {
    LaunchArgs args;

    const auto parsePositive = [](std::string_view text, int& out) -> bool {
        if (text.empty()) {
            return false;
        }
        int value = 0;
        for (const char c : text) {
            if (c < '0' || c > '9') {
                return false;
            }
            const int digit = c - '0';
            if (value > (std::numeric_limits<int>::max() - digit) / 10) {
                return false;
            }
            value = value * 10 + digit;
        }
        if (value <= 0) {
            return false;
        }
        out = value;
        return true;
    };

    const auto parseSize = [&](std::string_view text, int& width, int& height) -> bool {
        const std::size_t split = text.find('x');
        if (split == std::string_view::npos || text.find('x', split + 1) != std::string_view::npos) {
            return false;
        }
        int w = 0;
        int h = 0;
        if (!parsePositive(text.substr(0, split), w) || !parsePositive(text.substr(split + 1), h)) {
            return false;
        }
        width = w;
        height = h;
        return true;
    };

    if (argv == nullptr) {
        if (argc > 1) {
            args.ok = false;
        }
        return args;
    }

    for (int i = 1; i < argc; ++i) {
        if (argv[i] == nullptr) {
            args.ok = false;
            continue;
        }
        const std::string_view token(argv[i]);
        if (token == "--game") {
            if (i + 1 >= argc || argv[i + 1] == nullptr) {
                args.ok = false;
                continue;
            }
            args.game = argv[++i];
        } else if (token == "--headless") {
            args.headless = true;
        } else if (token == "--size") {
            if (i + 1 >= argc || argv[i + 1] == nullptr) {
                args.ok = false;
                continue;
            }
            int width = 0;
            int height = 0;
            if (!parseSize(argv[++i], width, height)) {
                args.ok = false;
            } else {
                args.width = width;
                args.height = height;
            }
        } else if (!token.empty() && token.front() == '-') {
            args.ok = false;
        }
    }
    return args;
}

}  // namespace fuse::relight::bridge::launcher
