// SVG widgets: the process-wide SVG store (SvgHandle), the `svgimage` widget and icons on buttons / tool buttons.
#include "fuse/ui/widgets/widgets.hpp"
#include "widgets_internal.hpp"

#include "fuse/ui/painter.hpp"

#include <cmath>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace fuse::ui::widgets {
namespace {

// ---- SVG store: slots with generations; images are immutable once stored ----
struct SvgSlot {
    std::unique_ptr<SvgImage> image; ///< stable address while alive
    u32 generation = 1;
    bool permanent = false; ///< built-in icons
};
struct SvgStore {
    std::mutex mutex; // creation / destruction may come from loader threads; painting reads stable pointers
    std::vector<SvgSlot> slots;
    std::vector<u32> free;
    SvgHandle icons[kIconCount]{};
    bool iconsReady = false;
};
SvgStore& store() {
    static SvgStore s;
    return s;
}
SvgHandle adoptLocked(SvgStore& s, SvgImage&& img, bool permanent) {
    u32 i;
    if (!s.free.empty()) {
        i = s.free.back();
        s.free.pop_back();
    } else {
        i = u32(s.slots.size());
        s.slots.emplace_back();
    }
    s.slots[i].image = std::make_unique<SvgImage>(std::move(img));
    s.slots[i].permanent = permanent;
    return {i, s.slots[i].generation};
}

u64 pack(SvgHandle h) { return u64(h.index) | (u64(h.generation) << 32); }
SvgHandle unpack(u64 v) { return {u32(v & 0xFFFFFFFFu), u32(v >> 32)}; }

// ---- button icons: a side table keyed by (context uid, node index); the entry checks the node's generation ----
struct IconEntry {
    NodeHandle node;
    SvgHandle svg;
    f32 size = 16.f;
};
std::unordered_map<u64, IconEntry>& iconTable() {
    static std::unordered_map<u64, IconEntry> t;
    return t;
}
u64 iconKey(const UiContext& ui, NodeHandle n) { return (ui.uid() << 32) | u64(n.index); }
IconEntry* findIconEntry(const UiContext& ui, NodeHandle n) { // (a lookup does not allocate)
    auto& t = iconTable();
    const auto it = t.find(iconKey(ui, n));
    return it != t.end() && it->second.node == n ? &it->second : nullptr;
}

constexpr f32 kIconGap = 6.f;

// ---- svgimage ----
struct SvgImageW {
    static constexpr std::string_view kName = "svgimage";
    static constexpr A11yRole kRole = A11yRole::Image;
    static Size measure(UiContext& ui, NodeHandle n, Size) {
        const SvgImage* img = svgGet(unpack(ui.userData(n)));
        return img ? Size{std::ceil(img->size().w), std::ceil(img->size().h)} : Size{};
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const SvgImage* img = svgGet(unpack(pc.ui.userData(pc.node)));
        if (!img) return;
        Painter p(pc, dl);
        img->render(p, pc.content, pc.style.color);
    }
};

} // namespace

std::expected<SvgHandle, SvgError> svgCreate(std::string_view xml) {
    auto img = SvgImage::parse(xml);
    if (!img) return std::unexpected(std::move(img.error()));
    return svgAdopt(std::move(*img));
}

SvgHandle svgAdopt(SvgImage image) {
    SvgStore& s = store();
    std::lock_guard lock(s.mutex);
    return adoptLocked(s, std::move(image), false);
}

void svgDestroy(SvgHandle h) {
    SvgStore& s = store();
    std::lock_guard lock(s.mutex);
    if (h.index >= s.slots.size()) return;
    SvgSlot& slot = s.slots[h.index];
    if (slot.generation != h.generation || !slot.image || slot.permanent) return;
    slot.image.reset();
    ++slot.generation;
    s.free.push_back(h.index);
}

const SvgImage* svgGet(SvgHandle h) {
    SvgStore& s = store();
    std::lock_guard lock(s.mutex);
    if (h.index >= s.slots.size() || s.slots[h.index].generation != h.generation) return nullptr;
    return s.slots[h.index].image.get();
}

