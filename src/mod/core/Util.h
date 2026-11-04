#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace ib {

namespace fs = std::filesystem;

// 玩家可见文案统一带这个前缀
inline constexpr std::string_view kMsgPrefix = "§b[背包备份]§r ";

int64_t nowMs();

// 当天 00:00 的时间戳（毫秒），备份索引按天分组用
int64_t todayStartMs();

// 2026-11-04 13:20:11
std::string formatTime(int64_t ms);

// 2026-11-04
std::string formatDay(int64_t dayMs);

// uuid 太长，界面里只显示后 8 位
std::string shortUuid(std::string_view uuid);

std::string toLower(std::string_view s);

bool containsIgnoreCase(std::string_view haystack, std::string_view needle);

} // namespace ib
