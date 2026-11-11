@echo off
rem Dois cliques aqui copiam o index.html para o firmware (painel.h).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0atualizar_painel.ps1"
pause
