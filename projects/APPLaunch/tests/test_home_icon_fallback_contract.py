# SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
# SPDX-License-Identifier: MIT

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
POOL = (ROOT / "main/ui/home_icon_buffer_pool.cpp").read_text(encoding="utf-8")
PAGE = (ROOT / "main/ui/ui_launch_page.cpp").read_text(encoding="utf-8")
ICONS = ROOT / "APPLaunch/share/images/line"

# An icon that fails to decode falls back to the generic app icon, which goes
# through the same decode path as every other icon.
assert "found = icons_.find(fallback_path_)" in POOL
fallback = re.search(r'kFallbackIcon = "(\w+)"', PAGE).group(1)
assert (ICONS / f"{fallback}.png").is_file()

# Every slug the home grid knows has its icon file.
known = re.search(r"kKnown = \{(.*?)\};", PAGE, re.S).group(1)
for slug in re.findall(r'"(\w+)"', known):
    assert (ICONS / f"{slug}.png").is_file(), slug
