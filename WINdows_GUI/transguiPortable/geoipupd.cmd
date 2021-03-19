
:: CFPP2P GeoIP updater for Transmission Remote GUI
:: February 27, 2021

@echo off

if exist GeoIP.gz (
    if exist GeoIP.gz.bak.gz del GeoIP.gz.bak.gz
    rename GeoIP.gz GeoIP.gz.bak.gz
    )

if exist GeoIP.dat (
    if exist GeoIP.dat.bak del GeoIP.dat.bak
    copy GeoIP.dat GeoIP.dat.bak
    )

wget --output-document GeoIP.gz --no-check-certificate --continue https://mailfud.org/geoip-legacy/GeoIP.dat.gz

if not exist GeoIP.gz goto miyuru3

grep -F "GeoIP.dat" GeoIP.gz

if errorlevel 1 goto miyuru2
if not errorlevel 0 goto miyuru2

gzip --decompress --stdout GeoIP.gz > GeoIP.dat

if not exist GeoIP.dat goto miyuru

grep -F "GeoLite2 Country 20" GeoIP.dat

if errorlevel 0 if not errorlevel 1 goto done

:miyuru

if exist GeoIP.dat.bak copy GeoIP.dat.bak GeoIP.dat

:miyuru2

del GeoIP.gz

:miyuru3

echo .
echo .
echo .
echo first source FAILED, trying secondary...
echo .
echo .
echo .

wget --output-document GeoIP.gz --no-check-certificate --continue https://dl.miyuru.lk/geoip/maxmind/country/maxmind4.dat.gz

if not exist GeoIP.gz goto failed3

grep -F "maxmind4.dat" GeoIP.gz

if errorlevel 1 goto failed2
if not errorlevel 0 goto failed2

gzip --decompress --stdout GeoIP.gz > GeoIP.dat

if not exist GeoIP.dat goto failed

grep -F " converted to legacy DB with sherpya/geolite2legacy by miyuru.lk" GeoIP.dat

if errorlevel 0 if not errorlevel 1 goto done

:failed

if exist GeoIP.dat.bak copy GeoIP.dat.bak GeoIP.dat

:failed2

del GeoIP.gz

:failed3

echo .
echo .
echo .
echo GeoIP update FAILED
echo .
echo .
echo .

goto alldone

:done

del GeoIP.gz

echo .
echo .
echo .
echo GeoIP update successful
echo .
echo .
echo .

:alldone

if exist GeoIP.gz.bak.gz rename GeoIP.gz.bak.gz GeoIP.gz

echo   .
echo   .
echo To close this window:
pause
