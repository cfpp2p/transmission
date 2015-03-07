@echo off


set HOME=%cd%
set TRANSMISSION_WEB_HOME=%cd%\web

transmission-daemon --pid-file transmission-daemon.pid
