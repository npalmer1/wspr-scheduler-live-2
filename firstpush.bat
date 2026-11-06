@echo off
rem WSPR Scheduler Live 2: first push to GitHub (run once; afterwards use gitpush.bat)
cd /d C:\WSPR-Live-2
if errorlevel 1 (echo Folder C:\WSPR-Live-2 not found & pause & exit /b 1)
if not exist .git git init -b main
git add .
git commit -m "WSPR Scheduler Live 2 - Qt6 port, V0.2.1"
git branch -M main
git remote remove origin 2>nul
git remote add origin https://github.com/npalmer1/wspr-scheduler-live-2.git
echo.
git remote -v
echo.
git push -u origin main
pause
