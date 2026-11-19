# Third-Party Notices

The component wrappers in this directory fetch or expose upstream projects.
Their licenses remain those of the upstream projects and are not changed by
the MIT license used for M5Stack-authored wrapper code.

| Wrapper | Upstream source | License record |
| --- | --- | --- |
| `Miniaudio` | `https://github.com/mackron/miniaudio` | The complete public-domain/MIT-0 notice is embedded at the end of `Miniaudio/include/miniaudio.h`. |
| `tree.hh` | Kasper Peeters tree container | GPL-3.0-only attribution in `cp0_lvgl/include/tree.hh`; full text in `../LICENSES/tree.hh-GPL-3.0-only.txt`. |

The full notice is also retained in `Miniaudio/LICENSE`. SDK dependencies used by these components include
LVGL (MIT), eventpp (Apache-2.0), and platform libraries selected by
SConstruct. `../LICENSES/` records fmt (MIT), cJSON and C-Thread-Pool. The repository
inventory distinguishes optional cached sources from linked dependencies.

See [`../docs/OPEN_SOURCE_COMPONENTS.md`](../docs/OPEN_SOURCE_COMPONENTS.md)
for the complete repository inventory, including SDK, LVGL, and system
dependencies.
