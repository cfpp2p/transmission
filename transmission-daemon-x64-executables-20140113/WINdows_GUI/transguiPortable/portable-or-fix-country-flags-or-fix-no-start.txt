SRS  05/02/2014

transmission remote GUI SRS build transgui.exe

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