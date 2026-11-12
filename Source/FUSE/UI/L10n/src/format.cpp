// Locale number and date formatting from a small built-in slice of CLDR 44 (latn digits, gregorian calendar).
// Rounding is half-even on the shortest round-trip decimal of the double, as ICU does.
#include "fuse/ui/l10n/l10n.hpp"

#include "l10n_internal.hpp"

#include <charconv>
#include <cmath>

namespace fuse::ui::l10n {

namespace {
#define NBSP "\xC2\xA0"          // U+00A0
#define NNBSP "\xE2\x80\xAF"     // U+202F narrow no-break space
#define MINUS "\xE2\x88\x92"     // U+2212
#define RLM "\xE2\x80\x8F"       // U+200F

struct NumData {
    std::string_view tag, decimal, group;
    int primary, secondary, minGrouping;
    std::string_view minus, pctPrefix, pctSuffix;
};
constexpr NumData kNum[] = {
    {"en", ".", ",", 3, 3, 1, "-", "", "%"},
    {"en-in", ".", ",", 3, 2, 1, "-", "", "%"},
    {"hi", ".", ",", 3, 2, 1, "-", "", "%"},
    {"de", ",", ".", 3, 3, 1, "-", "", NBSP "%"},
    {"de-at", ",", NBSP, 3, 3, 1, "-", "", NBSP "%"},
    {"de-ch", ".", "\xE2\x80\x99", 3, 3, 1, "-", "", "%"},
    {"fr", ",", NNBSP, 3, 3, 1, "-", "", NNBSP "%"},
    {"es", ",", ".", 3, 3, 2, "-", "", NBSP "%"},
    {"it", ",", ".", 3, 3, 1, "-", "", "%"},
    {"pt", ",", ".", 3, 3, 1, "-", "", "%"},
    {"pt-pt", ",", NBSP, 3, 3, 2, "-", "", "%"},
    {"nl", ",", ".", 3, 3, 1, "-", "", "%"},
    {"sv", ",", NBSP, 3, 3, 1, MINUS, "", NBSP "%"},
    {"fi", ",", NBSP, 3, 3, 1, MINUS, "", NBSP "%"},
    {"ru", ",", NBSP, 3, 3, 1, "-", "", NBSP "%"},
    {"uk", ",", NBSP, 3, 3, 1, "-", "", "%"},
    {"pl", ",", NBSP, 3, 3, 2, "-", "", "%"},
    {"cs", ",", NBSP, 3, 3, 1, "-", "", NBSP "%"},
    {"ar", ".", ",", 3, 3, 1, "-", "", "%"},
    {"he", ".", ",", 3, 3, 1, "-", "", "%"},
    {"ja", ".", ",", 3, 3, 1, "-", "", "%"},
    {"zh", ".", ",", 3, 3, 1, "-", "", "%"},
    {"ko", ".", ",", 3, 3, 1, "-", "", "%"},
    {"tr", ",", ".", 3, 3, 1, "-", "%", ""},
};

using Months = std::string_view[12];
struct DateData {
    std::string_view tag, longP, mediumP, shortP;
    Months wide, abbr;
};
constexpr DateData kDate[] = {
    {"en", "MMMM d, y", "MMM d, y", "M/d/yy",
     {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"},
     {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"}},
    {"en-gb", "d MMMM y", "d MMM y", "dd/MM/y",
     {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"},
     {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sept", "Oct", "Nov", "Dec"}},
    {"en-in", "d MMMM y", "d MMM y", "dd/MM/yy",
     {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"},
     {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sept", "Oct", "Nov", "Dec"}},
    {"de", "d. MMMM y", "dd.MM.y", "dd.MM.yy",
     {"Januar", "Februar", "März", "April", "Mai", "Juni", "Juli", "August", "September", "Oktober", "November", "Dezember"},
     {"Jan.", "Feb.", "März", "Apr.", "Mai", "Juni", "Juli", "Aug.", "Sept.", "Okt.", "Nov.", "Dez."}},
    {"fr", "d MMMM y", "d MMM y", "dd/MM/y",
     {"janvier", "février", "mars", "avril", "mai", "juin", "juillet", "août", "septembre", "octobre", "novembre", "décembre"},
     {"janv.", "févr.", "mars", "avr.", "mai", "juin", "juil.", "août", "sept.", "oct.", "nov.", "déc."}},
    {"es", "d 'de' MMMM 'de' y", "d MMM y", "d/M/yy",
     {"enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto", "septiembre", "octubre", "noviembre", "diciembre"},
     {"ene", "feb", "mar", "abr", "may", "jun", "jul", "ago", "sept", "oct", "nov", "dic"}},
    {"it", "d MMMM y", "d MMM y", "dd/MM/yy",
     {"gennaio", "febbraio", "marzo", "aprile", "maggio", "giugno", "luglio", "agosto", "settembre", "ottobre", "novembre", "dicembre"},
     {"gen", "feb", "mar", "apr", "mag", "giu", "lug", "ago", "set", "ott", "nov", "dic"}},
    {"pt", "d 'de' MMMM 'de' y", "d 'de' MMM 'de' y", "dd/MM/y",
     {"janeiro", "fevereiro", "março", "abril", "maio", "junho", "julho", "agosto", "setembro", "outubro", "novembro", "dezembro"},
     {"jan.", "fev.", "mar.", "abr.", "mai.", "jun.", "jul.", "ago.", "set.", "out.", "nov.", "dez."}},
    {"nl", "d MMMM y", "d MMM y", "dd-MM-y",
     {"januari", "februari", "maart", "april", "mei", "juni", "juli", "augustus", "september", "oktober", "november", "december"},
     {"jan", "feb", "mrt", "apr", "mei", "jun", "jul", "aug", "sep", "okt", "nov", "dec"}},
    {"sv", "d MMMM y", "d MMM y", "y-MM-dd",
     {"januari", "februari", "mars", "april", "maj", "juni", "juli", "augusti", "september", "oktober", "november", "december"},
     {"jan.", "feb.", "mars", "apr.", "maj", "juni", "juli", "aug.", "sep.", "okt.", "nov.", "dec."}},
    {"ru", "d MMMM y 'г'.", "d MMM y 'г'.", "dd.MM.y",
     {"января", "февраля", "марта", "апреля", "мая", "июня", "июля", "августа", "сентября", "октября", "ноября", "декабря"},
     {"янв.", "февр.", "мар.", "апр.", "мая", "июн.", "июл.", "авг.", "сент.", "окт.", "нояб.", "дек."}},
    {"pl", "d MMMM y", "d MMM y", "d.MM.y",
     {"stycznia", "lutego", "marca", "kwietnia", "maja", "czerwca", "lipca", "sierpnia", "września", "października", "listopada", "grudnia"},
     {"sty", "lut", "mar", "kwi", "maj", "cze", "lip", "sie", "wrz", "paź", "lis", "gru"}},
    {"tr", "d MMMM y", "d MMM y", "d.MM.y",
     {"Ocak", "Şubat", "Mart", "Nisan", "Mayıs", "Haziran", "Temmuz", "Ağustos", "Eylül", "Ekim", "Kasım", "Aralık"},
     {"Oca", "Şub", "Mar", "Nis", "May", "Haz", "Tem", "Ağu", "Eyl", "Eki", "Kas", "Ara"}},
    {"ja", "y年M月d日", "y/MM/dd", "y/MM/dd",
     {"1月", "2月", "3月", "4月", "5月", "6月", "7月", "8月", "9月", "10月", "11月", "12月"},
     {"1月", "2月", "3月", "4月", "5月", "6月", "7月", "8月", "9月", "10月", "11月", "12月"}},
    {"zh", "y年M月d日", "y年M月d日", "y/M/d",
     {"一月", "二月", "三月", "四月", "五月", "六月", "七月", "八月", "九月", "十月", "十一月", "十二月"},
     {"1月", "2月", "3月", "4月", "5月", "6月", "7月", "8月", "9月", "10月", "11月", "12月"}},
    {"ko", "y년 M월 d일", "y. M. d.", "yy. M. d.",
     {"1월", "2월", "3월", "4월", "5월", "6월", "7월", "8월", "9월", "10월", "11월", "12월"},
     {"1월", "2월", "3월", "4월", "5월", "6월", "7월", "8월", "9월", "10월", "11월", "12월"}},
    {"ar", "d MMMM y", "dd" RLM "/MM" RLM "/y", "d" RLM "/M" RLM "/y",
     {"يناير", "فبراير", "مارس", "أبريل", "مايو", "يونيو", "يوليو", "أغسطس", "سبتمبر", "أكتوبر", "نوفمبر", "ديسمبر"},
     {"يناير", "فبراير", "مارس", "أبريل", "مايو", "يونيو", "يوليو", "أغسطس", "سبتمبر", "أكتوبر", "نوفمبر", "ديسمبر"}},
};

// region entry ("de-ch"), then the language, then the first entry (en)
template <class T, std::size_t N> const T& lookup(const T (&table)[N], std::string_view locale) {
    const detail::Tag t = detail::splitTag(locale);
    if (!t.region.empty()) {
        const std::string full = t.lang + "-" + t.region;
        for (const T& e : table)
            if (e.tag == full) return e;
    }
    for (const T& e : table)
        if (e.tag == t.lang) return e;
    return table[0];
}

// shortest round-trip fixed decimal of |x|, rounded half-even to maxF fraction digits: "1234.5"
std::string roundedDigits(double x, int maxF) {
    char buf[400];
    const auto r = detail::toCharsFixed(buf, buf + sizeof buf, std::fabs(x));
    std::string s(buf, r.ec == std::errc{} ? std::size_t(r.ptr - buf) : 0);
    if (s.empty()) s = "0";
    const std::size_t dot = s.find('.');
    if (dot == s.npos) return s;
    const std::size_t keep = dot + 1 + std::size_t(maxF < 0 ? 0 : maxF);
    if (s.size() <= keep) return s;
    const char d = s[keep];
    bool up = d > '5';
    if (d == '5') {
        bool tail = false;
        for (std::size_t k = keep + 1; k < s.size(); ++k) tail |= s[k] != '0';
        const char last = s[keep - 1] == '.' ? s[keep - 2] : s[keep - 1];
        up = tail || ((last - '0') & 1);
    }
    s.resize(keep);
    if (s.back() == '.') s.pop_back();
    if (up) {
        std::size_t k = s.size();
        for (;;) {
            if (k == 0) { s.insert(s.begin(), '1'); break; }
            --k;
            if (s[k] == '.') continue;
            if (s[k] == '9') { s[k] = '0'; continue; }
            ++s[k];
            break;
        }
    }
    return s;
}

std::string formatAbs(const NumData& nd, double x, const NumberOptions& opt, bool& zero) {
    const std::string s = roundedDigits(x, opt.maxFraction);
    const std::size_t dot = s.find('.');
    const std::string ip = s.substr(0, dot);
    std::string fp = dot == s.npos ? std::string{} : s.substr(dot + 1);
    while (int(fp.size()) > opt.minFraction && !fp.empty() && fp.back() == '0') fp.pop_back();
    while (int(fp.size()) < opt.minFraction) fp += '0';
    zero = ip.find_first_not_of('0') == ip.npos && fp.find_first_not_of('0') == fp.npos;
    std::string out;
    const int len = int(ip.size());
    const bool group = opt.grouping && len >= nd.primary + nd.minGrouping;
    for (int k = 0; k < len; ++k) {
        out += ip[std::size_t(k)];
        const int rest = len - 1 - k; // digits after this one
        if (!group || rest == 0) continue;
        if (rest == nd.primary || (rest > nd.primary && (rest - nd.primary) % nd.secondary == 0)) out += nd.group;
    }
    if (!fp.empty()) {
        out += nd.decimal;
        out += fp;
    }
    return out;
}
} // namespace

std::string formatNumber(std::string_view locale, double x, NumberOptions opt) {
    if (std::isnan(x)) return "NaN";
    const NumData& nd = lookup(kNum, locale);
    if (std::isinf(x)) return std::string(x < 0 ? nd.minus : "") + "\xE2\x88\x9E";
    bool zero = false;
    std::string body = formatAbs(nd, x, opt, zero);
    return x < 0 && !zero ? std::string(nd.minus) + body : body;
}

std::string formatPercent(std::string_view locale, double fraction, int maxFraction) {
    const NumData& nd = lookup(kNum, locale);
    bool zero = false;
    const std::string body = formatAbs(nd, fraction * 100.0, {0, maxFraction, true}, zero);
    std::string out = fraction < 0 && !zero ? std::string(nd.minus) : std::string{};
    out += nd.pctPrefix;
    out += body;
    out += nd.pctSuffix;
    return out;
}

std::string_view decimalSeparator(std::string_view locale) { return lookup(kNum, locale).decimal; }
std::string_view groupingSeparator(std::string_view locale) { return lookup(kNum, locale).group; }

std::string_view monthName(std::string_view locale, int month, bool abbreviated) {
    if (month < 1 || month > 12) return {};
    const DateData& dd = lookup(kDate, locale);
    return abbreviated ? dd.abbr[month - 1] : dd.wide[month - 1];
}

std::string formatDate(std::string_view locale, Date d, DateStyle style) {
    const DateData& dd = lookup(kDate, locale);
    const std::string_view p = style == DateStyle::Long ? dd.longP : style == DateStyle::Medium ? dd.mediumP : dd.shortP;
    auto num = [](std::string& out, int v, std::size_t width) {
        std::string s = std::to_string(v < 0 ? -v : v);
        while (s.size() < width) s.insert(s.begin(), '0');
        if (v < 0) out += '-';
        out += s;
    };
    std::string out;
    for (std::size_t k = 0; k < p.size();) {
        const char c = p[k];
        if (c == '\'') { // 'literal', '' = apostrophe
            if (k + 1 < p.size() && p[k + 1] == '\'') { out += '\''; k += 2; continue; }
            const std::size_t e = p.find('\'', k + 1);
            out += p.substr(k + 1, (e == p.npos ? p.size() : e) - k - 1);
            k = e == p.npos ? p.size() : e + 1;
            continue;
        }
        if (c != 'y' && c != 'M' && c != 'd') { out += c; ++k; continue; }
        std::size_t n = 1;
        while (k + n < p.size() && p[k + n] == c) ++n;
        k += n;
        if (c == 'y') {
            if (n == 2) num(out, ((d.year % 100) + 100) % 100, 2);
            else num(out, d.year, n);
        } else if (c == 'd') {
            num(out, d.day, n);
        } else if (n >= 3) {
            out += monthName(locale, d.month, n == 3);
        } else {
            num(out, d.month, n);
        }
    }
    return out;
}

// ---- calendar names, time of day, currency -------------------------------------------------------------------
namespace {
using Days = std::string_view[7]; // Monday .. Sunday
struct CalData {
    std::string_view tag;
    int firstDay;               // ISO 1..7
    std::string_view monthYear; // CLDR yMMMM ('L' = stand-alone month)
    Days wide, shrt;
};
constexpr CalData kCal[] = {
    {"en", 7, "MMMM y", {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"},
     {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"}},
    {"en-gb", 1, "MMMM y", {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"},
     {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"}},
    {"en-in", 7, "MMMM y", {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"},
     {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"}},
    {"de", 1, "MMMM y", {"Montag", "Dienstag", "Mittwoch", "Donnerstag", "Freitag", "Samstag", "Sonntag"},
     {"Mo.", "Di.", "Mi.", "Do.", "Fr.", "Sa.", "So."}},
    {"fr", 1, "MMMM y", {"lundi", "mardi", "mercredi", "jeudi", "vendredi", "samedi", "dimanche"},
     {"lu", "ma", "me", "je", "ve", "sa", "di"}},
    {"es", 1, "MMMM 'de' y", {"lunes", "martes", "miércoles", "jueves", "viernes", "sábado", "domingo"},
     {"LU", "MA", "MI", "JU", "VI", "SA", "DO"}},
    {"it", 1, "MMMM y", {"lunedì", "martedì", "mercoledì", "giovedì", "venerdì", "sabato", "domenica"},
     {"lu", "ma", "me", "gi", "ve", "sa", "do"}},
    {"pt", 7, "MMMM 'de' y",
     {"segunda-feira", "terça-feira", "quarta-feira", "quinta-feira", "sexta-feira", "sábado", "domingo"},
     {"seg.", "ter.", "qua.", "qui.", "sex.", "sáb.", "dom."}},
    {"pt-pt", 1, "MMMM 'de' y",
     {"segunda-feira", "terça-feira", "quarta-feira", "quinta-feira", "sexta-feira", "sábado", "domingo"},
     {"seg.", "ter.", "qua.", "qui.", "sex.", "sáb.", "dom."}},
    {"nl", 1, "MMMM y", {"maandag", "dinsdag", "woensdag", "donderdag", "vrijdag", "zaterdag", "zondag"},
     {"ma", "di", "wo", "do", "vr", "za", "zo"}},
    {"sv", 1, "MMMM y", {"måndag", "tisdag", "onsdag", "torsdag", "fredag", "lördag", "söndag"},
     {"må", "ti", "on", "to", "fr", "lö", "sö"}},
    {"ru", 1, "LLLL y 'г'.", {"понедельник", "вторник", "среда", "четверг", "пятница", "суббота", "воскресенье"},
     {"пн", "вт", "ср", "чт", "пт", "сб", "вс"}},
    {"pl", 1, "LLLL y", {"poniedziałek", "wtorek", "środa", "czwartek", "piątek", "sobota", "niedziela"},
     {"pon", "wto", "śro", "czw", "pią", "sob", "nie"}},
    {"tr", 1, "MMMM y", {"Pazartesi", "Salı", "Çarşamba", "Perşembe", "Cuma", "Cumartesi", "Pazar"},
     {"Pt", "Sa", "Ça", "Pe", "Cu", "Ct", "Pa"}},
    {"ja", 7, "y年M月", {"月曜日", "火曜日", "水曜日", "木曜日", "金曜日", "土曜日", "日曜日"},
     {"月", "火", "水", "木", "金", "土", "日"}},
    {"zh", 7, "y年M月", {"星期一", "星期二", "星期三", "星期四", "星期五", "星期六", "星期日"},
     {"周一", "周二", "周三", "周四", "周五", "周六", "周日"}},
    {"ko", 7, "y년 MMMM", {"월요일", "화요일", "수요일", "목요일", "금요일", "토요일", "일요일"},
     {"월", "화", "수", "목", "금", "토", "일"}},
    {"ar", 6, "MMMM y", {"الاثنين", "الثلاثاء", "الأربعاء", "الخميس", "الجمعة", "السبت", "الأحد"},
     {"إثنين", "ثلاثاء", "أربعاء", "خميس", "جمعة", "سبت", "أحد"}},
};
struct Standalone {
    std::string_view tag;
    Months wide;
};
constexpr Standalone kStandalone[] = {
    {"ru", {"январь", "февраль", "март", "апрель", "май", "июнь", "июль", "август", "сентябрь", "октябрь", "ноябрь", "декабрь"}},
    {"pl", {"styczeń", "luty", "marzec", "kwiecień", "maj", "czerwiec", "lipiec", "sierpień", "wrzesień", "październik",
            "listopad", "grudzień"}},
};

struct TimeData {
    std::string_view tag, shortP, mediumP, am, pm;
};
constexpr TimeData kTime[] = {
    {"en", "h:mm" NNBSP "a", "h:mm:ss" NNBSP "a", "AM", "PM"},
    {"en-gb", "HH:mm", "HH:mm:ss", "am", "pm"},
    {"en-in", "h:mm" NNBSP "a", "h:mm:ss" NNBSP "a", "am", "pm"},
    {"hi", "h:mm" NNBSP "a", "h:mm:ss" NNBSP "a", "am", "pm"},
    {"de", "HH:mm", "HH:mm:ss", "AM", "PM"},
    {"fr", "HH:mm", "HH:mm:ss", "AM", "PM"},
    {"es", "H:mm", "H:mm:ss", "a." NBSP "m.", "p." NBSP "m."},
    {"it", "HH:mm", "HH:mm:ss", "AM", "PM"},
    {"pt", "HH:mm", "HH:mm:ss", "AM", "PM"},
    {"nl", "HH:mm", "HH:mm:ss", "a.m.", "p.m."},
    {"sv", "HH:mm", "HH:mm:ss", "fm", "em"},
    {"fi", "H.mm", "H.mm.ss", "ap.", "ip."},
    {"ru", "HH:mm", "HH:mm:ss", "AM", "PM"},
    {"uk", "HH:mm", "HH:mm:ss", "дп", "пп"},
    {"pl", "HH:mm", "HH:mm:ss", "AM", "PM"},
    {"cs", "H:mm", "H:mm:ss", "dop.", "odp."},
    {"tr", "HH:mm", "HH:mm:ss", "ÖÖ", "ÖS"},
    {"ja", "H:mm", "H:mm:ss", "午前", "午後"},
    {"zh", "HH:mm", "HH:mm:ss", "上午", "下午"},
    {"ko", "a h:mm", "a h:mm:ss", "오전", "오후"},
    {"ar", "h:mm" NBSP "a", "h:mm:ss" NBSP "a", "ص", "م"},
    {"he", "H:mm", "H:mm:ss", "לפנה״צ", "אחה״צ"},
};

void pad(std::string& out, int v, std::size_t width) {
    std::string s = std::to_string(v < 0 ? -v : v);
    while (s.size() < width) s.insert(s.begin(), '0');
    if (v < 0) out += '-';
    out += s;
}
} // namespace

std::string_view monthNameStandalone(std::string_view locale, int month) {
    if (month < 1 || month > 12) return {};
    const std::string lang = detail::splitTag(locale).lang;
    for (const Standalone& s : kStandalone)
        if (s.tag == lang) return s.wide[month - 1];
    return monthName(locale, month);
}

std::string formatMonthYear(std::string_view locale, int year, int month) {
    month = month < 1 ? 1 : month > 12 ? 12 : month;
    const std::string_view p = lookup(kCal, locale).monthYear;
    std::string out;
    for (std::size_t k = 0; k < p.size();) {
        const char c = p[k];
        if (c == '\'') {
            const std::size_t e = p.find('\'', k + 1);
            out += p.substr(k + 1, (e == p.npos ? p.size() : e) - k - 1);
            k = e == p.npos ? p.size() : e + 1;
            continue;
        }
        if (c != 'y' && c != 'M' && c != 'L') { out += c; ++k; continue; }
        std::size_t n = 1;
        while (k + n < p.size() && p[k + n] == c) ++n;
        k += n;
        if (c == 'y') pad(out, year, 1);
        else if (n < 3) pad(out, month, n);
        else out += c == 'L' ? monthNameStandalone(locale, month) : monthName(locale, month, n == 3);
    }
    return out;
}

std::string_view weekdayName(std::string_view locale, int isoWeekday, DayWidth width) {
    if (isoWeekday < 1 || isoWeekday > 7) return {};
    const CalData& c = lookup(kCal, locale);
    return width == DayWidth::Short ? c.shrt[isoWeekday - 1] : c.wide[isoWeekday - 1];
}

int firstDayOfWeek(std::string_view locale) { return lookup(kCal, locale).firstDay; }

bool uses24HourClock(std::string_view locale) { return lookup(kTime, locale).shortP.find('h') == std::string_view::npos; }

std::string_view dayPeriodName(std::string_view locale, bool pm) {
    const TimeData& t = lookup(kTime, locale);
    return pm ? t.pm : t.am;
}

std::string formatTime(std::string_view locale, Time t, TimeStyle style, HourCycle cycle) {
    const TimeData& td = lookup(kTime, locale);
    std::string_view p = style == TimeStyle::Medium ? td.mediumP : td.shortP;
    const bool h12 = p.find('h') != p.npos;
    if (cycle == HourCycle::H12 && !h12) p = style == TimeStyle::Medium ? "h:mm:ss a" : "h:mm a";
    if (cycle == HourCycle::H23 && h12) p = style == TimeStyle::Medium ? "HH:mm:ss" : "HH:mm";
    const int hour = ((t.hour % 24) + 24) % 24;
    std::string out;
    for (std::size_t k = 0; k < p.size();) {
        const char c = p[k];
        if (c != 'H' && c != 'h' && c != 'm' && c != 's' && c != 'a') { out += c; ++k; continue; }
        std::size_t n = 1;
        while (k + n < p.size() && p[k + n] == c) ++n;
        k += n;
        switch (c) {
        case 'H': pad(out, hour, n); break;
        case 'h': pad(out, hour % 12 == 0 ? 12 : hour % 12, n); break;
        case 'm': pad(out, t.minute, n); break;
        case 's': pad(out, t.second, n); break;
        default: out += hour >= 12 ? td.pm : td.am; break;
        }
    }
    return out;
}

namespace {
struct CurData {
    std::string_view tag;
    bool prefix;            // symbol before the number
    std::string_view space; // between symbol and number
    int negative;           // 0 "-¤n" / "-n ¤", 1 "¤ -n" (nl), 2 "¤-n" (de-CH)
    std::string_view lead;  // before everything (ar / he: RLM)
};
constexpr CurData kCur[] = {
    {"en", true, "", 0, ""},      {"en-gb", true, "", 0, ""},   {"en-in", true, "", 0, ""},
    {"hi", true, "", 0, ""},      {"de", false, NBSP, 0, ""},   {"de-at", true, NBSP, 0, ""},
    {"de-ch", true, NBSP, 2, ""}, {"fr", false, NBSP, 0, ""},   {"es", false, NBSP, 0, ""},
    {"it", false, NBSP, 0, ""},   {"pt", true, NBSP, 0, ""},    {"pt-pt", false, NBSP, 0, ""},
    {"nl", true, NBSP, 1, ""},    {"sv", false, NBSP, 0, ""},   {"fi", false, NBSP, 0, ""},
    {"ru", false, NBSP, 0, ""},   {"uk", false, NBSP, 0, ""},   {"pl", false, NBSP, 0, ""},
    {"cs", false, NBSP, 0, ""},   {"tr", true, "", 0, ""},      {"ja", true, "", 0, ""},
    {"zh", true, "", 0, ""},      {"ko", true, "", 0, ""},      {"ar", false, NBSP, 0, RLM},
    {"he", false, NBSP RLM, 0, RLM},
};
struct Sym {
    std::string_view loc, code, sym; // loc "" = the CLDR root symbol
};
constexpr Sym kSym[] = {
    {"", "USD", "US$"},     {"", "EUR", "€"},       {"", "GBP", "£"},        {"", "JPY", "JP¥"},
    {"", "CNY", "CN¥"},     {"", "INR", "₹"},       {"", "KRW", "₩"},        {"", "BRL", "R$"},
    {"", "CAD", "CA$"},     {"", "AUD", "A$"},      {"", "MXN", "MX$"},      {"", "ILS", "₪"},
    {"", "TWD", "NT$"},     {"", "HKD", "HK$"},     {"", "NZD", "NZ$"},
    {"en", "USD", "$"},     {"en", "JPY", "¥"},     {"en-gb", "USD", "US$"}, {"hi", "USD", "$"},
    {"de", "USD", "$"},     {"de", "JPY", "¥"},     {"fr", "USD", "$US"},    {"fr", "JPY", "JPY"},
    {"fr", "GBP", "£GB"},   {"es", "JPY", "JPY"},   {"it", "USD", "USD"},    {"it", "JPY", "JPY"},
    {"sv", "SEK", "kr"},    {"ru", "RUB", "₽"},     {"ru", "USD", "$"},      {"ru", "JPY", "¥"},
    {"uk", "UAH", "₴"},     {"pl", "PLN", "zł"},    {"pl", "USD", "USD"},    {"cs", "CZK", "Kč"},
    {"tr", "TRY", "₺"},     {"tr", "USD", "$"},     {"ja", "JPY", "￥"},     {"ja", "USD", "$"},
    {"ja", "CNY", "元"},    {"zh", "CNY", "¥"},     {"ar", "EGP", "ج.م." RLM}, {"ar", "SAR", "ر.س." RLM},
};
bool isAsciiLetter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
} // namespace

int currencyDigits(std::string_view code) {
    for (std::string_view z : {"JPY", "KRW", "CLP", "ISK", "VND", "UGX", "PYG", "XAF", "XOF"})
        if (code == z) return 0;
    for (std::string_view t : {"KWD", "BHD", "OMR", "JOD", "TND", "LYD", "IQD"})
        if (code == t) return 3;
    return 2;
}

std::string_view currencySymbol(std::string_view locale, std::string_view code) {
    const detail::Tag t = detail::splitTag(locale);
    if (!t.region.empty()) {
        const std::string full = t.lang + "-" + t.region;
        for (const Sym& s : kSym)
            if (s.loc == full && s.code == code) return s.sym;
    }
    for (const Sym& s : kSym)
        if (!s.loc.empty() && s.loc == t.lang && s.code == code) return s.sym;
    for (const Sym& s : kSym)
        if (s.loc.empty() && s.code == code) return s.sym;
    return code;
}

std::string formatCurrency(std::string_view locale, double amount, std::string_view code, int fraction) {
    if (std::isnan(amount)) return "NaN";
    const NumData& nd = lookup(kNum, locale);
    const CurData& cd = lookup(kCur, locale);
    const int digits = fraction >= 0 ? fraction : currencyDigits(code);
    bool zero = false;
    const std::string body = formatAbs(nd, amount, {digits, digits, true}, zero);
    const bool neg = amount < 0 && !zero;
    const std::string_view sym = currencySymbol(locale, code);
    // CLDR currency spacing: a letter beside the digits gets a no-break space when the pattern has none
    std::string_view gap = cd.space;
    if (gap.empty() && !sym.empty() && isAsciiLetter(cd.prefix ? sym.back() : sym.front())) gap = NBSP;
    std::string out(cd.lead);
    if (cd.prefix) {
        if (neg && cd.negative == 0) out += nd.minus;
        out += sym;
        if (neg && cd.negative == 2) {
            out += nd.minus;
        } else {
            out += gap;
            if (neg && cd.negative == 1) out += nd.minus;
        }
        out += body;
    } else {
        if (neg) out += nd.minus;
        out += body;
        out += gap;
        out += sym;
    }
    return out;
}

} // namespace fuse::ui::l10n
