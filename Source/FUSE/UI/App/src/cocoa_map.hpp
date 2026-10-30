// Pure-C++ tables shared by the Cocoa backend (cocoa.mm). Kept free of Objective-C so they are unit-tested on every
// host (tests/test_cocoa_map.cpp) even though cocoa.mm itself only builds on Apple platforms.
#pragma once
#include "fuse/ui/ui.hpp"
#include <cstdint>

namespace fuse::app::cocoa {

/// macOS virtual key code (NSEvent.keyCode, Carbon kVK_*) -> fuse Key.
inline fuse::ui::Key keyFromVk(unsigned short vk) {
    using K = fuse::ui::Key;
    switch (vk) {
    case 0x00: return K::A; case 0x0B: return K::B; case 0x08: return K::C; case 0x02: return K::D; case 0x0E: return K::E;
    case 0x03: return K::F; case 0x05: return K::G; case 0x04: return K::H; case 0x22: return K::I; case 0x26: return K::J;
    case 0x28: return K::K; case 0x25: return K::L; case 0x2E: return K::M; case 0x2D: return K::N; case 0x1F: return K::O;
    case 0x23: return K::P; case 0x0C: return K::Q; case 0x0F: return K::R; case 0x01: return K::S; case 0x11: return K::T;
    case 0x20: return K::U; case 0x09: return K::V; case 0x0D: return K::W; case 0x07: return K::X; case 0x10: return K::Y;
    case 0x06: return K::Z;
    case 0x1D: return K::Num0; case 0x12: return K::Num1; case 0x13: return K::Num2; case 0x14: return K::Num3;
    case 0x15: return K::Num4; case 0x17: return K::Num5; case 0x16: return K::Num6; case 0x1A: return K::Num7;
    case 0x1C: return K::Num8; case 0x19: return K::Num9;
    case 0x30: return K::Tab; case 0x24: case 0x4C: return K::Enter; case 0x35: return K::Escape; case 0x31: return K::Space;
    case 0x33: return K::Backspace; case 0x75: return K::Delete;
    case 0x7B: return K::Left; case 0x7C: return K::Right; case 0x7E: return K::Up; case 0x7D: return K::Down;
    case 0x73: return K::Home; case 0x77: return K::End; case 0x74: return K::PageUp; case 0x79: return K::PageDown;
    case 0x21: return K::BracketLeft; case 0x1E: return K::BracketRight;
    case 0x7A: return K::F1; case 0x78: return K::F2; case 0x63: return K::F3; case 0x76: return K::F4; case 0x60: return K::F5;
    case 0x61: return K::F6; case 0x62: return K::F7; case 0x64: return K::F8; case 0x65: return K::F9; case 0x6D: return K::F10;
    case 0x67: return K::F11; case 0x6F: return K::F12;
    default: return K::Other;
    }
}

/// NSEventModifierFlags -> fuse Mods. Command maps to Super; callers wanting Cmd-as-Ctrl shortcuts swap it.
inline std::uint8_t modsFromFlags(unsigned long flags) {
    std::uint8_t m = 0;
    if (flags & (1ul << 17)) m |= fuse::ui::kModShift;   // NSEventModifierFlagShift
    if (flags & (1ul << 18)) m |= fuse::ui::kModCtrl;    // Control
    if (flags & (1ul << 19)) m |= fuse::ui::kModAlt;     // Option
    if (flags & (1ul << 20)) m |= fuse::ui::kModSuper;   // Command
    return m;
}

/// Cocoa views are bottom-left origin; fuse is top-left.
inline float flipY(float y, float viewHeight) { return viewHeight - y; }

/// A11yRole -> NSAccessibilityRole string constant value (the constants' documented string values).
inline const char* nsRole(fuse::ui::A11yRole r) {
    using R = fuse::ui::A11yRole;
    switch (r) {
    case R::Window: return "AXWindow";
    case R::Group: return "AXGroup";
    case R::Text: return "AXStaticText";
    case R::Button: return "AXButton";
    case R::CheckBox: case R::Switch: return "AXCheckBox";
    case R::RadioButton: return "AXRadioButton";
    case R::Slider: case R::Dial: return "AXSlider";
    case R::ProgressBar: return "AXProgressIndicator";
    case R::TextField: return "AXTextField";
    case R::ScrollArea: return "AXScrollArea";
    case R::List: return "AXList";
    case R::ListItem: return "AXCell";
    case R::Image: return "AXImage";
    case R::Separator: return "AXSplitter";
    case R::Link: return "AXLink";
    case R::ComboBox: return "AXComboBox";
    case R::SpinBox: return "AXIncrementor";
    case R::ScrollBar: return "AXScrollBar";
    case R::Tab: return "AXRadioButton"; // NSAccessibilityTabGroup children are radio buttons
    case R::TabList: return "AXTabGroup";
    default: return "AXUnknown";
    }
}
/// AXSubrole for roles that need one.
inline const char* nsSubrole(fuse::ui::A11yRole r) {
    using R = fuse::ui::A11yRole;
    return r == R::Switch ? "AXSwitch" : r == R::Tab ? "AXTabButton" : nullptr;
}

} // namespace fuse::app::cocoa
