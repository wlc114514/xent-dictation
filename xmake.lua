set_project("xent-dictation")
set_version("0.3.2")
set_xmakever("3.0.0")

-- Xent 0.3.2 的发布包是 llvm-mingw 构建的静态库（windows-gnu），只用公开头文件
set_allowedplats("mingw")
set_allowedarchs("x86_64", "arm64")
set_defaultplat("mingw")
set_defaultarchs("x86_64")

set_languages("c23")
set_warnings("all")
add_rules("mode.debug", "mode.release")
add_defines("_WIN32_WINNT=0x0A00", "WINVER=0x0A00")   -- GetDpiForWindow 等 Win10 接口
add_rules("plugin.compile_commands.autoupdate", {outputdir = "build"})   -- 给 clangd 用

-- 预编译的 Xent SDK：include/{xent,xtk,fluxent} + lib/libxent.a + 工具链缺的导入库
package("xent")
    set_homepage("https://github.com/Project-Xent/fluxent")
    set_description("Xent native UI toolkit (xent-core, xent-kit, fluxent), prebuilt for windows-gnu")
    set_license("0BSD")

    on_source(function (package)
        local arch = package:is_arch("arm64", "aarch64") and "arm64" or "x86_64"
        package:add("urls", "https://github.com/Project-Xent/fluxent/releases/download/v$(version)/xent-$(version)-" .. arch .. "-windows-gnu.zip")
        if arch == "arm64" then
            package:add("versions", "0.3.2", "4bcc5cad2393af87680280327c8cf9d1fdb1490fb29fe18aaeb8ab9d481ff9ca")
        else
            package:add("versions", "0.3.2", "5d43f340170bc3f7e91e45df72b228ffaff0849d939faddde96ce41aaa7920f1")
        end
    end)

    -- 顺序照 xent.pc：libxent.a 在前，系统库在后
    add_links("xent", "coremessaging", "uiautomationcore", "runtimeobject")
    add_syslinks("user32", "gdi32", "dcomp", "d2d1", "d3d11", "dxgi", "dwrite", "dwmapi", "ole32", "oleaut32",
                 "uuid", "uxtheme", "imm32", "advapi32", "shell32", "windowscodecs")

    on_install("mingw", function (package)
        os.cp("include", package:installdir())
        os.cp("lib/*.a", package:installdir("lib"))
    end)

    on_test(function (package)
        assert(package:has_cfuncs("flux_run", {includes = "fluxent/fluxent.h", configs = {languages = "c23"}}))
    end)
package_end()

add_requires("xent 0.3.2")

-- 后端：判分规则、词表读写、出题状态机，不依赖界面
target("dict_core")
    set_kind("static")
    set_default(false)
    add_files("src/dict_text.c", "src/dict_words.c", "src/dict_session.c")
    add_includedirs("src", {public = true})
target_end()

-- 应用本体
target("dictation")
    set_kind("binary")
    add_deps("dict_core")
    add_packages("xent")
    add_files("src/dictation.c", "src/dict_view.c", "src/dict_judge.c")
    add_syslinks("winhttp")
    add_ldflags("-mwindows", {force = true})
    set_rundir("$(projectdir)")   -- xmake run 在仓库根目录找 dictation_extra.json 等数据文件
    if is_mode("release") then
        add_cflags("-ffunction-sections", "-fdata-sections", {force = true})
        add_ldflags("-Wl,--gc-sections", "-Wl,--icf=safe", "-s", {force = true})
    end
target_end()

-- 无头布局自检：不开窗口，打印页面里每个节点的实际矩形（STAGE=5/8/12/13/14/15）
target("test_dict_layout")
    set_kind("binary")
    set_default(false)
    add_deps("dict_core")
    add_packages("xent")
    add_files("src/dictation.c", "src/dict_view.c", "src/dict_judge.c")
    add_defines("DICT_LAYOUT_TEST")
    add_syslinks("winhttp")
target_end()

-- 后端测试
target("test_dict_core")
    set_kind("binary")
    set_default(false)
    add_deps("dict_core")
    add_files("tests/test_dict_core.c")
    add_tests("default")
target_end()
