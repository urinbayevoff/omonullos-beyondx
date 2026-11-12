#!/usr/bin/env python3
"""Panfrost: AARCH64_4K page tables for the Exynos 9820's Mali-G76 (zboot).

Added for OmonullOS, October 2026. With Panfrost's legacy Mali LPAE page
tables the Galaxy S10 5G's Mali-G76 r0p0 faulted at the start of every
client (js=1 DATA_INVALID_FAULT, js=0 IMPRECISE_FAULT, 4-8 per run) and the
first clients after a boot failed whole (100+ faults, nothing drawn).
Samsung's kbase drives Bifrost with AARCH64_4K tables; with those (Panfrost's
GPU_QUIRK_FORCE_AARCH64_PGTABLE, as on the MediaTek G57s) the faults are
gone, runtime PM on or off (docs/s10-5g/log.md, 2026-10-17 10:25).
"samsung,exynos9820-mali" is already the first compatible in exynos9820.dtsi.
"""
from pathlib import Path
import sys

p = Path(sys.argv[1]) / 'drivers/gpu/drm/panfrost/panfrost_drv.c'
s = p.read_text()
if 'exynos9820_data' in s:
    sys.exit(0)
data_anchor = 'static const struct of_device_id dt_match[] = {\n'
match_anchor = '\t{ .compatible = "arm,mali-bifrost", .data = &default_data, },\n'
if s.count(data_anchor) != 1 or s.count(match_anchor) != 1:
    sys.exit('patch-panfrost-zb: dt_match not found as expected')
s = s.replace(data_anchor,
              '/* zboot: Mali-G76 r0p0 faults with legacy page tables (patch-panfrost-zb.py) */\n'
              'static const struct panfrost_compatible exynos9820_data = {\n'
              '\t.num_supplies = ARRAY_SIZE(default_supplies) - 1,\n'
              '\t.supply_names = default_supplies,\n'
              '\t.num_pm_domains = 1, /* optional */\n'
              '\t.gpu_quirks = BIT(GPU_QUIRK_FORCE_AARCH64_PGTABLE),\n'
              '};\n\n' + data_anchor)
s = s.replace(match_anchor,
              '\t{ .compatible = "samsung,exynos9820-mali", .data = &exynos9820_data },\n'
              + match_anchor)
p.write_text(s)
