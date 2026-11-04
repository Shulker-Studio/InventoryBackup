#include "mod/core/Util.h"

#include <chrono>
#include <format>

namespace ib {

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()
    )
        .count();
}

namespace {

std::chrono::zoned_time<std::chrono::system_clock::duration> localTimeOf(int64_t ms) {
    auto const timePoint = std::chrono::system_clock::time_point{std::chrono::milliseconds{ms}};
    return std::chrono::zoned_time{std::chrono::current_zone(), timePoint};
}

} // namespace

int64_t todayStartMs() {
    auto const zoned = localTimeOf(nowMs());
    auto const day   = std::chrono::floor<std::chrono::days>(zoned.get_local_time());
    auto const start = std::chrono::zoned_time{zoned.get_time_zone(), std::chrono::local_days{day}};
    return std::chrono::duration_cast<std::chrono::milliseconds>(start.get_sys_time().time_since_epoch()).count();
}

std::string formatTime(int64_t ms) { return std::format("{:%Y-%m-%d %H:%M:%S}", localTimeOf(ms)); }

std::string formatDay(int64_t ms) { return std::format("{:%Y-%m-%d}", localTimeOf(ms)); }

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
