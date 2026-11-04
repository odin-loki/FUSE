// fuse_ui_l10n gate: string-extraction input (scanned, not compiled)
#include "game.hpp"

void menu(StringTable& st, const Args& args, std::string variable) {
    label(st.tr("menu.title"));
    label(st.tr("Hello, \"world\"\n"));
    label(st.tr("adjacent " "literals"
                " joined", {{"n", 1}}));
    label(st.tr(u8"café"));
    label(st.tr(R"(raw {n, plural, one {# item} other {# items}})", args));
    // st.tr("commented.out")
    const char* s = "tr(\"in.a.string\")";
    label(st.tr(variable));
    label(st.tr("a" + std::string("b")));
    label(str("not.tr"));
    int n = 1'000; char q = '"'; label(st.tr("after.quote"));
    /* tr("block.comment") */
    label(st.tr("menu.title"));
}
