@echo off
rem 单词默写启动脚本：放在数据文件（dictation_extra.json 等）所在目录里运行。
rem XENTHON 改成你编译好的 xenthon\src 目录，DICTATION 改成本仓库目录。
set "XENTHON=%~dp0xent-latest\xenthon\src"
set "DICTATION=%~dp0xent-dictation"
set "PYTHONPATH=%XENTHON%;%DICTATION%"
cd /d "%~dp0"
start "" pythonw -m dictation
