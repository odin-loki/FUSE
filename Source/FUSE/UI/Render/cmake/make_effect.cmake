# Script mode: cmake -DTEMPLATE=<ui_effect.comp.in> -DSNIPPET=<effect.glsl> -DOUT=<effect.comp> -P make_effect.cmake
# Splices an effect snippet into the effect pipeline template (see fuse_ui_effects.cmake).
file(READ "${TEMPLATE}" _template)
file(READ "${SNIPPET}" _snippet)
string(REPLACE "@FUSE_UI_EFFECT_SNIPPET@" "${_snippet}" _out "${_template}")
file(WRITE "${OUT}" "${_out}")
