# Building the firmware

This documents a known-good, reproducible build of the TG-GR6000N (CC2630F128)
firmware, including how to obtain a usable CC26x0 driverlib without a TI login.

## 1. Prerequisites

- **ARM GCC toolchain** (`arm-none-eabi-gcc`). Verified with Arm GNU Toolchain
  15.2. Install via your package manager or
  <https://developer.arm.com/downloads/-/gnu-rm>.
- **git**, **curl**.

```bash
arm-none-eabi-gcc --version   # confirm it's on PATH
```

## 2. Obtain the CC26x0 driverlib

The Makefile expects a TI CC26x0 driverlib (headers + a prebuilt
`driverlib.lib`). The official source is the **SimpleLink CC2640R2 SDK**
(login-gated). If you don't have it, you can build an equivalent driverlib from
a public source mirror, as below.

> ### Driverlib selection notes (why this specific source)
> - `contiki-os/cc26xxware` is a *partial* mirror — it omits `rf_ieee_cmd.h`
>   (Contiki used proprietary-mode RF), and its API is older than this firmware
>   targets (e.g. `SysCtrlAdjustRechargeAfterPowerDown()` takes no argument
>   there, but the firmware calls it with one → compile error).
> - **`contiki-ng/cc2640r2-sdk`** is a complete mirror of the CC2640R2 SDK
>   driverlib and matches the API this firmware expects. CC2640R2 is BLE-only,
>   so it lacks `rf_ieee_cmd.h` — but that header is just IEEE 802.15.4 command
>   *struct* definitions, which we add from a version-matched source. This is
>   the base used below.

```bash
ROOT="$HOME/Code/ti/cc2640r2-sdk"
git clone --depth 1 https://github.com/contiki-ng/cc2640r2-sdk.git "$ROOT"

# Add the IEEE 802.15.4 command header (CC2630 uses IEEE mode; the BLE-only
# CC2640R2 SDK omits it). Contiki's RF-core API header is TI's original,
# version-matched to this driverlib generation. Normalize its includes to the
# bare names used elsewhere in driverlib/.
curl -fsSL -o "$ROOT/driverlib/rf_ieee_cmd.h" \
  https://raw.githubusercontent.com/contiki-os/contiki/master/cpu/cc26xx-cc13xx/rf-core/api/ieee_cmd.h
sed -i '' \
  -e 's#"driverlib/rf_mailbox.h"#"rf_mailbox.h"#' \
  -e 's#"driverlib/rf_common_cmd.h"#"rf_common_cmd.h"#' \
  "$ROOT/driverlib/rf_ieee_cmd.h"
# (GNU sed: drop the '' after -i)
```

## 3. Build `driverlib.lib`

The Makefile links `$(CC26X0_DRIVERLIB)/bin/gcc/driverlib.lib`. Compile it from
the driverlib sources (their `""` / `../inc/` includes resolve in-tree, so no
extra `-I` is needed beyond `driverlib` and `inc`):

```bash
ROOT="$HOME/Code/ti/cc2640r2-sdk"
cd "$ROOT"
mkdir -p driverlib/bin/gcc
for c in driverlib/*.c; do
  arm-none-eabi-gcc -mcpu=cortex-m3 -mthumb -march=armv7-m -Os \
    -ffunction-sections -fdata-sections -w \
    -I driverlib -I inc -c "$c" -o "${c%.c}.o"
done
arm-none-eabi-ar rcs driverlib/bin/gcc/driverlib.lib driverlib/*.o
# sanity: should print the symbol
arm-none-eabi-nm driverlib/bin/gcc/driverlib.lib | grep NOROM_SetupTrimDevice
```

## 4. Build the firmware

```bash
cd /path/to/oepl-cc2630-firmware
make CC26X0_DIR="$HOME/Code/ti/cc2640r2-sdk"
# -> binaries/Tag_FW_CC2630_TG-GR6000N.bin  (exactly 131072 bytes)
```

Expected size is ~15 KB text and a 128 KB output bin (app + CCFG, gap-filled
0xFF). Quick sanity checks before flashing:

```bash
BIN=binaries/Tag_FW_CC2630_TG-GR6000N.bin
wc -c < "$BIN"                       # 131072
xxd -l 8 "$BIN"                      # SP=0x20005000, reset vector non-zero
xxd -s $((0x1FFDB)) -l 1 "$BIN"      # CCFG byte 51 must be c5 (bootloader enabled)
```

## 5. UART debug self-test build (no J-Link)

To verify the UART TX path / baud on DIO3 with only an FTDI adapter, build with
`UART_TX_SELFTEST` (see `firmware/rtt.c` for the read-out procedure):

```bash
make CC26X0_DIR="$HOME/Code/ti/cc2640r2-sdk" \
  DEFINES="-DCC2630 -DOEPL_TARGET_CC2630 -DOEPL_DISPLAY_UC8159_600X448 -DUART_TX_SELFTEST"
```

If 115200 shows garbage, the UART clock assumption is wrong — find the baud that
decodes the `U` stream cleanly and rebuild with `-DUART_CLK_HZ=<actual>`
(`actual = 48e6 * decoded_baud / 115200`).

## Caveat: RF/radio

The `rf_ieee_cmd.h` added above is version-matched to the same TI driverlib
family, and the firmware only uses a few stable IEEE command structs
(`rfc_CMD_IEEE_RX_t/TX_t`, `rfc_ieeeRxOutput_t`). It compiles and links cleanly,
but the RF-core struct layout vs. the CC2630 ROM has **not** been verified on
hardware. The UART self-test and boot/display paths do not depend on RF, so they
can be validated independently. If you have the official SimpleLink CC2640R2
SDK, prefer its driverlib for production radio use.
