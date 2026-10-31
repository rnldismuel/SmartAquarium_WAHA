@echo off
setlocal
if "%ESP32_IP%"=="" set /p ESP32_IP=Masukkan IP ESP32 (contoh 10.10.8.13): 
if "%ESP32_API_KEY%"=="" set /p ESP32_API_KEY=Masukkan ESP32 API key: 
curl.exe -i -H "X-API-Key: %ESP32_API_KEY%" "http://%ESP32_IP%/api/health"
echo.
curl.exe -i -H "X-API-Key: %ESP32_API_KEY%" "http://%ESP32_IP%/api/info"
pause
