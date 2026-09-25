// FUSE Relight RL-6.1: the developer menu and the overlay state machine (see dev_menu.hpp).
#include <fuse/relight/overlay/dev_menu.hpp>

#include <fuse/relight/overlay/font5x7.hpp>
#include <fuse/relight/overlay/raster.hpp>

#include <fuse/relight/options/option.hpp>
#include <fuse/relight/options/option_config.hpp>
#include <fuse/relight/options/option_layer.hpp>
#include <fuse/relight/options/option_manager.hpp>
#include <fuse/relight/options/option_value.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cinttypes>
#include <cstdio>
#include <variant>

namespace fuse::relight::overlay {

namespace opt = fuse::relight::options;

const char* debugViewName(DebugView view) {
    switch (view) {
    case DebugView::Albedo:
        return "albedo";
    case DebugView::Normal:
        return "normal";
    case DebugView::Depth:
        return "depth";
    case DebugView::Motion:
        return "motion";
    case DebugView::DiffuseDemod:
        return "diffuse";
    case DebugView::SpecularDemod:
        return "specular";
    default:
        return "off";
    }
}

const char* tabName(Tab tab) {
    switch (tab) {
    case Tab::Options:
        return "opts";
    case Tab::Textures:
        return "tex";
    case Tab::Debug:
        return "dbg";
    case Tab::Capture:
        return "cap";
    default:
        return "stats";
    }
}

namespace {

bool containsNoCase(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    if (needle.size() > haystack.size()) {
        return false;
    }
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size() && match; ++j) {
            match = std::tolower(static_cast<unsigned char>(haystack[i + j])) ==
                    std::tolower(static_cast<unsigned char>(needle[j]));
        }
        if (match) {
            return true;
        }
    }
    return false;
}

opt::Option<opt::HashSet>* hashList(const char* name) {
    return dynamic_cast<opt::Option<opt::HashSet>*>(opt::OptionManager::findOption(name));
}

/// Scroll position after paging keys / wheel over a list of `count` rows showing `page`.
std::int32_t scrolled(std::int32_t scroll, std::int32_t key, std::int32_t wheel, std::int32_t page,
                      std::int32_t count) {
    if (key == kVkNext) {
        scroll += std::max(1, page);
    } else if (key == kVkPrior) {
        scroll -= std::max(1, page);
    } else if (key == kVkDown) {
        scroll += 1;
    } else if (key == kVkUp) {
        scroll -= 1;
    } else if (key == kVkHome) {
        scroll = 0;
    }
    scroll -= wheel;
    return std::clamp(scroll, 0, std::max(0, count - std::max(1, page)));
}

} // namespace

// ---- actions ---------------------------------------------------------------------------------------------------

bool DevMenu::editOption(opt::OptionBase& option, const std::string& value) {
    const opt::OptionLayerTarget target(opt::OptionEditTarget::User);
    const opt::OptionLayer* layer = option.getTargetLayer();
    if (!layer) {
        return false;
    }
    opt::OptionConfig config;
    config.set(option.getFullName(), value);
    option.readOption(config, layer);
    layer->onLayerValueChanged();
    opt::OptionManager::applyPendingValues(nullptr, false);
    return true;
}

bool DevMenu::textureInCategory(const TextureCategory& category, std::uint64_t hash) {
    const opt::Option<opt::HashSet>* list = hashList(category.option);
    return list && list->containsHash(hash);
}

bool DevMenu::toggleTextureCategory(const TextureCategory& category, std::uint64_t hash) {
    opt::Option<opt::HashSet>* list = hashList(category.option);
    const opt::OptionLayer* layer = opt::OptionLayer::getRtxConfLayer();
    if (!list || !layer) {
        return false;
    }
    if (list->containsHash(hash)) {
        list->clearHash(hash, layer);
        opt::OptionManager::applyPendingValues(nullptr, false);
        if (list->containsHash(hash)) {
            list->removeHash(hash, layer); // a weaker layer lists it: an explicit removal in rtx.conf
        }
    } else {
        list->addHash(hash, layer);
    }
    opt::OptionManager::applyPendingValues(nullptr, false);
    return true;
}

