#include "fuse/ui/widgets/widgets.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace fuse::ui::widgets {

// The widget theme: one rule body over `:root` colour tokens. Dark, light and high contrast differ only in the token
// values (plus the outline rules of high contrast), so apps restyle by overriding tokens. Every colour of the rules
// and of the widget paint code (widgets_internal.hpp `tok::`) is a token; `background-end` doubles as the accent /
// indicator colour for widgets that draw one (no gradient set).
namespace {
/// `hc`: where the high-contrast value comes from - W window, T windowText, F buttonFace, H highlight,
/// h highlightText, G grayText, L hotlight, S selection (window mixed with highlight), D dim line (window mixed with
/// windowText), 0 transparent, = the dark value.
struct TokenRow {
    std::string_view name, dark, light;
    char hc;
};
// @tokens-begin (generated order: classic tokens, rule tokens, paint tokens)
constexpr TokenRow kTokens[] = {
    {"--bg", "#1e1f24", "#f3f3f5", 'W'},
    {"--surface", "#2a2b32", "#ffffff", 'W'},
    {"--surface-hi", "#34353e", "#e7e8ed", 'F'},
    {"--border", "#474955", "#8a8d9b", 'T'},
    {"--text", "#e6e6eb", "#1c1d21", 'T'},
    {"--text-dim", "#9a9cab", "#5d606e", 'G'},
    {"--accent", "#3d8bfd", "#0a64d8", 'H'},
    {"--accent-hi", "#5a9dff", "#0957bd", 'L'},
    {"--danger", "#e5484d", "#c62f35", 'H'},
    {"--hover", "#40414c", "#dcdde4", 'F'},
    {"--pressed", "#23242a", "#c8c9d2", 'F'},
    {"--on-accent", "#ffffff", "#ffffff", 'h'},
    {"--heading", "#ffffff", "#0e0f12", 'T'},
    {"--field", "#15161a", "#ffffff", 'W'},
    {"--field-alt", "#1e1f25", "#ffffff", 'W'},
    {"--tooltip-bg", "#0f1013", "#fbfbfd", 'W'},
    {"--code-bg", "#101114", "#eceef2", 'W'},
    {"--code-text", "#b8e0a0", "#2c6a1c", 'T'},
    {"--quote-bg", "#23242b", "#e6e7ec", 'W'},
    {"--canvas", "#17171c", "#fafafb", 'W'},
    {"--warning", "#d9a400", "#8a6100", 'H'},
    {"--shadow-card", "rgba(0, 0, 0, 0.45)", "rgba(0, 0, 0, 0.18)", '0'},
    {"--shadow-pop", "rgba(0, 0, 0, 0.5)", "rgba(0, 0, 0, 0.22)", '0'},
    {"--scrim", "rgba(0, 0, 0, 0.35)", "rgba(0, 0, 0, 0.25)", '0'},
    {"--primary-ring", "#ffffff", "#0e0f12", 'L'},
    {"--inset", "rgba(0, 0, 0, 0.25)", "rgba(0, 0, 0, 0.06)", 'W'},
    {"--control-border", "#808080", "#6e7180", 'T'},
    {"--track", "#5a5a66", "#a9acb8", 'T'},
    {"--knob", "#f0f0f5", "#4a4d5a", 'T'},
    {"--switch-knob", "#ffffff", "#ffffff", 'W'},
    {"--knob-hi", "#ffffff", "#1c1d21", 'T'},
    {"--scroll-track", "rgba(0, 0, 0, 0.2)", "rgba(0, 0, 0, 0.08)", 'W'},
    {"--scroll-thumb", "#8a8a96", "#8e919e", 'T'},
    {"--scroll-thumb-active", "#b4b4c0", "#6a6d7a", 'H'},
    {"--view-scroll-track", "rgba(0, 0, 0, 0.15)", "rgba(0, 0, 0, 0.06)", 'W'},
    {"--progress-track", "rgba(0, 0, 0, 0.3)", "rgba(0, 0, 0, 0.12)", 'G'},
    {"--dial-face", "#2a2a33", "#ffffff", 'W'},
    {"--separator", "#44444f", "#c3c5cf", 'T'},
    {"--raised", "#2a2a33", "#e7e8ed", 'F'},
    {"--grid-line", "#55555f", "#c3c5cf", 'T'},
    {"--stripe", "rgba(255, 255, 255, 0.035)", "rgba(0, 0, 0, 0.035)", '0'},
    {"--hot", "#ff6b6b", "#d6373d", 'L'},
    {"--glyph-dim", "#9a9aa6", "#6a6d7a", 'T'},
    {"--strip", "#22222a", "#dcdde4", 'W'},
    {"--dropzone", "#22232b", "#ffffff", 'W'},
    {"--dropzone-glyph", "#c8cad4", "#4a4d5a", 'T'},
    {"--link-hover", "#8fbcff", "#0a4aa0", 'L'},
    {"--chart-line", "#ffffff", "#000000", 'T'},
    {"--canvas-grid", "#23232a", "#ececf0", 'D'},
    {"--canvas-grid-hi", "#2a2a33", "#e0e1e7", 'D'},
    {"--wire", "#ffffff", "#1c1d21", 'T'},
    {"--shadow", "#000000", "rgba(0, 0, 0, 0.5)", '0'},
    {"--line-strong", "#3a3a45", "#c3c5cf", 'T'},
    {"--node-head", "#3b4a6b", "#c9d6f2", 'F'},
    {"--pin-outline", "#111111", "#ffffff", 'W'},
    {"--key", "#e8e8ee", "#3a3c47", 'T'},
    {"--key-selected", "#ffd93d", "#b06a00", 'L'},
    {"--text-faint", "#8a8a96", "#565967", 'T'},
    {"--on-clip", "#101014", "#101014", '='},
    {"--row-line", "#26262e", "#e6e7ec", 'D'},
    {"--media-bg", "#000000", "#000000", '='},
    {"--media-bar", "#202027", "#e7e8ed", 'W'},
    {"--code-editor-bg", "#1a1a20", "#ffffff", 'W'},
    {"--gutter", "#212128", "#f0f1f4", 'W'},
    {"--gutter-text", "#6a6a76", "#676a78", 'T'},
    {"--gutter-text-hi", "#c8c8d0", "#1c1d21", 'L'},
    {"--current-line", "rgba(255, 255, 255, 0.04)", "rgba(0, 0, 0, 0.04)", '0'},
    {"--selection", "#264f78", "#add6ff", 'S'},
    {"--caret", "#ffffff", "#000000", 'T'},
    {"--code-fg", "#d4d4d4", "#1c1d21", 'T'},
    {"--code-keyword", "#569cd6", "#0550ae", 'L'},
    {"--code-string", "#ce9178", "#a31515", 'T'},
    {"--code-number", "#b5cea8", "#116644", 'T'},
    {"--code-comment", "#6a9955", "#3d6b2f", 'G'},
    {"--hover-wash", "rgba(255, 255, 255, 0.06)", "rgba(0, 0, 0, 0.06)", '0'},
    {"--meter-bg", "#15171c", "#e7e8ed", 'D'},
    {"--meter-ok", "#3bd16f", "#1e9e4a", 'T'},
    {"--meter-warn", "#f2c94c", "#b98900", 'L'},
    {"--meter-danger", "#eb5757", "#c62f35", 'H'},
    {"--meter-hold", "#e8eaee", "#1c1d21", 'T'},
    {"--meter-lamp-off", "#3a1e22", "#e6c9cc", 'D'},
    {"--lcd-text", "#7cfc9a", "#0b5a26", 'T'},
    {"--lcd-bg", "#0b1a10", "#e2efe5", 'W'},
    {"--workspace", "#121317", "#d5d6de", 'W'},
    {"--scene-bg", "#15181d", "#fafafb", 'W'},
    {"--item-focus", "#ffd54f", "#b06a00", 'L'},
    {"--placeholder", "rgba(230, 230, 235, 0.5)", "#5d606e", 'G'},
    {"--window-close", "#c42b1c", "#c42b1c", 'H'},
    {"--focus-ring", "var(--accent)", "var(--accent)", 'L'},
    {"--text-disabled", "rgba(230, 230, 235, 0.4)", "rgba(28, 29, 33, 0.45)", 'G'},
};
// @tokens-end

constexpr std::string_view kRootTail = "  background: var(--bg); color: var(--text); font-size: 14px;\n}\n";
constexpr std::string_view kOverrideTail = "  background: var(--bg); color: var(--text);\n}\n";

constexpr std::string_view kBody = R"css(panel { background: transparent; }
label { color: var(--text); }
button, toolbutton {
  background: var(--surface-hi); color: var(--text); border: 1px var(--border); border-radius: 6px;
  padding: 6px 14px; text-align: center;
}
toolbutton { padding: 4px 8px; border-radius: 4px; background: transparent; border-width: 0px; }
button:hover, toolbutton:hover { background: var(--hover); }
button:pressed, toolbutton:pressed { background: var(--pressed); }
button:focus-visible, toolbutton:focus-visible, lineedit:focus, spinbox:focus-visible, combobox:focus-visible { border-color: var(--accent); }
button:disabled { color: var(--text-dim); background: var(--surface); }
button.primary { background: var(--accent); border-color: var(--accent); color: var(--on-accent); }
button.primary:hover { background: var(--accent-hi); }
checkbox, radio, switch, slider, dial, progressbar, scrollbar { background-end: var(--accent); }
checkbox, radio, switch { padding: 2px 0px; }
slider { height: 22px; }
progressbar { height: 8px; border-radius: 4px; }
lineedit, spinbox, combobox {
  background: var(--field); color: var(--text); border: 1px var(--border); border-radius: 5px; padding: 5px 8px;
}
combobox { background: var(--surface-hi); }
link { color: var(--accent-hi); }
separator { border-color: var(--border); }
.card {
  background: var(--surface); border-radius: 10px; padding: 14px; gap: 10px;
  box-shadow: 0px 4px 14px var(--shadow-card);
}
menu {
  background: var(--surface); border: 1px var(--border); border-radius: 6px; padding: 4px; gap: 0px;
  box-shadow: 0px 6px 18px var(--shadow-pop);
}
menuitem { padding: 4px 12px; border-radius: 4px; color: var(--text); }
menuitem:focus, menuitem:hover { background: var(--surface-hi); }
menubar { background: var(--surface); padding: 2px 4px; gap: 2px; }
menubutton { padding: 4px 10px; border-radius: 4px; color: var(--text); }
menubutton:hover, menubutton:focus-visible { background: var(--surface-hi); }
listview, treeview {
  background: var(--field); color: var(--text); border: 1px var(--border); border-radius: 5px; padding: 3px;
  background-end: var(--accent);
}
listview:focus, treeview:focus, tabbar:focus-visible { border-color: var(--accent); }
tabbar { color: var(--text); background-end: var(--accent); }
popup.modal { background: var(--scrim); }
.dialog { min-width: 260px; }
tooltip {
  background: var(--tooltip-bg); color: var(--text); border: 1px var(--border); border-radius: 4px; padding: 3px 7px;
  font-size: 12px;
}
.row { flex-direction: row; gap: 8px; align-items: center; }
.title { font-size: 20px; color: var(--heading); }
.dim { color: var(--text-dim); }
gridview, calendar, chart, colorpicker {
  background: var(--field); color: var(--text); border: 1px var(--border); border-radius: 5px; padding: 4px;
  background-end: var(--accent);
}
gridview:focus, calendar:focus, chart:focus, colorpicker:focus { border-color: var(--accent); }
.propgrid { gap: 2px; padding: 4px; background: var(--field); border: 1px var(--border); border-radius: 5px; }
.proprow { flex-direction: row; gap: 8px; align-items: center; padding: 1px 4px; }
.propname { color: var(--text-dim); width: 110px; }
.propgroup { color: var(--heading); background: var(--surface); border-radius: 3px; padding: 3px 6px; }
.propgroup:hover { background: var(--surface-hi); }
.md { gap: 6px; padding: 6px; }
.md-h1 { font-size: 22px; color: var(--heading); }
.md-h2 { font-size: 18px; color: var(--heading); }
.md-h3 { font-size: 15px; color: var(--accent-hi); }
.md-p { color: var(--text); }
.md-li { color: var(--text); padding: 0px 0px 0px 14px; }
.md-code { background: var(--code-bg); color: var(--code-text); padding: 6px 8px; border-radius: 4px; font-size: 13px; }
.md-quote { color: var(--text-dim); background: var(--quote-bg); padding: 4px 10px; border-radius: 3px; }
nodegraph, curveeditor, timeline, viewport, codeedit, video {
    background: var(--canvas); color: var(--text); border: 1px var(--border); border-radius: 4px; padding: 1px;
    background-end: var(--accent);
}
nodegraph:focus, curveeditor:focus, timeline:focus, viewport:focus, codeedit:focus, video:focus { border-color: var(--accent); }
codeedit { font-size: 13px; }
datetimeedit { background: var(--field-alt); color: var(--text); background-end: var(--accent); border: 1px var(--border); border-radius: 4px; padding: 3px 6px; }
datetimeedit:focus { border-color: var(--accent); }
.groupbox { border: 1px var(--border); border-radius: 6px; padding: 8px 10px; gap: 6px; }
.grouptitle { color: var(--text-dim); font-size: 12px; }
groupbody { gap: 6px; }
.toolbar { flex-direction: row; gap: 2px; align-items: center; background: var(--surface); padding: 2px 4px; }
.toolsep { width: 1px; height: 18px; background: var(--border); margin: 0px 4px; }
.statusbar { flex-direction: row; gap: 12px; align-items: center; background: var(--surface); padding: 2px 8px; }
.statusmsg, .statusperm { font-size: 12px; color: var(--text-dim); }
.mainwindow { gap: 0px; }
.msgbody { gap: 14px; }
.msgtext { max-width: 420px; }
.msgicon { width: 26px; height: 26px; border-radius: 13px; text-align: center; padding: 3px 0px; color: var(--on-accent); background: var(--accent); }
.msgicon.warning { background: var(--warning); }
.msgicon.error { background: var(--danger); }
.wizard { min-width: 380px; }
.wizardpages { min-height: 120px; }
.wizardpage { gap: 8px; }
.popover { min-width: 120px; }
.toasthost { gap: 6px; align-items: center; }
toast {
  background: var(--tooltip-bg); color: var(--text); border: 1px var(--border); border-radius: 6px; padding: 6px 14px;
  box-shadow: 0px 4px 14px var(--shadow-card);
}
header { background: var(--surface); color: var(--text); border-color: var(--border); background-end: var(--accent); }
textedit {
  background: var(--field); color: var(--text); border: 1px var(--border); border-radius: 5px; padding: 5px 6px;
  background-end: var(--accent);
}
textedit:focus { border-color: var(--accent); }
richtext { color: var(--text); }
docktabs { color: var(--text); background-end: var(--accent); padding: 0px 4px; }
.dock-floating {
  background: var(--surface); border: 1px var(--border); border-radius: 6px;
  box-shadow: 0px 6px 18px var(--shadow-pop);
}
.fontlist { height: 110px; }
.fontpreview { padding: 6px 4px; }
.filelist { height: 220px; width: 420px; }
.grow { flex-grow: 1; }
.buttons { justify-content: end; }
.md-h1, .md-h2, .md-h3 { font-weight: bold; }
lineedit, textedit, codeedit { cursor: text; }
link { cursor: pointer; }
*:disabled { cursor: not-allowed; }
menuitem:disabled { color: var(--text-dim); }
toolbutton:disabled, menubutton:disabled, link:disabled, checkbox:disabled, radio:disabled, switch:disabled,
slider:disabled, scrollbar:disabled, dial:disabled, spinbox:disabled, combobox:disabled, lineedit:disabled { opacity: 0.45; }
.fx { transition: background 0.12s ease-out, border-color 0.12s ease-out, color 0.12s ease-out; }
.fx-fade { enter-animation: fade; enter-duration: 0.12s; exit-animation: fade; exit-duration: 0.1s; }
.fx-slide { enter-animation: slide-down; enter-duration: 0.14s; exit-animation: slide-up; exit-duration: 0.1s; }
completer { background: var(--surface); border: 1px var(--border); border-radius: 6px; padding: 3px; gap: 0px; background-end: var(--accent); }
completeritem { padding: 3px 8px; border-radius: 4px; color: var(--text); background-end: var(--accent); }
editcombo {
  background: var(--surface-hi); color: var(--text); border: 1px var(--border); border-radius: 5px; padding: 0px 6px 0px 0px;
  gap: 4px; align-items: center;
}
editcombo lineedit { border-width: 0px; background: transparent; }
combodrop { color: var(--text); cursor: pointer; }
.dialog-buttonbox { flex-direction: row; gap: 8px; }
.toolbox { gap: 2px; }
.toolbox-header { border-radius: 3px; padding: 4px 10px; background: var(--surface); }
.toolbox-header.open { background: var(--surface-hi); color: var(--heading); }
.toolbox-page { padding: 6px 8px; gap: 6px; }
lcdnumber { color: var(--lcd-text); background: var(--lcd-bg); border: 1px var(--border); border-radius: 4px; padding: 6px; }
keysequenceedit {
  background: var(--field); color: var(--text); border: 1px var(--border); border-radius: 5px; padding: 5px 8px;
}
keysequenceedit:focus { border-color: var(--accent); }
mdiarea { background: var(--workspace); border: 1px var(--border); border-radius: 4px; }
mdisubwindow {
  background: var(--surface); border: 1px var(--border); border-radius: 6px; gap: 0px;
  box-shadow: 0px 4px 12px var(--shadow-card);
}
mdisubwindow.active { border-color: var(--accent); }
mdititle { flex-direction: row; padding: 2px 4px 2px 8px; background: var(--surface-hi); align-items: center; color: var(--text); }
mdititle.mdigrip { background: transparent; padding: 0px; width: 14px; height: 14px; align-self: end; cursor: nwse-resize; }
.mdiclose { padding: 0px 6px; }
.mdibody { padding: 6px; }
graphicsview { background: var(--scene-bg); color: var(--text); border: 1px var(--border); border-radius: 4px; background-end: var(--accent); }
graphicsview:focus { border-color: var(--accent); }
vkeyboard { background: var(--surface); border: 1px var(--border); border-radius: 8px; padding: 6px; gap: 4px; }
.vkrow { flex-direction: row; gap: 4px; justify-content: center; }
.vkey { min-width: 22px; padding: 6px 6px; }
.vkey.vkwide { min-width: 52px; }
.vkey.vkspace { min-width: 160px; }
.vkey:checked { background: var(--accent); border-color: var(--accent); color: var(--on-accent); }
)css";

