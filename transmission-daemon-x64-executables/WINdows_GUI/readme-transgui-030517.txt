
Install transgui-3.1-setup.exe first then over-write installed exe with transgui.exe

March 05, 2017

Source files now maintained at https://github.com/cfpp2p/transmisson-remote-gui

Enhancement release for cfpp2p daemon
https://github.com/cfpp2p/transmission

FIXED: Some Unicode .torrent files wouldn't load when double clicked.
FIXED: Add trackers from duplicate torrents.
ADDED: Add trackers from magnets links on duplicate torrents.
ADDED: Allow zero as a global timeout for seeding (spin edit).
FIXED: Adding torrent by url add trackers of duplicate torrents.
FIXED: download of flags.zip.

May 6, 2016

@cfpp2p cfpp2p released this Apr 18, 2016
Initial release for cfpp2p transmission daemon

https://github.com/cfpp2p/transmission

    Allow or specify customized individual "rpc-url" for host(s).
    Much faster processing when have and/or add many torrents and/or HUGE number of files in the torrent(s).
    Fix client stalling.
    Allow empty blocklist url.

May 6, 2016
cfpp2p pushed to master at cfpp2p/transmisson-remote-gui
@cfpp2p d5fbe2b
add a "size left" column

cfpp2p/transmission@5e303a7

    streaming integration

cfpp2p/transmission@b3ce2eb

    displays blocklist entry count & changes
    new "Seed Ratio" column
    "Ratio" column renamed to "U-D Ratio"

cfpp2p/transmission@43208e6

    fixed display issues with sort, selection and column size

cfpp2p/transmission@382b1be

    fixed crash on filter, text search and close

cfpp2p/transmission@94d49c2

    Many enhancements and fixes

cfpp2p/transmission@2ada374


July 31, 2014

Streaming integration with libtransmission

Highlights of changes and enhancements:

May 23, 2014

displays blocklist entry count & changes
new "Seed Ratio" column
"Ratio" column renamed to "U-D Ratio"


April 2, 2014

fixed display issues with sort, selection and column size
fixed crash on filter, text search and close

Skip Verify
Show Verify Queue status
Seeding time column
Seeding time also shown next to ratio in torrent info pane
Include paused torrents in error filter
Unicode support
fixed "List index (xx) out of bounds"



source
https://github.com/cfpp2p/transmission/commits/Windows_Daemon_%26_Clients
https://github.com/cfpp2p/transmission/tree/Windows_Daemon_%26_Clients/WINdows_GUI/source



May 2, 2014

transmission remote GUI transgui.exe

transgui.exe simply doesn't start
OR
Country flags don't show


Windows XP Default
transgui.exe simply doesn't start
  create directory
   "c:\Documents and Settings\-=:WHATEVER-YOUR-USERNAME-IS:=-\Local Settings\Application Data\Transmission Remote GUI\"

Country flags don't show
  Create a directory named "flags"   
   "c:\Documents and Settings\-=:WHATEVER-YOUR-USERNAME-IS:=-\Local Settings\Application Data\Transmission Remote GUI\flags\"
   
   
   
Windows 7 Default
transgui.exe simply doesn't start
  create directory
c:\Users\-=:WHATEVER-YOUR-USERNAME-IS:=-\AppData\Local\Transmission Remote GUI\

Country flags don't show
  Create a directory named "flags"
c:\Users\-=:WHATEVER-YOUR-USERNAME-IS:=-\AppData\Local\Transmission Remote GUI\flags\


More simply (NO prior "install" needed or ->portable) for any version Windows.
Just create a folder and copy there the transgui executable file (and, optionally, the lang folder). Then create empty transgui.ini file in that folder.
To see the country flag of peers also create a "flags" folder in same folder where transgui.exe was copied.