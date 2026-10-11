-- 追加到 fluxent/xmake.lua 末尾；详见 README。
target("dictation")
    set_kind("binary")
    add_deps("fluxent")
    -- 前端 dictation.c + 后端 dict_*.c（examples/dictation/tests 下的测试不参与）
    add_files("examples/dictation/*.c")
    -- 自己装配运行时（见 dictation.c 的 app_run），要用到 src/bridge 的内部头
    add_includedirs("include", "src", "src/bridge", "examples/dictation")
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

-- 无头布局自检：不开窗口，打印视图里每个节点的实际矩形（STAGE=5/8/12/13）
target("test_dict_layout")
    set_default(false)
    set_kind("binary")
    add_deps("fluxent")
    add_files("examples/dictation/*.c")
    add_includedirs("include", "src", "src/bridge", "examples/dictation")
    add_defines("DICT_LAYOUT_TEST")
    if is_plat("windows") then
        add_cflags("/utf-8", { force = true })
        add_syslinks("winhttp")
    end
target_end()

-- 后端无头测试：判分规则、词表读写、出题状态机（普通模式 + 无尽模式）
target("test_dict_core")
    set_default(false)
    set_kind("binary")
    add_files("examples/dictation/tests/test_dict_core.c",
              "examples/dictation/dict_text.c",
              "examples/dictation/dict_words.c",
              "examples/dictation/dict_session.c")
    add_includedirs("examples/dictation")
    if is_plat("windows") then
        add_cflags("/utf-8", { force = true })
    end
target_end()
