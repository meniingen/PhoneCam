@echo off
chcp 65001
set OPEN_TERMINAL_API_KEY=ezvpZGnzn2Ws3Rw4uxLiJnof_b21Plmq32OktvotfynnFa2uBFncNB0hOHH0jocL
cd /d "%~dp0"
uvx open-terminal run --host 0.0.0.0 --port 9900
pause