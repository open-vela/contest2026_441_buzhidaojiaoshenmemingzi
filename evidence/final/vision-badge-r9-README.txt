r9 is the post-r8 vision response transport fix.

Changes:
  * Vision prompt asks for one or two short Chinese sentences (<=60 Chinese characters).
  * CP/AP RPC result copy preserves UTF-8 boundaries and appends an ellipsis on truncation.
  * Camera, DNS, TLS, credentials and MiMo request paths are unchanged from r8.

VM build: PASS
  command: python3 tools/bk7258/bk7258.py build --board aidk_ai_toy --boot direct --workspace /home/alientek/ov441_ws --jobs 8
  source compile: AP mimo_request.c and vision_badge_rpc.c rebuilt

Images:
  boot.bin  3808 bytes    c87852b82ead29ee09cccc0bc4dba2b940206af50d0e0bb580146fe80f9be659
  cp.bin    920210 bytes  66f6f8f3f51c6bfb8a1d49111695d7b01d42a3755663c4aa9cdc976599661bbc
  ap.bin    593470 bytes  e2405c941268611f6e20a0a8964550c4e9a5c4a201fc3791a6712fbf84f9226a
  pair.bin  2854912 bytes e74e6ba3d54c3eb6fd14be6cc7263a374abb89b41f5dec29982cc249d5d209fa

Flash script:
  G:\CIE\R1\tools\flash-vision-badge-20260919-r9.cmd

The script verifies image hashes and writes only AP and pair, preserving RF and other ranges.
