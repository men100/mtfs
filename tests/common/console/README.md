# Shared diagnostic console

This directory contains the UART-neutral RTC command parser and its T-Monitor
transport adapter used by the EK-RA8P1 and STM32N6570-DK basic test targets.

The built-in commands are `help`, `rtc-get`, `rtc-status`, `rtc-set`, and
`rtc-clear`. Targets add synchronous storage and crypto commands through
`mtfs_console_set_extension()` and own their resource initialization and
cleanup.

This is test infrastructure, not a standalone application or production
library component.
