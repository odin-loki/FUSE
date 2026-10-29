#include "fuse/ui/widgets/widgets.hpp"

namespace fuse::ui::widgets {

// Default dark theme. Tokens live in :root so apps restyle by overriding them. `background-end` doubles as the
// accent / indicator colour for widgets that draw one (no gradient set).
std::string_view defaultThemeCss() {
    return R"css(
:root {
  --bg: #1e1f24; --surface: #2a2b32; --surface-hi: #34353e; --border: #474955;
  --text: #e6e6eb; --text-dim: #9a9cab; --accent: #3d8bfd; --accent-hi: #5a9dff; --danger: #e5484d;
  background: var(--bg); color: var(--text); font-size: 14px;
}
panel { background: transparent; }
label { color: var(--text); }
button, toolbutton {
  background: var(--surface-hi); color: var(--text); border: 1px var(--border); border-radius: 6px;
  padding: 6px 14px; text-align: center;
}
toolbutton { padding: 4px 8px; border-radius: 4px; background: transparent; border-width: 0px; }
button:hover, toolbutton:hover { background: #40414c; }
button:pressed, toolbutton:pressed { background: #23242a; }
button:focus-visible, toolbutton:focus-visible, lineedit:focus, spinbox:focus-visible, combobox:focus-visible { border-color: var(--accent); }
button:disabled { color: var(--text-dim); background: var(--surface); }
button.primary { background: var(--accent); border-color: var(--accent); color: #ffffff; }
button.primary:hover { background: var(--accent-hi); }
checkbox, radio, switch, slider, dial, progressbar, scrollbar { background-end: var(--accent); }
checkbox, radio, switch { padding: 2px 0px; }
slider { height: 22px; }
progressbar { height: 8px; border-radius: 4px; }
lineedit, spinbox, combobox {
  background: #15161a; color: var(--text); border: 1px var(--border); border-radius: 5px; padding: 5px 8px;
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
  background: #15161a; color: var(--text); border: 1px var(--border); border-radius: 5px; padding: 3px;
  background-end: var(--accent);
}
listview:focus, treeview:focus, tabbar:focus-visible { border-color: var(--accent); }
tabbar { color: var(--text); background-end: var(--accent); }
popup.modal { background: rgba(0, 0, 0, 0.35); }
.dialog { min-width: 260px; }
tooltip {
  background: #0f1013; color: var(--text); border: 1px var(--border); border-radius: 4px; padding: 3px 7px;
  font-size: 12px;
}
.row { flex-direction: row; gap: 8px; align-items: center; }
.title { font-size: 20px; color: #ffffff; }
.dim { color: var(--text-dim); }
)css";
}

} // namespace fuse::ui::widgets