bool DevMenu::saveLayers(std::string* what) {
    bool ok = true;
    std::string saved;
    if (opt::OptionLayer* rtx = opt::OptionLayer::getRtxConfLayer(); rtx && rtx->hasSaveableConfigFile()) {
        if (rtx->save()) {
            saved += rtx->getFilePath();
        } else {
            ok = false;
        }
    }
    if (const opt::OptionLayer* userConst = opt::OptionLayer::getUserLayer()) {
        opt::OptionLayer* user = opt::OptionManager::getLayer(userConst->getLayerKey());
        if (user && user->hasSaveableConfigFile() && user->hasUnsavedChanges()) {
            if (user->save()) {
                saved += (saved.empty() ? "" : " ") + user->getFilePath();
            } else {
                ok = false;
            }
        }
    }
    if (what) {
        *what = saved;
    }
    return ok && !saved.empty();
}

// ---- the menu --------------------------------------------------------------------------------------------------

DevMenu::DevMenu() {
    m_filter.reserve(64);
    m_status.reserve(128);
}

void DevMenu::setViewport(std::uint32_t frameW, std::uint32_t frameH, std::int32_t scaleOption) {
    std::int32_t s = scaleOption;
    if (s <= 0) {
        s = frameH < 720u ? 1 : (frameH < 1440u ? 2 : 3);
    }
    m_scale = std::clamp(s, 1, 8);
    const std::int32_t margin = 4 * m_scale;
    const std::int32_t w = std::min(static_cast<std::int32_t>(frameW) - 2 * margin, (64 * kCellWidth + 4) * m_scale);
    const std::int32_t h = std::min(static_cast<std::int32_t>(frameH) - 2 * margin, (30 * kLineHeight + 4) * m_scale);
    m_panel = (w > 0 && h > 0) ? Rect{margin, margin, w, h} : Rect{};
}

bool DevMenu::takeCaptureRequest() {
    const bool r = m_captureRequest;
    m_captureRequest = false;
    return r;
}

void DevMenu::pass(const InputEvent* event, const MenuFrame& frame) {
    m_ui.begin(m_panel, m_scale, event);
    header();
    switch (m_tab) {
    case Tab::Options:
        optionsTab();
        break;
    case Tab::Textures:
        texturesTab(frame);
        break;
    case Tab::Debug:
        debugTab(frame);
        break;
    case Tab::Capture:
        captureTab(frame);
        break;
    default:
        statsTab(frame);
        break;
    }
    m_ui.end();
}

void DevMenu::header() {
    m_ui.title("Relight dev  Alt+X");
    static constexpr const char* kLabels[] = {"Sta", "Opt", "Tex", "Dbg", "Cap"};
    static constexpr const char* kIds[] = {"tab.stats", "tab.opts", "tab.tex", "tab.dbg", "tab.cap"};
    for (std::size_t i = 0; i < static_cast<std::size_t>(Tab::Count); ++i) {
        if (i > 0) {
            m_ui.sameLine();
        }
        if (m_ui.button(kIds[i], kLabels[i], static_cast<std::size_t>(m_tab) == i)) {
            m_tab = static_cast<Tab>(i);
        }
    }
}

