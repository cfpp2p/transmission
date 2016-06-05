April 20, 2016

This update is for current transmission-daemon-cygwin cfpp2p

https://sourceforge.net/projects/transmissiondaemon/files/

x86_32-2016-01-16  or  64bit-2016-01-16

Download manually as per above or
use one of the below
Command Script or InternetShortcut

32bit-transmission-daemon-download.cmd
64bit-transmission-daemon-download.cmd
32bit-transmission-daemon-download.url
64bit-transmission-daemon-download.url

Then proceed with the following:
_____________________________________________________________________

transmission-daemon.exe

Replace the original "transmission-daemon.exe" with this update.

From: cfpp2p <cfpp2p>
Date: Wed, 1 Jun 2016 19:56:30 -0500
Subject: [PATCH] Allow extended ASCII character set ISO 8859-1
set on/off with "clean-json-utf-enabled" in settings.json
also fixed https://trac.transmissionbt.com/ticket/4882

Commits on May 23, 2016

    /* use original accounting method when maxConcurrentAnnounces less than zero */
    cfpp2p committed May 23, 2016

    Add session settings "multiscrape-maximum" and "concurrent-announces-maximum" to settings.json.
    cfpp2p committed May 23, 2016

    Fix fragmented datagrams.
    cfpp2p committed May 23, 2016


changes:
From: cfpp2p <cfpp2p>
Date: Wed, 20 Apr 2016 13:54:06 -0500
Subject: [PATCH] Add setting for CURLOPT_MAXREDIRS to settings.json
_____________________________________________________________________

transgui.exe

Replace the original "transgui.exe" GUI client with this update.

changes:
Allow or specify customized individual "rpc-url" for host(s).
Much faster processing when have and/or add many torrents and/or HUGE number of files in the torrent(s).
Fix client stalling.
Allow empty blocklist url.

_____________________________________________________________________

web client 

Replace the original "web" client with this update.

changes:

*****
From: cfpp2p <cfpp2p>
Date: Fri, 3 Jun 2016 20:26:38 -0500
Subject: [PATCH] Fix error box details button not working.

*****
Allow custom rpc-url functionality in Shift (as specified in the daemon settings).
example (settings.json)
"rpc-url": "/junk/moreJunk/andyetmOre/",

*****
"alert status of add torrent by URL"

*****
"Enable torcache in the web client" (http://torcache.net/)

Optionally allows use of torcache instead of magnet link or a direct hash
ONLY when a magnet or hash is preceded by the letter t and a space.
Presented in the "Open Torrent" -> "Upload Torrent Files" -> "enter a URL"
Can use a base 16 or base 32 hash.

Examples for torrent ubuntu ubuntu-15.10-desktop-i386.iso:

t 268a4ef6845c1553d4328b3606c59f56372fce64

T magnet:?xt=urn:btih:268a4ef6845c1553d4328b3606c59f56372fce64&dn=ubuntu-15.10-desktop-i386.iso&tr=http%3A%2F%2Ftorrent.ubuntu.com%3A6969%2Fannounce&tr=http%3A%2F%2Fipv6.torrent.ubuntu.com%3A6969%2Fannounce

t e2fe55uelqkvhvbsrm3anrm7ky3s7tte (base 32 hash)

t magnet:?xt=urn:btih:268a4ef6845c1553d4328b3606c59f56372fce64 


*****
"Group Tag View (beta)"

https://trac.transmissionbt.com/ticket/4518
Mark appropriate fields in prefs as html5 number fields (patch by e-moe)
https://trac.transmissionbt.com/ticket/5974
Torrents queued should be able to "Resume Now" (patch by JJTagy)

_____________________________________________________________________
_____________________________________________________________________
_____________________________________________________________________



*****
soft-STOP-localhost.cmd

NEW .cmd file, place where you put the x86_32-2016-01-16 or 64bit-2016-01-16
release's .exe and .dll files.

Edit (with notepad or any text editor) as necessary:
    localhost:9091 or whatever your daemon's host:port is
    -n username:password user-name and password if you set them
    http:// followed by nothing. (this line ONLY needed for if using 32 bit release)
    "REM" disables a line by making it a remark line

https://sourceforge.net/p/transmissiondaemon/discussion/general/thread/010fe8b6/
https://sourceforge.net/p/transmissiondaemon/discussion/general/thread/b4eea1a5/
https://sourceforge.net/p/transmissiondaemon/discussion/general/thread/6ec81ea0/