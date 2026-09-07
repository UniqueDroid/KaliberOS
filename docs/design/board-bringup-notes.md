# Board bring-up notes

Not an architecture doc - a running list of hardware findings from bringing
up new boards that don't belong in any one board's `board.c` comment,
because the whole point is that they'd bite the *next* board too if nobody
checked first. Where a design doc's own §-somewhere already covers a
finding in more detail, this file points there instead of duplicating it.

## Optional reset pins on reference drivers aren't always optional

Found 2026-09-07 bringing up `waveshare_c6_amoled`'s FT3168 touch
controller (I2C). Waveshare's own reference driver
(`Arduino_FT3x68.cpp`) treats the chip's RST pin as optional - it falls
back to the chip's own power-on reset when no RST GPIO is given. That's
true of the *driver code*, not necessarily of the *board*: on real
hardware, the FT3168 was completely silent on the I2C bus - not "wrong
address," not "NAK," genuinely absent from a full bus scan - until its
RST pin was actively pulsed (idle HIGH, pulse LOW ~20ms, back HIGH,
settle ~50ms, the exact sequence the reference driver uses when an RST
pin *is* given). Skipping the pulse looked, from software's side,
indistinguishable from "wrong I2C address" or "chip not populated" -
both far more plausible first guesses than "the optional pin isn't
actually optional on this board."

**The generalizable lesson, for the next board with an I2C peripheral
whose reference driver marks its reset pin optional:** don't take
"optional in the driver" as "optional on this board" - wire and pulse
the reset pin from the start, or at minimum run a full I2C bus scan
(not just a single-address probe) as the very first bring-up step
before spending time on register-level theories. A bus scan finding
*nothing at all* at the expected address is a different, stronger
signal than a scan finding the address present but NAKing a specific
command - the former points straight at reset/power, the latter at
addressing or protocol.

**The even more general form of this rule** (project chat 2026-09-07,
after this was the third time a related-source assumption turned out
not to hold for this specific board): **a reset pin is never optional
until proven otherwise on the actual device in hand** - not "the
reference driver treats it as optional," not "it probably shares a net
with another already-reset pin," proven, on this board, with a scope or
a bus scan. `waveshare_c6_amoled` has at least three more chips on the
same general pattern (I2C peripheral, dedicated reset line) still ahead
of it - QMI8658 (IMU, js-api.md's Step provider), PCF85063 (RTC, not
wired up yet), and the SH8601/CO5300 AMOLED panel itself (already
handled correctly, `disp_init()` does pulse `PIN_LCD_RESET` - the one
chip on this board that never hit this bug, because nobody skipped its
reset pulse in the first place). Whichever of these gets bricked up
next: pulse its reset pin from the start, don't wait for the same
silent-bus symptom to reappear and cost the same debugging pass again.

Also found in the same session: switching a chip's documented default
power mode (here, FT3168's "monitor"/low-power mode, register `0xA5` =
`0x01`) to a more verbose always-on mode (`0x00`, active/continuous
scan) during debugging can mask or fix a separate, real issue by
accident - confirm which change actually mattered before committing to
both. Here, the reset pulse was the real fix; the power-mode change was
never independently retested afterward and stayed as "known-working,"
flagged as a follow-up in `waveshare_c6_amoled/board.c`'s own comment,
not silently treated as settled.
