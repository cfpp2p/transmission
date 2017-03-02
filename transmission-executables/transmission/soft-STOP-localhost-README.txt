soft-STOP-localhost.cmd
https://sourceforge.net/p/transmissiondaemon/discussion/general/thread/010fe8b6/
https://sourceforge.net/p/transmissiondaemon/discussion/general/thread/b4eea1a5/
https://sourceforge.net/p/transmissiondaemon/discussion/general/thread/6ec81ea0/

in order

    transmission-remote localhost:9091 -n username:password -a http://
    transmission-remote localhost:9091 -n username:password --exit

Edit as necessary:
    localhost:9091 or whatever your daemon's host:port is
    -n username:password user-name and password if you set them
    http:// followed by nothing. (this line ONLY needed for if using 32 bit release)
    "REM" disables a line by making it a remark line

