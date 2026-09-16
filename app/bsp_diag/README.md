# BK7258 R1 BSP diagnostics

This directory contains only two board-bring-up commands:

- `apctl`: inspect CPU0/AP/RPTUN/CPU2-SMP lifecycle state;
- `bkwifi`: exercise the AP-owned Wi-Fi VNET service from the CPU0 NSH.

The command sources were derived from the platform-diagnostic portion of
`open-vela/contest2026_135_yongwangzhiqian` at commit
`7079493e73159e00c1b03e60bee6ee69845751cd`.  Product applications, cloud
services, UI, models and credentials from that repository are not included.
See the repository-level `SOURCE_PROVENANCE.md` for the complete boundary.
