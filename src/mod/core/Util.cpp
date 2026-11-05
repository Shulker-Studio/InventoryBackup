#include "mod/core/Util.h"

#include <chrono>
#include <ctime>
#include <format>

#include "mod/i18n/Text.h"

namespace ib {

std::string msgPrefix() { return msg("backup.common.prefix"); }

std::string formatPath(fs::path const& path) { return path.lexically_normal().generic_string(); }

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()
    )
        .count();
}

namespace {

// 用 UCRT 的 localtime_s，而不是 std::chrono::current_zone() / zoned_time：
// MSVC 的时区库（tzdb）实现在 MSVCP140_ATOMIC_WAIT.dll 里，而那个 DLL 只随较新的
// vcredist 分发，老服务器上缺它会让模组直接加载失败（错误码 126）。
// 我们只需要本地时间和日期，走 CRT 就够了。
std::tm localTm(int64_t ms) {
    auto const secs = static_cast<std::time_t>(ms / 1000);
    std::tm    tm{};
#if defined(_WIN32)
    localtime_s(&tm, &secs);
#else
    localtime_r(&secs, &tm);
#endif
    return tm;
}

} // namespace

int64_t todayStartMs() {
    std::tm day = localTm(nowMs());
    day.tm_hour = 0;
    day.tm_min  = 0;
    day.tm_sec  = 0;
    day.tm_isdst = -1; // 交给 mktime 判断夏令时
    return static_cast<int64_t>(std::mktime(&day)) * 1000;
}

std::string formatTime(int64_t ms) {
    auto const tm = localTm(ms);
    return std::format(
        "{:04}-{:02}-{:02} {:02}:{:02}:{:02}",
        tm.tm_year + 1900,
        tm.tm_mon + 1,
        tm.tm_mday,
        tm.tm_hour,
        tm.tm_min,
        tm.tm_sec
    );
}

std::string formatDay(int64_t ms) {
    auto const tm = localTm(ms);
    return std::format("{:04}-{:02}-{:02}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

std::string shortUuid(std::string_view uuid) {
    if (uuid.size() <= 8) return std::string{uuid};
    return std::string{uuid.substr(uuid.size() - 8)};
}

std::string toLower(std::string_view s) {
    std::string out{s};
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool containsIgnoreCase(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    return toLower(haystack).find(toLower(needle)) != std::string::npos;
}

} // namespace ib
