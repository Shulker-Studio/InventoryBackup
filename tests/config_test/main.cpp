// 配置迁移的离线自测：只编译 ConfigMigration.cpp，不链接 LeviLamina。
// 跑法：xmake build config_test && ./bin/config_test
#include <cstdio>
#include <string>
#include <vector>

#include "mod/config/Config.h"

namespace {

int gFailed = 0;

void check(bool ok, char const* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++gFailed;
}

void checkEqual(long long actual, long long expected, char const* what) {
    bool ok = actual == expected;
    std::printf("  [%s] %s（实际 %lld，期望 %lld）\n", ok ? "PASS" : "FAIL", what, actual, expected);
    if (!ok) ++gFailed;
}

void caseMissingVersionAddsPageSize() {
    std::printf("\n1. v1 老配置（无 version / 无 pageSize）\n");
    auto const text = R"({
    "backupIntervalMinutes": 5,
    "maxKeepDays": 30,
    "enableIntervalBackup": true,
    "enableLeaveBackup": false,
    "enableDeathBackup": true,
    "skipSimulatedPlayers": true
})";
    auto result = ib::migrateConfig(text);
    check(result.error.empty(), "不报错");
    checkEqual(result.fromVersion, 1, "识别为 v1");
    checkEqual(result.config.pageSize, 10, "补上 pageSize=10");
    checkEqual(result.config.backupIntervalMinutes, 5, "用户写的间隔保留");
    checkEqual(result.config.maxKeepDays, 30, "用户写的保留天数保留");
    checkEqual(result.config.enableDeathBackup ? 1 : 0, 1, "用户写的开关保留");
    check(result.rewritten.find("\"version\": 2") != std::string::npos, "回写内容版本升到 2");
    check(result.rewritten.find("\"pageSize\": 10") != std::string::npos, "回写内容含 pageSize");
}

void caseExplicitV1() {
    std::printf("\n2. 显式 version: 1\n");
    auto result = ib::migrateConfig(R"({"version": 1, "pageSize": 25, "maxKeepDays": 7})");
    check(result.error.empty(), "不报错");
    check(result.rewritten.find("\"pageSize\": 25") != std::string::npos, "已有 pageSize 不被默认值覆盖");
    checkEqual(result.config.maxKeepDays, 7, "其它键保留");
}

void caseCurrentVersionNoRewrite() {
    std::printf("\n3. 当前版本配置\n");
    auto const text = R"({
    "version": 2,
    "backupIntervalMinutes": 15,
    "maxKeepDays": 90,
    "enableIntervalBackup": true,
    "enableLeaveBackup": true,
    "enableDeathBackup": false,
    "skipSimulatedPlayers": true,
    "pageSize": 20
})";
    auto result = ib::migrateConfig(text);
    check(result.error.empty(), "不报错");
    check(result.rewritten.empty(), "不需要回写");
    checkEqual(result.config.pageSize, 20, "读到 pageSize=20");
}

void caseEmptyText() {
    std::printf("\n4. 空文本（首次生成）\n");
    auto result = ib::migrateConfig("");
    check(result.error.empty(), "不报错");
    check(!result.rewritten.empty(), "给出默认配置内容");
    checkEqual(result.config.pageSize, 10, "默认 pageSize=10");
    checkEqual(result.config.backupIntervalMinutes, 15, "默认间隔 15");
}

void caseComments() {
    std::printf("\n5. 带 // 注释\n");
    auto result = ib::migrateConfig(R"({
    // 注释
    "version": 2,
    "pageSize": 8
})");
    check(result.error.empty(), "允许注释");
    checkEqual(result.config.pageSize, 8, "读到 pageSize=8");
}

void caseInvalidValues() {
    std::printf("\n6. 非法取值\n");
    check(!ib::migrateConfig(R"({"version": 2, "pageSize": 0})").error.empty(), "pageSize=0 被拒");
    check(!ib::migrateConfig(R"({"version": 2, "pageSize": 74})").error.empty(), "pageSize=74 被拒");
    check(!ib::migrateConfig(R"({"version": 2, "backupIntervalMinutes": 0})").error.empty(), "间隔 0 被拒");
    check(!ib::migrateConfig(R"({"version": 2, "maxKeepDays": -3})").error.empty(), "负数天数被拒");
}

void caseBadJson() {
    std::printf("\n7. 坏的 JSON / 根节点不对\n");
    check(!ib::migrateConfig("{ this is not json ").error.empty(), "语法错误被拒");
    check(!ib::migrateConfig("[1, 2, 3]").error.empty(), "数组根节点被拒");
}

void caseNewerVersion() {
    std::printf("\n8. 文件版本比插件新\n");
    auto result = ib::migrateConfig(R"({"version": 99, "pageSize": 12, "unknownKey": "x"})");
    check(result.error.empty(), "不报错");
    check(result.newer, "标记为版本更新");
    check(result.rewritten.empty(), "绝不覆盖用户文件");
    checkEqual(result.config.pageSize, 12, "认识的键照样读");
}

} // namespace

int main() {
    std::printf("配置迁移离线测试（当前配置版本 v%d）\n", ib::kConfigVersion);
    caseMissingVersionAddsPageSize();
    caseExplicitV1();
    caseCurrentVersionNoRewrite();
    caseEmptyText();
    caseComments();
    caseInvalidValues();
    caseBadJson();
    caseNewerVersion();
    std::printf("\n%s（失败 %d 项）\n", gFailed == 0 ? "全部通过" : "存在失败", gFailed);
    return gFailed == 0 ? 0 : 1;
}
