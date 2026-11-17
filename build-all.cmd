@echo off
rem Builds x64, x86 (32-bit) and arm64.
for %%a in (x64 x86 arm64) do (
  call "%~dp0build.cmd" %%a || exit /b 1
)
