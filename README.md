# sys-ftpsrv

A background FTP server sysmodule for the Nintendo Switch, plus an
[Ultrahand](https://github.com/ppkantorski/Ultrahand-Overlay) overlay to turn it on / off and configure it.

It runs [ftpsrv](https://github.com/ITotalJustice/ftpsrv), the same FTP server built into sphaira, pinned to the same revision (`7c82402`).
ftpsrv is single threaded and poll based, uses no dynamic memory (the sysmodule has no heap at all), and supports resume (`REST`), `MLSD` / `MLST`, `SIZE` / `MDTM`, idle timeouts and [custom Nx commands](https://github.com/ITotalJustice/ftpsrv/wiki/Nx-%E2%80%90-Custom-Commands).
It also exposes sphaira's per-device mount options, so you can choose exactly which devices the server shows.

## Install

Extract `sys-ftpsrv.zip` to the root of your SD card:

```
atmosphere/contents/4200000000000F7B/exefs.nsp
atmosphere/contents/4200000000000F7B/flags/boot2.flag   (starts on boot)
atmosphere/contents/4200000000000F7B/toolbox.json       (ovl-sysmodules support)
config/sys-ftpsrv/config.ini.template
switch/.overlays/sys-ftpsrv.ovl
```

Requires Atmosphère, and [Ultrahand Overlay](https://github.com/ppkantorski/Ultrahand-Overlay) + nx-ovlloader for the overlay.

> [!IMPORTANT]
> The server **will not start** until a login is configured: either enable *Anonymous login* in the overlay, or set `user` / `pass` in `/config/sys-ftpsrv/config.ini`.
> This matches upstream ftpsrv, since an open server is a risk when using ldn_mitm.

If you are migrating from sys-ftpd, disable it (remove its `boot2.flag`) as both default to port 5000.

### Using alongside sphaira

sys-ftpsrv does not start, stop or reconfigure sphaira's built-in FTP server; coordinating the two is left to you.

- **Port:** sphaira's FTP server also defaults to 5000, and its *FTP Install* menu always starts it (even with FTP disabled in its settings). If both use the same port, whichever starts first gets it and the other silently keeps retrying. Give them different ports, or stop sys-ftpsrv while using sphaira's FTP.
  The overlay reads `/config/sphaira/config.ini`. The status panel's *Sphaira* row shows sphaira's saved port and its "Enable FTP" setting (`setting on` / `setting off`). When the ports match, the warnings are:
  - **Yellow:** same port, potential conflict.
  - **Red:** sphaira's server is running on that port. The overlay checks this about every 3 seconds while it is open: it connects to the port and sends an unauthenticated `TID`, which sys-ftpsrv recognises (`530`) and sphaira's server doesn't (`500`). If sys-ftpsrv is running at the same time, its status shows *Blocked by sphaira*.

  Known limitation: if sys-ftpsrv got the port first, sphaira's server just retries in the background without opening a socket, so it can't be detected and the warning stays yellow.
- **Gamecard:** the *Gamecard* mount keeps the cart open while it is browsed, which may conflict with sphaira's gamecard menu (sphaira's own FTP server leaves this mount out for that reason). It is off by default.

## Overlay

The top of every page is a fixed status panel, laid out like Horizon OC and emuiibo, with the settings scrolling below it.

| Panel row | |
| --- | --- |
| FTP Server | On / Off. **R** starts the server, **L** stops it (only when pressed on their own). The row can also be tapped. |
| Status | Running / Stopped / errors, *Running, port shared* (yellow) or *Blocked by sphaira* (red), see below. |
| Address | `ip:port` to connect to. |
| Changes | *Restart needed* when a setting changed while the server is running. **R** (or tapping the row) restarts the server to apply it. When there is nothing to apply, R does nothing so connections aren't dropped. |
| Sphaira | Sphaira's FTP port and its "Enable FTP" setting, or *running* when its server is detected on the port. |

L / R are used rather than X / Y as those are commonly bound elsewhere. They no longer jump to the top / bottom of the list here; ZL / ZR still skip a page.

| Page | Settings |
| --- | --- |
| Main | **Login:** anonymous login, username, password, read only. As in [sys-ftpd-ovl](https://github.com/Alpacinator/sys-ftpd-ovl), A on *Username* cycles through the `users` presets (default `switch,ftpd`, add your own under `[Presets]` in `config.ini`) and A on *Password* generates a new random password, shown so it can be entered in your client. **Network:** port (cycles 21 / 2121 / 5000 / 5001, yellow / red when it matches sphaira's) and idle timeout. |
| Mounts | Extra devices: album, `/switch` and `/atmosphere/contents` shortcuts, save data (optionally writable), gamecard, content storage, BIS. When any of these are enabled the FTP root lists every device (the SD card is at `/sdmc:`); otherwise the root *is* the SD card, like sys-ftpd. |
| Password generator | Length (4 / 6 / 8 / 10 / 12), uppercase only, and special characters for generated passwords. Uses the system's kernel-seeded random generator. |
| Advanced | Start on boot (`flags/boot2.flag`), controller LED flash on transfer, local time, non-ASCII file names, log file (`/config/sys-ftpsrv/log.txt`). |

Settings are written to `/config/sys-ftpsrv/config.ini` (seeded from the template the first time the overlay opens), which documents every option.
The theme, wallpaper and translations can be overridden in `/config/sys-ftpsrv/` as with any libultrahand overlay.

## Building

Needs devkitPro with `switch-dev`, `switch-cmake` and the portlibs used by libultrahand:

```sh
(dkp-)pacman -S switch-curl switch-zlib switch-minizip switch-mbedtls
```

```sh
cmake --preset Release
cmake --build --preset Release
# sd card layout is written to build/Release/out/
./build_release.sh   # additionally zips it into sys-ftpsrv.zip
```

The program id can be changed with `-DSYS_FTPSRV_PROGRAM_ID=<16 hex digits>`.

## Layout

| Path | |
| --- | --- |
| `common/` | Config schema and loader shared by both binaries, built on `cfgini.c`, a small INI reader/writer that reads the same syntax as minIni and edits files in place (comments and formatting kept). |
| `sysmodule/` | The sysmodule, based on ftpsrv's `main_sysmod.c`, and its NPDM. |
| `overlay/` | The libultrahand overlay. |
| `assets/config.ini.template` | Documented default config. |
| `licenses/` | Third party license texts, copied into release zips. |

ftpsrv, minIni-nx and libultrahand aren't vendored: CMake downloads them at configure time, pinned to the commits / tags in `CMakeLists.txt` (ftpsrv `7c82402`, the same revision sphaira uses). Update a pin there to pick up upstream fixes.

Sphaira-only features of its built-in server (the `install`, `games` and `mounts` virtual folders) depend on sphaira's installer and mounts and are not available in the sysmodule.

## License

- Sysmodule, shared code and build files: [MIT](LICENSE-MIT).
- Overlay: GPL-2.0-or-later, as it is built with libultrahand (GPL-2.0).

See [LICENSE](LICENSE) for the details and [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) for every third party component.

## Credits

- [ftpsrv](https://github.com/ITotalJustice/ftpsrv) by TotalJustice (MIT), the FTP server itself.
- [minIni-nx](https://github.com/ITotalJustice/minIni-nx) by TotalJustice (Apache-2.0), used by ftpsrv's custom commands in the sysmodule.
- [libultrahand](https://github.com/ppkantorski/libultrahand) by ppkantorski (GPL-2.0, libultra also CC-BY-4.0), based on [libtesla](https://github.com/WerWolv/libtesla) by WerWolv.
- [sys-ftpd-ovl](https://github.com/Alpacinator/sys-ftpd-ovl) by Alpacinator, whose username / password behaviour the overlay follows.
- [Horizon OC](https://github.com/Horizon-OC/Horizon-OC) and [emuiibo](https://github.com/XorTroll/emuiibo), whose layouts inspired the status panel.
