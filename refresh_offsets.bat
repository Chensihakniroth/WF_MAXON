@echo off
setlocal EnableExtensions

set "ROOT=%~dp0"
if not defined PYTHON set "PYTHON=python"

where "%PYTHON%" >nul 2>nul
if errorlevel 1 (
  echo [WF_MAXON] ERROR: Python was not found. Install Python 3.11+ and ensure it is on PATH.
  exit /b 2
)

if defined WF_MAXON_WARFRAME_EXE (
  "%PYTHON%" "%ROOT%tools\refresh_offsets.py" --binary "%WF_MAXON_WARFRAME_EXE%" %*
) else (
  "%PYTHON%" "%ROOT%tools\refresh_offsets.py" %*
)
if errorlevel 1 (
  echo [WF_MAXON] Offset refresh failed. The generated header was left unchanged.
  exit /b 1
)

echo [WF_MAXON] Offset refresh complete.
endlocal
