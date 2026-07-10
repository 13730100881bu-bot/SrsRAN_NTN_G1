@echo off
setlocal
cd /d "%~dp0"

where node >nul 2>nul
if errorlevel 1 (
  echo Node.js was not found. Please install Node.js 22 or newer.
  pause
  exit /b 1
)

if not exist "node_modules\vinext" (
  echo Preparing the local web interface...
  call npm install
  if errorlevel 1 (
    echo Failed to install dependencies.
    pause
    exit /b 1
  )
)

start "NTN Beam Planner Server" cmd /k "npm run dev -- -H 127.0.0.1 -p 4317"

powershell -NoProfile -Command ^
  "$url='http://127.0.0.1:4317'; $ready=$false; for($i=0;$i -lt 40;$i++){ try { $response=Invoke-WebRequest -UseBasicParsing -Uri $url -TimeoutSec 1; if($response.StatusCode -eq 200){$ready=$true;break} } catch {}; Start-Sleep -Milliseconds 500 }; if($ready){Start-Process $url; exit 0}else{exit 1}"

if errorlevel 1 (
  echo The page did not become ready. Check the server window for details.
  pause
  exit /b 1
)

echo The NTN beam planner is open at http://127.0.0.1:4317
echo Close the "NTN Beam Planner Server" window when you are finished.
endlocal
