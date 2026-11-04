@echo off
rem Запуск ядра в Windows. Код выхода 75 = «обновить и запустить снова».
cd /d "%~dp0\.."
set DROPSHIP_LAUNCHER=1
:loop
core-cpp\build\Release\dropship_core_app.exe %*
if %ERRORLEVEL% EQU 75 (
  echo == Update ==
  call scripts\update.cmd
  goto loop
)
exit /b %ERRORLEVEL%