void DevMenu::statsTab(const MenuFrame& f) {
    static const MenuStats kNone;
    const MenuStats& s = f.stats ? *f.stats : kNone;
    if (s.frameMs >= 0.0) {
        m_ui.textf(palette::kText, "frame %" PRIu64 " %.2fms", s.frame, s.frameMs);
    } else {
        m_ui.textf(palette::kText, "frame %" PRIu64 " --", s.frame);
    }
    m_ui.textf(palette::kText, "mode %s", s.mode);
    if (std::string_view(s.inject) == "ui") {
        m_ui.textf(palette::kText, "inject ui@%u %s", s.injectDraw, s.pass);
    } else {
        m_ui.textf(palette::kText, "inject %s %s", s.inject, s.pass);
    }
    m_ui.textf(palette::kText, "draws %u scene %u", s.draws, s.sceneDraws);
    m_ui.textf(palette::kText, "repl %u lights %u", s.replaced, s.lights);
    m_ui.textf(palette::kText, "inst %u gpu %u", s.instances, s.gpuInstances);
    m_ui.textf(palette::kText, "bindless %u swaps %u", s.bindless, s.swaps);
    m_ui.textf(palette::kText, "tex %u %s", f.textures ? static_cast<unsigned>(f.textures->size()) : 0u,
               s.renderer ? "adopted" : "no renderer");
    if (!s.error.empty()) {
        m_ui.text(s.error, palette::kWarn);
    }
}

void DevMenu::refreshOptionList() {
    if (m_filterBuilt == m_filter) {
        return;
    }
    m_filterBuilt = m_filter;
    m_options.clear();
    for (const auto& [name, option] : opt::OptionManager::getOptions()) {
        if (option && containsNoCase(name, m_filter)) {
            m_options.push_back(option);
        }
    }
    m_optionScroll = 0;
}

void DevMenu::optionsTab() {
    m_ui.textField("opt.filter", m_filter, std::max(4, m_ui.charsLeft() - 6), 48);
    m_ui.sameLine();
    if (m_ui.button("save", "Save")) {
        std::string what;
        if (saveLayers(&what)) {
            ++m_saves;
            m_status = "saved " + what;
        } else {
            ++m_saveFailures;
            m_status = "save failed";
        }
    }
    refreshOptionList();
    const std::int32_t count = static_cast<std::int32_t>(m_options.size());
    const std::int32_t page = std::max(1, (m_ui.linesLeft() - 1) / 2);
    char id[160];
    char value[64];
    for (std::int32_t row = m_optionScroll; row < std::min(count, m_optionScroll + page); ++row) {
        opt::OptionBase& o = *m_options[static_cast<std::size_t>(row)];
        const std::string& name = o.getFullName();
        m_ui.text(name, o.isDefault() ? palette::kDim : palette::kAccent);
        const opt::OptionValue v = o.getResolvedValue();
        std::snprintf(id, sizeof(id), "opt.%s", name.c_str());
        if (const bool* b = std::get_if<bool>(&v)) {
            if (m_ui.button(id, *b ? "True" : "False", *b) && editOption(o, *b ? "False" : "True")) {
                ++m_edits;
            }
        } else if (const std::int32_t* i = std::get_if<std::int32_t>(&v)) {
            std::snprintf(id, sizeof(id), "opt.%s.dec", name.c_str());
            const bool dec = m_ui.button(id, "-");
            m_ui.sameLine();
            std::snprintf(value, sizeof(value), "%d", *i);
            m_ui.text(value);
            m_ui.sameLine();
            std::snprintf(id, sizeof(id), "opt.%s.inc", name.c_str());
            const bool inc = m_ui.button(id, "+");
            if (dec || inc) {
                std::snprintf(value, sizeof(value), "%d", *i + (inc ? 1 : -1));
                if (editOption(o, value)) {
                    ++m_edits;
                }
            }
        } else if (const float* fl = std::get_if<float>(&v)) {
            std::snprintf(id, sizeof(id), "opt.%s.dec", name.c_str());
            const bool dec = m_ui.button(id, "-");
            m_ui.sameLine();
            std::snprintf(value, sizeof(value), "%g", static_cast<double>(*fl));
            m_ui.text(value);
            m_ui.sameLine();
            std::snprintf(id, sizeof(id), "opt.%s.inc", name.c_str());
            const bool inc = m_ui.button(id, "+");
            if (dec || inc) {
                const float step = *fl != 0.f ? std::max(std::abs(*fl) * 0.1f, 1e-4f) : 0.1f;
                std::snprintf(value, sizeof(value), "%g", static_cast<double>(*fl + (inc ? step : -step)));
                if (editOption(o, value)) {
                    ++m_edits;
                }
            }
        } else if (const opt::HashSetLayer* hs = std::get_if<opt::HashSetLayer>(&v)) {
            m_ui.textf(palette::kDim, "{%llu hashes}", static_cast<unsigned long long>(hs->size()));
        } else {
            m_ui.text(opt::formatOptionValue(v), palette::kDim);
        }
    }
    if (m_ui.button("opt.up", "<")) {
        m_optionScroll = scrolled(m_optionScroll, kVkPrior, 0, page, count);
    }
    m_ui.sameLine();
    if (m_ui.button("opt.down", ">")) {
        m_optionScroll = scrolled(m_optionScroll, kVkNext, 0, page, count);
    }
    m_ui.sameLine();
    m_ui.textf(palette::kDim, "%d/%d", count ? m_optionScroll + 1 : 0, count);
    if (!m_status.empty()) {
        m_ui.text(m_status, palette::kDim);
    }
    m_optionScroll = scrolled(m_optionScroll, m_ui.keyPressed(), m_ui.wheel(), page, count);
}

