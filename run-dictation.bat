@echo off
rem 单词默写（Project-Xent / fluxent 前端）启动脚本
rem
rem 注意：程序按「当前工作目录」找数据文件（dictation_extra.json / dictation_judge.json），
rem 所以本脚本要放在数据文件所在目录里运行。
rem 下面的路径改成你自己编译出来的 dictation.exe 即可。
cd /d "%~dp0"
start "" "xent\fluxent\build\windows\x64\release\dictation.exe"
