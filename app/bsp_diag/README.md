# BK7258 R1 BSP diagnostics

This directory contains two board bring-up commands maintained for the Team 441 CP image:

- `apctl`: inspect CPU0/AP/RPTUN/CPU2-SMP lifecycle state;
- `bkwifi`: exercise the AP-owned Wi-Fi VNET service from the CPU0 NSH.

Both commands expose runtime state only. They do not contain network credentials or product data.
