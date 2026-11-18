# WSPR Scheduler Live

Shows who has heard your WSPR transmissions, from [wspr.live](https://wspr.live), and keeps the reports in
the WSPR Scheduler database (`wspr_rx.reported`), where WSPR Scheduler Map draws them on its TX map.
Written in C++ with Qt 6; a port of the C# *WSPR Scheduler Live* by Neville, G4GCI.

## Features

- **Automatic updates:** the reports for your callsign from wspr.live every 5 minutes (the last 10
  minutes to 24 hours), or straight away with *Update now*. Reports already saved are skipped.
- **Filters on the saved reports:** band, date and time (UTC), reporter callsign, distance in miles or
  km, and the reporter's software version (click a Version cell).
- **CW/SSB comparison:** how each report would have been for CW or SSB at a chosen power.
- **Another call:** look at another station's reports without saving them.
- **Keeps going:** if MySQL or the internet isn't there when it starts (after a power cut, say), it
  keeps trying every minute.

## Requirements

- Qt 6.4 or later: Widgets, Sql and Network, plus the QMYSQL driver.
- MySQL or MariaDB with the WSPR Scheduler databases. The database password is read from `DBcredential`
  in your user folder, as written by WSPR Scheduler, and your callsign from the same file (or set it with
  Config).
- An internet connection to wspr.live.

## Building

```
cmake -S . -B build
cmake --build build
```

Tests: configure with `-DWSPRLIVE_TESTS=ON`. The database tests need a scratch MySQL, never the real one.
The Windows installer for SourceForge is made with `installer\make_release.bat` (see installer\README.md).

## Linux

On Ubuntu, Debian or Raspberry Pi OS one script installs everything and builds the program:

```
wget https://raw.githubusercontent.com/npalmer1/wspr-scheduler-live-2/main/linux/install.sh
bash install.sh
```

Run it again to update. See [linux/README.md](linux/README.md).

## Licence

GNU General Public License v3.0. See [LICENSE](LICENSE).
