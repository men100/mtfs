# STM32Cube RTC timestamp provider

This port adapts the STM32 calendar RTC to `mtfs_time_provider_t`. It uses
conventional local time. UTC conversion, time zones, and daylight-saving policy
remain application responsibilities.

The STM32N657 target uses LSI (nominally 32 kHz), matching ST's N6570-DK RTC
examples. The port will select LSI only when no RTC source has been selected. It
will not reset a live backup domain to replace another source, because that
would silently invalidate both calendar and marker.

The setting marker occupies `TAMP_BKP28R` through `TAMP_BKP30R`:

- register 28: `MTFS` magic, written last as the commit word;
- register 29: marker format version and check word;
- register 30: inverted magic.

`set_local` clears the commit word before changing the RTC, reads back the
calendar, and commits the marker only after an exact match. A missing or partial
marker is `UNSET`; a valid marker with an unreadable or invalid calendar is
`ERROR`. The STM32 calendar's two-digit year limits this port to 2000-2099 even
though FAT itself covers 1980-2107.

The backup registers and RTC survive a software system reset. They survive loss
of VDD only while the backup domain is supplied through VBAT. Tamper erase,
backup-domain reset, or loss of both VDD and VBAT removes the marker. LSI is not
guaranteed to operate from VBAT on STM32N6; therefore VDD-off time retention is
not claimed for this LSI target configuration. Hardware retention remains to be
measured on STM32N6570-DK.

The current target is a Full Secure image and accesses secure RTC/TAMP aliases.
For a future TrustZone split, keep RTC setting and marker writes in Secure code;
expose a validated read-only time service to Non-Secure code rather than sharing
the backup-register write zone.

The application must register the returned provider after initializing an OS
mutex, for example:

```c
mtfs_stm32_rtc_init(&rtc_context, lock, unlock, mutex_context);
mtfs_time_provider_register(mtfs_stm32_rtc_provider(&rtc_context));
```

Do not link this directory, `mtfs_fattime.c`, or the RTC HAL modules when
`MTFS_FF_FS_NORTC=1`; FatFs then keeps its fixed timestamp without RTC RAM or
runtime cost.
