// FUSE UI (UI2): immediate-mode front-end. See immediate.hpp.
#include <fuse/ui/immediate.hpp>

#include <algorithm>

namespace fuse::ui::im {

u64 idOf(std::string_view label) {
    u64 h = 1469598103934665603ull;
    for (char c : label) h = (h ^ u8(c)) * 1099511628211ull;
    return h;
}

std::string_view visibleLabel(std::string_view label) {
    const usize at = label.find("##");
    return at == std::string_view::npos ? label : label.substr(0, at);
}

ImmediateUi::ImmediateUi(UiContext& ui, NodeId parent) : m_ui(ui) {
    m_container = ui.create("Panel", parent.isValid() ? parent : ui.root(), "im");
    m_ui.layout(m_container).gap = 6.f;
    m_ownsContainer = true;
}

ImmediateUi::~ImmediateUi() {
    if (m_ownsContainer) m_ui.destroy(m_container);
}

ImmediateUi::Entry* ImmediateUi::entry(u64 key) {
    for (Entry& e : m_entries) if (e.key == key) return &e;
    return nullptr;
}

void ImmediateUi::begin() {
    if (m_rebuild) {
        for (Entry& e : m_entries) m_ui.destroy(e.node);
        m_entries.clear();
        m_rebuild = false;
        ++m_rebuilds;
    }
    for (Entry& e : m_entries) e.touched = false;
    m_sequence.clear();
}

void ImmediateUi::end() {
    for (Entry& e : m_entries) if (!e.touched) m_ui.destroy(e.node);
    std::erase_if(m_entries, [](const Entry& e) { return !e.touched; });
    // Child order must follow call order; a reorder is rebuilt at the next begin().
    usize i = 0;
    for (NodeId c = m_ui.firstChild(m_container); c.isValid(); c = m_ui.nextSibling(c), ++i) {
        if (i >= m_sequence.size() || entry(m_sequence[i])->node != c) {
            m_rebuild = true;
            break;
        }
    }
    m_lastSequence = m_sequence;
}

ImmediateUi::Entry& ImmediateUi::touch(std::string_view label, std::string_view cls) {
    const u64 key = idOf(label);
    Entry* e = entry(key);
    if (!e || !m_ui.alive(e->node)) {
        if (!e) {
            m_entries.push_back(Entry{key, NodeId{}, false, 0, false});
            e = &m_entries.back();
        }
        e->node = m_ui.create(cls, m_container, label);
        m_ui.onClicked(e->node, [this, key](UiContext&, NodeId) { if (Entry* x = entry(key)) ++x->clicks; });
        m_ui.onChanged(e->node, [this, key](UiContext&, NodeId) {
            if (Entry* x = entry(key); x && !m_syncing) x->changed = true;
        });
    }
    e->touched = true;
    m_sequence.push_back(key);
    m_ui.setText(e->node, visibleLabel(label));
    return *e;
}

NodeId ImmediateUi::nodeFor(std::string_view label) const {
    const u64 key = idOf(label);
    for (const Entry& e : m_entries) if (e.key == key) return e.node;
    return {};
}

void ImmediateUi::label(std::string_view text) { touch(text, "Label"); }

bool ImmediateUi::button(std::string_view label) {
    Entry& e = touch(label, "Button");
    const bool clicked = e.clicks > 0;
    e.clicks = 0;
    return clicked;
}

bool ImmediateUi::checkbox(std::string_view label, bool& value) {
    Entry& e = touch(label, "CheckBox");
    const bool changed = e.changed;
    e.changed = false;
    if (changed) value = m_ui.checked(e.node);
    else {
        m_syncing = true;
        m_ui.setChecked(e.node, value);
        m_syncing = false;
    }
    return changed;
}

bool ImmediateUi::slider(std::string_view label, f32& value, f32 min, f32 max) {
    Entry& e = touch(label, "Slider");
    m_ui.setAccessibleName(e.node, visibleLabel(label));
    m_syncing = true;
    m_ui.setRange(e.node, min, max);
    m_syncing = false;
    const bool changed = e.changed;
    e.changed = false;
    if (changed) value = m_ui.value(e.node);
    else {
        m_syncing = true;
        m_ui.setValue(e.node, value);
        m_syncing = false;
    }
    return changed;
}

bool ImmediateUi::inputText(std::string_view label, std::string& value) {
    const u64 key = idOf(label);
    Entry* existing = entry(key);
    const bool isNew = !existing;
    const bool changed = existing && existing->changed;
    if (changed) value = m_ui.text(existing->node);
    Entry& e = touch(label, "LineEdit");
    // touch() set the text to the label; a line edit shows its value instead.
    m_ui.setAccessibleName(e.node, visibleLabel(label));
    m_ui.setText(e.node, value);
    if (isNew) m_ui.setCursor(e.node, u32(value.size()));
    e.changed = false;
    return changed;
}

void ImmediateUi::progress(std::string_view label, f32 fraction) {
    Entry& e = touch(label, "ProgressBar");
    m_ui.setAccessibleName(e.node, visibleLabel(label));
    m_ui.setText(e.node, {});
    m_syncing = true;
    m_ui.setValue(e.node, std::clamp(fraction, 0.f, 1.f));
    m_syncing = false;
}

} // namespace fuse::ui::im
