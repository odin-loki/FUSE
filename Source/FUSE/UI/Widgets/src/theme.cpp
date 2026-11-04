#include "fuse/ui/widgets/widgets.hpp"

#include <string>

namespace fuse::ui::widgets {

// Default theme: the colour tokens live in :root (dark and light variants share one rule body), so apps restyle by
// overriding tokens. `background-end` doubles as the accent / indicator colour for widgets that draw one.
namespace {
constexpr std::string_view kDarkRoot = R"css(
:root {
  --bg: #1e1f24; --surface: #2a2b32; --surface-hi: #34353e; --border: #474955;
  --text: #e6e6eb; --text-dim: #9a9cab; --accent: #3d8bfd; --accent-hi: #5a9dff; --danger: #e5484d;
  --hover: #40414c; --pressed: #23242a; --on-accent: #ffffff; --heading: #ffffff; --field: #15161a;
  --tooltip-bg: #0f1013; --code-bg: #101114; --code-text: #b8e0a0; --quote-bg: #23242b; --canvas: #17171c;
  --field-alt: #1e1f25;
  background: var(--bg); color: var(--text); font-size: 14px;
}
)css";
constexpr std::string_view kLightRoot = R"css(
:root {
  --bg: #f3f3f5; --surface: #ffffff; --surface-hi: #e7e8ed; --border: #c3c5cf;
  --text: #1c1d21; --text-dim: #5d606e; --accent: #0a64d8; --accent-hi: #0957bd; --danger: #c62f35;
  --hover: #dcdde4; --pressed: #c8c9d2; --on-accent: #ffffff; --heading: #0e0f12; --field: #ffffff;
  --tooltip-bg: #fbfbfd; --code-bg: #eceef2; --code-text: #2c6a1c; --quote-bg: #e6e7ec; --canvas: #fafafb;
  --field-alt: #ffffff;
  background: var(--bg); color: var(--text); font-size: 14px;
}
)css";
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
toolbutton:disabled, menuitem:disabled, icon:disabled { color: var(--text-dim); }
checkbox:disabled, radio:disabled, switch:disabled, slider:disabled, dial:disabled, scrollbar:disabled,
spinbox:disabled, combobox:disabled, lineedit:disabled, link:disabled, menubutton:disabled, listview:disabled,
treeview:disabled, tabbar:disabled, tableview:disabled, gridview:disabled, colorpicker:disabled, calendar:disabled {
  opacity: 0.45;
}
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
  box-shadow: 0px 4px 14px rgba(0, 0, 0, 0.45);
}
menu {
  background: var(--surface); border: 1px var(--border); border-radius: 6px; padding: 4px; gap: 0px;
  box-shadow: 0px 6px 18px rgba(0, 0, 0, 0.5);
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
popup.modal { background: rgba(0, 0, 0, 0.35); }
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
.fontlist { height: 110px; }
.fontpreview { padding: 6px 4px; }
.filelist { height: 220px; width: 420px; }
.grow { flex-grow: 1; }
.buttons { justify-content: end; }
)css";
} // namespace

std::string_view defaultThemeCss() { return themeCss(ThemeVariant::Dark); }
std::string_view lightThemeCss() { return themeCss(ThemeVariant::Light); }
std::string_view themeCss(ThemeVariant v) {
    static const std::string dark = std::string(kDarkRoot) + std::string(kBody);
    static const std::string light = std::string(kLightRoot) + std::string(kBody);
    return v == ThemeVariant::Light ? std::string_view(light) : std::string_view(dark);
}

} // namespace fuse::ui::widgets
