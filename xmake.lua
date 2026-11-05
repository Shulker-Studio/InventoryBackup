add_rules("mode.debug", "mode.release")

add_repositories("levimc-repo https://github.com/LiteLDev/xmake-repo.git")

-- 背包备份是纯服务端模组
-- 26.60.1 起 LevelStorage::getServerId 恢复为 MCAPI，26.60.0 会链接失败，所以下限卡 26.60.1
add_requires("levilamina >=26.60.1 <26.70.0", {configs = {target_type = "server"}})
add_requires("levibuildscript")
-- 索引 / 暂存 / 导入导出都是 JSON
add_requires("nlohmann_json 3.12.0")
-- 备份文件用 LeviLamina 的 compress_utils（gzip）压缩，底层 zlib
add_requires("zlib 1.3.2")

if not has_config("vs_runtime") then
    set_runtimes("MD")
end

target("InventoryBackup")
    add_rules("@levibuildscript/linkrule")
    -- 版本号只有一个来源：tooth.json（on_load 里读）加 git 短 hash；modpacker 与 Version.h 都吃这一份
    if is_plat("windows") then
        add_defines("NOMINMAX", "UNICODE", "_HAS_CXX23=1")
        set_exceptions("none") -- To avoid conflicts with /EHa.
        add_cxflags("/EHa", "/utf-8", "/W4", "/w44265", "/w44289", "/w44296", "/w45263", "/w44738", "/w45204")
        add_cxflags(
            "/EHs",
            "-Wno-microsoft-cast",
            "-Wno-invalid-offsetof",
            "-Wno-c++2b-extensions",
            "-Wno-microsoft-include",
            "-Wno-overloaded-virtual",
            "-Wno-ignored-qualifiers",
            "-Wno-missing-field-initializers",
            "-Wno-potentially-evaluated-expression",
            "-Wno-pragma-system-header-outside-header",
            {tools = {"clang_cl"}}
        )
        set_toolchains("clang-cl")
    end
    add_defines("LL_PLAT_S")
    add_packages("levilamina", "nlohmann_json", "zlib")
    set_kind("shared")
    set_languages("c++20")
    set_symbols("debug")
    add_headerfiles("src/**.h")
    add_files("src/**.cpp")
    add_files("src/mod/InventoryBackup.rc")
    set_configdir("$(builddir)/config")
    add_configfiles("src/(mod/Version.h.in)")
    add_includedirs("src", "$(builddir)/config")

    -- tooth.json 是版本号的唯一来源；带 +<短 hash> 方便定位到具体提交
    on_load(function (target)
        import("core.base.json")

        local version = json.loadfile("tooth.json")["version"]
        if type(version) ~= "string" or not version:match("^%d+%.%d+%.%d+$") then
            raise("tooth.json version must use major.minor.patch format")
        end

        local major, minor, patch = version:match("^(%d+)%.(%d+)%.(%d+)$")
        local hash = os.iorun("git rev-parse --short HEAD"):gsub("\n", "")
        local versionString = version .. "+" .. hash

        target:set("version", version)
        target:set("configvar", "IB_VERSION_MAJOR", major)
        target:set("configvar", "IB_VERSION_MINOR", minor)
        target:set("configvar", "IB_VERSION_PATCH", patch)
        target:set("configvar", "IB_VERSION_STRING", versionString)
        target:add("rules", "@levibuildscript/modpacker", {modVersion = versionString})
    end)

    -- modpacker 只搬 dll / pdb / manifest，语言文件得自己拷进产物目录
    after_build(function (target)
        if not os.isdir("lang") then
            return
        end
        local outdir = path.join(os.projectdir(), "bin", target:name(), "lang")
        os.mkdir(outdir)
        for _, file in ipairs(os.files("lang/*.json")) do
            os.cp(file, path.join(outdir, path.filename(file)))
        end
    end)
