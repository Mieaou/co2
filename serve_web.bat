@echo off
title AeroSense Local Web Server
cd /d "%~dp0docs"
echo ================================================================
echo   AeroSense: Starting Local Web Server (Web Bluetooth Enabled)
echo   URL: http://localhost:8000
echo ================================================================
start http://localhost:8000
python -m http.server 8000
