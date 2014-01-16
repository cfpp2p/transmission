@echo off

set /p PID=<transmission-daemon.pid

taskkill /f /pid %PID%
del transmission-daemon.pid
