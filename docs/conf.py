# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Thalles Campagnani
# Sphinx configuration. Build the HTML with `make` in this folder.
project = "vkfwtest"
author = "Thalles Campagnani"
copyright = "2026, Thalles Campagnani"
language = "en"

extensions = ["sphinx_rtd_theme", "sphinx_rtd_dark_mode"]
default_dark_mode = False
exclude_patterns = ["_build", ".venv", "steps.md", "requirements.txt"]

html_theme = "sphinx_rtd_theme"
html_title = "vkfwtest — Vulkan test battery for GPU firmware"
html_show_sourcelink = False
html_theme_options = {
    "navigation_depth": 3,
    "collapse_navigation": False,
    "sticky_navigation": True,
    "prev_next_buttons_location": "both",
}
pygments_style = "sphinx"
html_static_path = ["_static"]
html_css_files = ["tables.css"]
html_js_files = ["theme_persist.js"]