// Keyboard focus rings for the focusable classes that had none (borders are painted over the box: no layout change).
constexpr std::string_view kFocusRules = R"css(toolbutton:focus-visible, checkbox:focus-visible, radio:focus-visible, switch:focus-visible, dial:focus-visible,
slider:focus-visible, scrollbar:focus-visible, link:focus-visible, menubutton:focus-visible, docktabs:focus-visible,
header:focus-visible, splitter:focus-visible, tableview:focus-visible, treemodelview:focus-visible,
richtext:focus-visible, progressbar:focus-visible, minimap:focus-visible, kineticscroll:focus-visible {
  border: 1px var(--accent);
}
checkbox:focus-visible, radio:focus-visible, switch:focus-visible, dial:focus-visible, slider:focus-visible,
link:focus-visible, scrollbar:focus-visible { border-radius: 4px; }
tabbar:focus-visible { border: 1px var(--accent); border-radius: 4px; }
tableview:focus, treemodelview:focus, datetimeedit:focus-visible { border-color: var(--accent); }
button.primary:focus-visible { border-color: var(--primary-ring); }
)css";

// High contrast: every control outlined in the text colour, hover / selection in the highlight colours, 2px focus
// rings in the hot-light colour, no translucent scrim.
constexpr std::string_view kHighContrastRules = R"css(
button, toolbutton, lineedit, spinbox, combobox, listview, treeview, tabbar, menu, tooltip, toast, .card, gridview,
calendar, chart, colorpicker, datetimeedit, nodegraph, curveeditor, timeline, viewport, codeedit, video, textedit,
header, tableview, treemodelview, progressbar, .propgrid, .groupbox, .dock-floating, .toolbar, .statusbar, menubar,
completer, editcombo, lcdnumber, keysequenceedit, mdiarea, mdisubwindow, graphicsview, vkeyboard, .toolbox-header {
  border-width: 1px; border-color: var(--text);
}
editcombo lineedit { border-width: 0px; }
mdisubwindow { padding: 2px; }
mdisubwindow.active { border-width: 2px; border-color: var(--accent-hi); }
button:hover, toolbutton:hover, menubutton:hover, menubutton:focus-visible, menuitem:hover, menuitem:focus,
.propgroup:hover, button.primary, button.primary:hover, .vkey:checked, .toolbox-header.open {
  background: var(--accent); color: var(--on-accent);
}
button:disabled, toolbutton:disabled, menuitem:disabled { color: var(--text-dim); border-color: var(--text-dim); background: var(--bg); }
.msgicon, .msgicon.warning, .msgicon.error { background: var(--accent); color: var(--on-accent); }
.md-h3 { color: var(--text); }
toolbutton:disabled, menubutton:disabled, link:disabled, checkbox:disabled, radio:disabled, switch:disabled,
slider:disabled, scrollbar:disabled, dial:disabled, spinbox:disabled, combobox:disabled, lineedit:disabled {
  opacity: 1; color: var(--text-dim); border-color: var(--text-dim);
}
button:focus-visible, toolbutton:focus-visible, lineedit:focus, spinbox:focus-visible, combobox:focus-visible,
checkbox:focus-visible, radio:focus-visible, switch:focus-visible, dial:focus-visible, slider:focus-visible,
scrollbar:focus-visible, link:focus-visible, menubutton:focus-visible, menuitem:focus-visible, docktabs:focus-visible,
header:focus-visible, splitter:focus-visible, tableview:focus, treemodelview:focus, richtext:focus-visible,
tabbar:focus-visible, listview:focus, treeview:focus, gridview:focus, calendar:focus, chart:focus, colorpicker:focus,
nodegraph:focus, curveeditor:focus, timeline:focus, viewport:focus, codeedit:focus, video:focus, textedit:focus,
datetimeedit:focus, progressbar:focus-visible, minimap:focus-visible, kineticscroll:focus-visible,
keysequenceedit:focus, graphicsview:focus, button.primary:focus-visible {
  border-width: 2px; border-color: var(--accent-hi);
}
)css";

