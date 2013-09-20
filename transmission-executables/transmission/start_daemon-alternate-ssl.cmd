@echo off

set CURL_CA_BUNDLE=%cd%\ca-bundle.crt
set HOME=%cd%
set TRANSMISSION_WEB_HOME=%cd%\web

transmission-daemon --pid-file transmission-daemon.pid
