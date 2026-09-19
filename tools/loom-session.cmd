@echo off
rem SPDX-License-Identifier: MPL-2.0
rem Copyright (c) 2026 Joshua DeMoss
rem
rem loom-session -- the human CLI of a persistent Loom session (docs/guides/sessions.md).
rem Optional tooling: it needs Python 3.8 or newer. Set LOOM_PYTHON to choose the interpreter.
setlocal
set "RUNTIME=%~dp0..\lib\loom\python"
if not exist "%RUNTIME%\loom_session\__init__.py" (
    echo loom-session: cannot find %RUNTIME%\loom_session 1>&2
    exit /b 5
)
if defined LOOM_PYTHON (set "PY=%LOOM_PYTHON%") else (set "PY=python")
where "%PY%" >nul 2>nul
if errorlevel 1 if not exist "%PY%" (
    echo loom-session: needs Python 3.8 or newer, and none was found on PATH ^(set LOOM_PYTHON^) 1>&2
    exit /b 5
)
set "PYTHONPATH=%RUNTIME%;%PYTHONPATH%"
"%PY%" -m loom_session %*
exit /b %ERRORLEVEL%
