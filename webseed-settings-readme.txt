With the 2015-04-01 releases came several new settings
pertaining to webseeds. These settings can be changed
within the settings.json file.

Note that none of these specific settings are
described on the official transmission web site.
The advanced webseed algorithm used
in the releases here has fixed most,
if not all, of the bugs posted on the bug trac system
of the official transmission website.
Official ticket https://trac.transmissionbt.com/ticket/5923
summarizes many of the problems, and offers fixes,
however this release uses at times a quite different,
but very well tested, approach to solve the problems
presented there.


Below are the webseed specific settings for settings.json

"blocklist-webseeds": false,
default is false
Set to true to filter the webseeds based on the loaded
blocklist(s)/whitelist(s).
note: "blocklist-enabled": true, must be true additionally
for webseed blocklisting to be active.


"drop-interrupted-webseeds": true,
default is true
Setting to false will use an alternative algorithm.
Use false if you experience any problems with
webseeds when the setting is set to true.


"webseed-max-connect-fails": 5,
default is 5 tries
How many times to try to connect to each
particular webseed before we quit trying forever.
Setting to zero means to try indefinitely.


"webseed-timeout-seconds": 20,
default is 20 seconds
How long in seconds to wait for a webseed
to connect before issuing a timeout connect fail.
Minimum timeout is 5 seconds.


"webseeders-maximum": 4
default is 4
Allowed maximum of webseed servers per torrent.
Set to zero to disallow webseeds entirely.


______________________________________________________________