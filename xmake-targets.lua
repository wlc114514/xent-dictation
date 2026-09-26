-- 追加到 fluxent/xmake.lua 末尾使用；用法见 README。
-- 前提：examples/dictation/ 已放进 fluxent 仓库（与本文件同级的 README 有说明）。

target("dictation")
    set_kind("binary")
    add_deps("fluxent")
    add_files("examples/dictation/*.c")
    add_includedirs("include", "examples/dictation")
    if is_plat("mingw") then
        add_cflags("-ffunction-sections", "-fdata-sections", { force = true })
        add_ldflags("-Wl,--subsystem,windows", "-Wl,--gc-sections", "-Wl,--icf=safe", { force = true })
    end
    if is_plat("windows") then
        -- 源码按 UTF-8 解释：程序里有中文字符串字面量
        add_cflags("/utf-8", { force = true })
        add_ldflags("/SUBSYSTEM:WINDOWS", "/OPT:REF", "/OPT:ICF", { force = true })
        add_syslinks("winhttp")
    end
target_end()

-- 无头布局自检：不开窗口，打印视图里每个节点的实际矩形
target("test_dict_layout")
    set_default(false)
    set_kind("binary")
    add_deps("fluxent")
    add_files("examples/dictation/dictation.c")
    add_includedirs("include", "src", "src/bridge", "examples/dictation")
    add_defines("DICT_LAYOUT_TEST")
    if is_plat("windows") then
        add_cflags("/utf-8", { force = true })
        add_syslinks("winhttp")
    end
target_end()