void DevMenu::texturesTab(const MenuFrame& f) {
    const std::int32_t count = f.textures ? static_cast<std::int32_t>(f.textures->size()) : 0;
    m_ui.textf(palette::kText, "%d textures", count);
    m_ui.sameLine();
    if (m_ui.button("save", "Save")) {
        std::string what;
        if (saveLayers(&what)) {
            ++m_saves;
            m_status = "saved " + what;
        } else {
            ++m_saveFailures;
            m_status = "save failed";
        }
    }
    const std::int32_t page = std::max(1, (m_ui.linesLeft() - 1) / 2);
    m_textureScroll = std::clamp(m_textureScroll, 0, std::max(0, count - page));
    char id[64];
    for (std::int32_t row = m_textureScroll; row < std::min(count, m_textureScroll + page); ++row) {
        const MenuTexture& t = (*f.textures)[static_cast<std::size_t>(row)];
        m_ui.textf(palette::kAccent, "%d %016" PRIX64, row, t.hash);
        bool first = true;
        for (const TextureCategory& c : kTextureCategories) {
            if (!hashList(c.option)) {
                continue;
            }
            if (!first) {
                m_ui.sameLine();
            }
            first = false;
            std::snprintf(id, sizeof(id), "tex.%d.%s", row, c.key);
            if (m_ui.button(id, c.label, textureInCategory(c, t.hash)) && toggleTextureCategory(c, t.hash)) {
                ++m_tags;
            }
        }
        m_ui.sameLine();
        m_ui.textf(palette::kDim, "%ux%u", t.width, t.height);
    }
    if (m_ui.button("tex.up", "<")) {
        m_textureScroll = scrolled(m_textureScroll, kVkPrior, 0, page, count);
    }
    m_ui.sameLine();
    if (m_ui.button("tex.down", ">")) {
        m_textureScroll = scrolled(m_textureScroll, kVkNext, 0, page, count);
    }
    if (!m_status.empty()) {
        m_ui.sameLine();
        m_ui.text(m_status, palette::kDim);
    }
    m_textureScroll = scrolled(m_textureScroll, m_ui.keyPressed(), m_ui.wheel(), page, count);
}

void DevMenu::debugTab(const MenuFrame& f) {
    const std::int32_t current = OverlayOptions::debugView();
    char id[32];
    for (std::size_t i = 0; i < kDebugViewCount; ++i) {
        const DebugView v = static_cast<DebugView>(i);
        const bool available = i == 0 || (f.debugAvailable && (*f.debugAvailable)[i]);
        std::snprintf(id, sizeof(id), "dbg.%s", debugViewName(v));
        if (m_ui.button(id, debugViewName(v), current == static_cast<std::int32_t>(i))) {
            OverlayOptions::debugView.setImmediately(static_cast<std::int32_t>(i));
            ++m_edits;
        }
        if (!available) {
            m_ui.sameLine();
            m_ui.text("n/a", palette::kDim);
        }
    }
}

