# Third party licenses

Full license texts are in [`licenses/`](licenses), and are shipped in release zips at `config/sys-ftpsrv/licenses/`.

| Component | License | Used in | Text |
| --- | --- | --- | --- |
| [ftpsrv](https://github.com/ITotalJustice/ftpsrv) by TotalJustice | MIT | sysmodule | `ftpsrv.txt` |
| [minIni-nx](https://github.com/ITotalJustice/minIni-nx) by TotalJustice, based on minIni by CompuPhase | Apache-2.0 | sysmodule only (used by ftpsrv's custom commands to read hekate's config) | `minIni-nx_LICENSE.txt`, `minIni-nx_NOTICE.txt` |
| [libnx](https://github.com/switchbrew/libnx) | ISC | sysmodule, overlay | `libnx.txt` |
| [libultrahand](https://github.com/ppkantorski/libultrahand) by ppkantorski | GPL-2.0, libultra also CC-BY-4.0 | overlay | `GPL-2.0.txt`, `libultrahand-libultra_CC-BY-4.0.txt` |
| [libtesla](https://github.com/WerWolv/libtesla) by WerWolv (part of libultrahand) | GPL-2.0-or-later | overlay | `GPL-2.0.txt` |
| [cJSON](https://github.com/DaveGamble/cJSON) (part of libultrahand) | MIT | overlay | `cJSON.txt` |
| [stb_truetype](https://github.com/nothings/stb) (part of libultrahand) | Public domain | overlay | - |

The overlay does not link minIni: its config file handling is sys-ftpsrv's own `common/cfgini.c` (MIT), which keeps the Apache-2.0 code out of the GPL overlay.
