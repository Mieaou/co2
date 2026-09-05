@echo off
powershell.exe -ExecutionPolicy Bypass -NoProfile -File "%~dp0upload.ps1" %*