std::string hexOf(Color c, f32 k = 1.f) {
    const auto ch = [k](f32 v) { return unsigned(std::lround(std::clamp(v * k, 0.f, 1.f) * 255.f)); };
    char b[8];
    std::snprintf(b, sizeof b, "#%02x%02x%02x", ch(c.r), ch(c.g), ch(c.b));
    return std::string(b);
}
Color mix(Color a, Color b, f32 k) { return {a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k, a.b + (b.b - a.b) * k, 1.f}; }
std::string contrastValue(const TokenRow& t, const HighContrastPalette& p) {
    switch (t.hc) {
    case 'W': return hexOf(p.window);
    case 'T': return hexOf(p.windowText);
    case 'F': return hexOf(p.buttonFace);
    case 'H': return hexOf(p.highlight);
    case 'h': return hexOf(p.highlightText);
    case 'G': return hexOf(p.grayText);
    case 'L': return hexOf(p.hotlight);
    case 'S': return hexOf(mix(p.window, p.highlight, 0.35f));
    case 'D': return hexOf(mix(p.window, p.windowText, 0.25f));
    case '0': return "rgba(0, 0, 0, 0)";
    default: return std::string(t.dark);
    }
}
/// The value of one token under `d` (accent override applied).
std::string tokenValue(const TokenRow& t, const ThemeDesc& d) {
    if (d.variant == ThemeVariant::HighContrast) return contrastValue(t, d.contrast);
    if (d.hasAccent && t.name == "--accent") return hexOf(d.accent);
    if (d.hasAccent && t.name == "--accent-hi") return hexOf(d.accent, d.variant == ThemeVariant::Light ? 0.88f : 1.15f);
    return std::string(d.variant == ThemeVariant::Light ? t.light : t.dark);
}
std::string rootCss(const ThemeDesc& d, std::string_view tail) {
    std::string css = ":root {\n";
    css.reserve(4096);
    int col = 0;
    for (const TokenRow& t : kTokens) {
        css += col == 0 ? "  " : " ";
        css += t.name;
        css += ": ";
        css += tokenValue(t, d);
        css += ";";
        if (++col == 4) { css += "\n"; col = 0; }
    }
    if (col) css += "\n";
    css += tail;
    return css;
}
std::string fullCss(const ThemeDesc& d) {
    std::string css = rootCss(d, kRootTail);
    css += kBody;
    css += kFocusRules;
    if (d.variant == ThemeVariant::HighContrast) css += kHighContrastRules;
    return css;
}
} // namespace

