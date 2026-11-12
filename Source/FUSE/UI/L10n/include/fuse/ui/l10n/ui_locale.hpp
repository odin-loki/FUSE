// Header-only glue between fuse_ui_l10n and a UiContext: widgets (the calendar's month header, weekday names and
// first day of the week) format with the l10n tables. Include it where the app sets its locale and link
// fuse_ui_l10n_lib; core and widgets stay free of locale tables otherwise.
//
//   fuse::ui::l10n::setUiLocale(ui, "de");   // calendar: "März 2024", Mo. Di. ..., Monday first
//   fuse::ui::l10n::setUiLocale(ui, "en-US"); // "March 2024", Su Mo ..., Sunday first
#pragma once

#include "fuse/ui/l10n/l10n.hpp"
#include "fuse/ui/ui.hpp"

namespace fuse::ui::l10n {

inline const LocaleHooks& uiLocaleHooks() {
    static constexpr LocaleHooks hooks{
        [](std::string_view loc, int year, int month) { return formatMonthYear(loc, year, month); },
        [](std::string_view loc, int iso) { return weekdayName(loc, iso, DayWidth::Short); },
        [](std::string_view loc) { return firstDayOfWeek(loc); },
    };
    return hooks;
}

/// Sets the context locale with the l10n data (an empty tag: English, Monday first).
inline void setUiLocale(UiContext& ui, std::string_view tag) { ui.setLocale(tag, &uiLocaleHooks()); }

} // namespace fuse::ui::l10n
