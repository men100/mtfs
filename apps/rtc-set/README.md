# RTC setting application

`rtc-set` is an embedded, UART-neutral command parser. The target supplies only
a character input loop and a text-output callback. It never performs timezone,
UTC, or daylight-saving conversion: all values are conventional local time.

Commands:

```text
set 2026-08-14 21:30:00
get
status
clear
help
```

`set` validates the complete Gregorian date before calling the provider. The
provider must clear its setting marker, update the RTC, read it back, and commit
the marker only after verification. A normal serial terminal at the target's
configured UART settings is sufficient; no PC helper is required.

Keep this directory out of the production library. Link it only into diagnostic
or provisioning firmware and run its character loop in a normal task, never an
ISR.

A target can install one synchronous extension callback with
`mtfs_rtc_set_app_set_extension()`. The core parser remains independent of
FatFs and board drivers; the target owns the command, help line, resource
initialization, and cleanup. Unknown extension commands fall through to the
normal `unknown command` response.