SvgHandle iconHandle(Icon icon) {
    SvgStore& s = store();
    std::lock_guard lock(s.mutex);
    if (!s.iconsReady) {
        for (u32 i = 0; i < kIconCount; ++i) s.icons[i] = adoptLocked(s, SvgImage(iconImage(Icon(i))), true);
        s.iconsReady = true;
    }
    return s.icons[std::min(u32(icon), kIconCount - 1)];
}

NodeHandle svgImage(UiContext& ui, NodeHandle parent, SvgHandle svg, f32 width, f32 height, std::string_view id) {
    auto h = ui.create("svgimage", parent, id);
    if (!h) return {};
    ui.setUserData(*h, pack(svg));
    if (width > 0.f) ui.set(*h, Prop::Width, PropValue::ofLength(Length::px(width)));
    if (height > 0.f) ui.set(*h, Prop::Height, PropValue::ofLength(Length::px(height)));
    return *h;
}

void setSvgImage(UiContext& ui, NodeHandle node, SvgHandle svg) {
    if (!ui.alive(node)) return;
    ui.setUserData(node, pack(svg));
    ui.invalidate(node);
}

SvgHandle svgImageOf(const UiContext& ui, NodeHandle node) { return ui.alive(node) ? unpack(ui.userData(node)) : SvgHandle{}; }

void setIcon(UiContext& ui, NodeHandle button, SvgHandle svg, f32 size) {
    if (!ui.alive(button)) return;
    auto& t = iconTable();
    std::erase_if(t, [&](const auto& kv) { return (kv.first >> 32) == ui.uid() && !ui.alive(kv.second.node); }); // destroyed since
    if (!svg.valid()) t.erase(iconKey(ui, button));
    else t[iconKey(ui, button)] = {button, svg, size};
    ui.invalidateLayout(button); // the icon changes the content size
    ui.invalidate(button);
}

void setIcon(UiContext& ui, NodeHandle button, Icon icon, f32 size) { setIcon(ui, button, iconHandle(icon), size); }
void clearIcon(UiContext& ui, NodeHandle button) { setIcon(ui, button, SvgHandle{}, 0.f); }

SvgHandle iconOf(const UiContext& ui, NodeHandle button) {
    const IconEntry* e = iconTable().empty() ? nullptr : findIconEntry(ui, button);
    return e ? e->svg : SvgHandle{};
}

void forgetIcons(const UiContext& ui) {
    std::erase_if(iconTable(), [&](const auto& kv) { return (kv.first >> 32) == ui.uid(); });
}

NodeHandle iconButton(UiContext& ui, NodeHandle parent, Icon icon, std::string_view text, std::string_view id) {
    const NodeHandle b = toolButton(ui, parent, text, id);
    if (b.valid()) {
        setIcon(ui, b, icon);
        if (text.empty()) ui.setA11y(b, A11yRole::Button, iconName(icon));
    }
    return b;
}

void registerSvgWidgets(UiContext& ui) { ui.registry().registerWidget<SvgImageW>(); }

namespace detail {

Size iconContentSize(const UiContext& ui, NodeHandle n, Size text) {
    if (iconTable().empty()) return text;
    const IconEntry* e = findIconEntry(ui, n);
    if (!e) return text;
    return {e->size + (text.w > 0.f ? kIconGap + text.w : 0.f), std::max(text.h, e->size)};
}

bool paintIcon(const PaintContext& pc, DrawList& dl, Rect& textBox) {
    if (iconTable().empty()) return false;
    const IconEntry* e = findIconEntry(pc.ui, pc.node);
    if (!e) return false;
    const Rect c = pc.content;
    const std::string_view text = pc.ui.text(pc.node);
    const f32 textWidth = text.empty() ? 0.f : std::ceil(pc.ui.measureString(text, pc.style.fontSize));
    const f32 total = e->size + (textWidth > 0.f ? kIconGap + textWidth : 0.f);
    const f32 x = std::round(c.x + std::max(0.f, (c.w - total) * 0.5f));
    const Rect box{x, std::round(c.y + (c.h - e->size) * 0.5f), e->size, e->size};
    if (const SvgImage* img = svgGet(e->svg)) {
        Painter p(pc, dl);
        img->render(p, box, pc.style.color);
    }
    textBox = {box.right() + kIconGap, c.y, std::max(0.f, c.right() - box.right() - kIconGap), c.h};
    return true;
}

} // namespace detail
} // namespace fuse::ui::widgets
