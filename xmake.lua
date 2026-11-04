add_rules("mode.debug", "mode.release")

add_repositories("levimc-repo https://github.com/LiteLDev/xmake-repo.git")

-- 背包备份是纯服务端模组
add_requires("levilamina 26.51.9", {configs = {target_type = "server"}})
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
    -- 版本号不写死：modpacker 会用 `git describe --tags --abbrev=0 --always` 取当前 tag（发布前打 v26.51.0）
    add_rules("@levibuildscript/modpacker")
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
    add_includedirs("src")

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
