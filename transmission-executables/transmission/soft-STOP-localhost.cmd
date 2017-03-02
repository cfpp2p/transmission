@ECHO OFF

REM https://sourceforge.net/p/transmissiondaemon/discussion/general/thread/010fe8b6/

REM transmission-remote localhost:9091 -n username:password -a http:// > nul
REM transmission-remote localhost:9091 -n username:password --exit

transmission-remote localhost:9091 -a http:// > nul
transmission-remote localhost:9091 --exit

pause