std::string themeCss(const ThemeDesc& d) { return fullCss(d); }

std::string themeTokensCss(const ThemeDesc& d) {
    std::string css = rootCss(d, kOverrideTail);
    if (d.variant == ThemeVariant::HighContrast) css += kHighContrastRules;
    return css;
}

std::string_view themeCss(ThemeVariant v) {
    static const std::string dark = fullCss({.variant = ThemeVariant::Dark});
    static const std::string light = fullCss({.variant = ThemeVariant::Light});
    static const std::string contrast = fullCss({.variant = ThemeVariant::HighContrast});
    return v == ThemeVariant::Light ? std::string_view(light) : v == ThemeVariant::HighContrast ? std::string_view(contrast) : std::string_view(dark);
}
std::string_view defaultThemeCss() { return themeCss(ThemeVariant::Dark); }
std::string_view lightThemeCss() { return themeCss(ThemeVariant::Light); }
std::string highContrastThemeCss(const HighContrastPalette& p) {
    return fullCss({.variant = ThemeVariant::HighContrast, .contrast = p});
}

std::expected<void, UiError> setTheme(UiContext& ui, const ThemeDesc& d) {
    const bool plain = !d.hasAccent && (d.variant != ThemeVariant::HighContrast || d.contrast == HighContrastPalette{});
    auto r = plain ? ui.setThemeStyleSheet(themeCss(d.variant)) : ui.setThemeStyleSheet(fullCss(d));
    if (r) {
        // @media (prefers-color-scheme / prefers-contrast) of the application's own sheets follow the theme shown
        const Color& w = d.contrast.window;
        const bool dark = d.variant == ThemeVariant::Dark ||
                          (d.variant == ThemeVariant::HighContrast && 0.2126f * w.r + 0.7152f * w.g + 0.0722f * w.b < 0.5f);
        ui.setMediaTheme(dark, d.variant == ThemeVariant::HighContrast);
        ui.markAllDirty();
    }
    return r;
}

u32 themeTokenCount() { return u32(std::size(kTokens)); }
std::string_view themeTokenName(u32 index) { return index < std::size(kTokens) ? kTokens[index].name : std::string_view{}; }
bool themeTokenColor(const ThemeDesc& d, std::string_view name, Color& out) {
    for (const TokenRow& t : kTokens)
        if (t.name == name) {
            const std::string v = tokenValue(t, d);
            if (v.starts_with("var(") && v.size() > 5) return themeTokenColor(d, std::string_view(v).substr(4, v.size() - 5), out);
            return parseColor(v, out);
        }
    return false;
}

} // namespace fuse::ui::widgets