void DevMenu::captureTab(const MenuFrame& f) {
    char label[48];
    std::snprintf(label, sizeof(label), "Capture %u", f.captureFrames);
    const bool busy = f.capture && (f.capture->requested || f.capture->active);
    if (m_ui.button("cap.go", label, busy) && !busy) {
        m_captureRequest = true;
    }
    if (!f.capture) {
        return;
    }
    const CaptureStatus& c = *f.capture;
    if (c.requested) {
        m_ui.text("starting", palette::kAccent);
    } else if (c.active) {
        m_ui.textf(palette::kAccent, "capturing %u/%u", c.framesDone, c.framesTotal);
    }
    if (c.written) {
        m_ui.text(c.ok ? "written" : "failed", c.ok ? palette::kText : palette::kWarn);
        m_ui.text(c.dir, palette::kDim);
        m_ui.textf(palette::kDim, "mesh %llu tex %llu", static_cast<unsigned long long>(c.meshes),
                   static_cast<unsigned long long>(c.textures));
        m_ui.textf(palette::kDim, "inst %llu", static_cast<unsigned long long>(c.instances));
    }
    if (!c.error.empty()) {
        m_ui.text(c.error, palette::kWarn);
    }
}

// ---- the state machine -----------------------------------------------------------------------------------------

OverlayCore::OverlayCore(const OverlayConfig& config) : m_config(config) {
    m_layer.reserve(64 * 1024);
    setVisible(config.startVisible);
}

void OverlayCore::setVisible(bool visible) {
    m_visible = visible;
    m_input.setVisible(visible);
}

std::size_t OverlayCore::update(std::uint32_t frameW, std::uint32_t frameH, const MenuFrame& frame, bool finalPass) {
    const std::size_t n = m_input.drain(m_events.data(), m_events.size());
    m_eventsTotal += n;
    bool viewport = false;
    for (std::size_t i = 0; i < n; ++i) {
        const InputEvent& e = m_events[i];
        if (e.type == EventType::Toggle) {
            setVisible(!m_visible);
            ++m_togglesApplied;
            continue;
        }
        if (!m_visible) {
            continue; // hidden: the game got the message (the hook forwarded it)
        }
        if (!viewport) {
            m_menu.setViewport(frameW, frameH, m_config.scale);
            viewport = true;
        }
        m_menu.pass(&e, frame);
    }
    if (m_visible && finalPass) {
        m_menu.setViewport(frameW, frameH, m_config.scale);
        m_menu.pass(nullptr, frame);
    }
    return n;
}

void OverlayCore::draw() { rasterize(m_menu.ui().draws(), m_menu.panel(), m_layer); }

std::size_t OverlayCore::runScript(const Script& script, std::uint64_t frame, WindowHook& hook, std::uint32_t frameW,
                                   std::uint32_t frameH, const MenuFrame& menuFrame, std::string* error) {
    std::size_t begin = 0, end = 0;
    script.range(frame, begin, end);
    std::vector<WindowMessage> messages;
    for (std::size_t i = begin; i < end; ++i) {
        const ScriptCommand& c = script.commands()[i];
        const CoordMap map = m_input.coordMap();
        std::int32_t fx = c.x, fy = c.y;
        if (c.kind == ScriptCommand::Kind::ClickWidget) {
            Rect r;
            if (!m_menu.ui().widgetRect(c.text, r)) {
                if (error && error->empty()) {
                    *error = "script line " + std::to_string(c.line) + ": widget '" + c.text + "' is not laid out";
                }
                continue;
            }
            fx = r.x + r.w / 2;
            fy = r.y + r.h / 2;
        }
        messages.clear();
        Script::messages(c, map.toClientX(fx), map.toClientY(fy), messages);
        for (const WindowMessage& m : messages) {
            hook.send(m_input, m.msg, m.wParam, m.lParam);
        }
        update(frameW, frameH, menuFrame, true);
    }
    return end - begin;
}

} // namespace fuse::relight::overlay
