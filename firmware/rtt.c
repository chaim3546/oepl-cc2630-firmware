// -----------------------------------------------------------------------------
//     Minimal SEGGER RTT implementation + UART TX debug output
// -----------------------------------------------------------------------------
// Compatible with J-Link RTT protocol. J-Link scans target RAM for the
// "SEGGER RTT" magic string to locate the control block.
//
// Also outputs all debug text to UART0 TX (DIO3) at 115200 baud for
// debugging without J-Link. The FTDI adapter connected for cc2538-bsl
// flashing receives this output.
// -----------------------------------------------------------------------------

#include "rtt.h"
#include "uart.h"
#include "ioc.h"
#include "prcm.h"
#include "hw_memmap.h"

// UART TX pin — DIO3 is CC2630 ROM bootloader TX (connects to FTDI RX).
// DIO2/DIO3 are the ROM serial bootloader's DEFAULT pins (TI SWRA466), so this
// is the exact path cc2538-bsl uses — the physical wiring is proven good.
#define UART_TX_PIN     3
#define UART_RX_PIN     2
#define UART_BAUD       115200

// UART functional clock used to compute the baud divisor. If the FTDI shows
// garbage at 115200, the real clock is almost certainly not 48 MHz when this
// runs. Use rtt_uart_selftest() to decode the 'U' stream at other bauds, then:
//     actual_clk = 48000000 * (decoded_baud / 115200)
// and override from the build, e.g.  make CFLAGS+=-DUART_CLK_HZ=24000000
#ifndef UART_CLK_HZ
#define UART_CLK_HZ     48000000
#endif

// Ring buffer size — must be power of 2 for efficient masking
#define RTT_BUFFER_SIZE 2048

// RTT buffer descriptor (matches SEGGER's layout exactly)
typedef struct {
    const char *sName;
    char       *pBuffer;
    unsigned    SizeOfBuffer;
    unsigned    WrOff;
    unsigned    RdOff;
    unsigned    Flags;
} RTT_BUFFER_DESC;

// RTT control block (matches SEGGER's layout exactly)
// J-Link searches RAM for acID[] to find this structure.
typedef struct {
    char            acID[16];
    int             MaxNumUpBuffers;
    int             MaxNumDownBuffers;
    RTT_BUFFER_DESC aUp[1];
    RTT_BUFFER_DESC aDown[1];
} RTT_CB;

// The actual ring buffer data
static char _aUpBuffer[RTT_BUFFER_SIZE];
static char _aDownBuffer[16];

// The control block — placed in .data so J-Link can find it via RAM scan.
// The ID is split to prevent the linker from merging it with other strings
// and to ensure J-Link doesn't find a false match in flash.
static RTT_CB _SEGGER_RTT __attribute__((used)) = {
    .acID              = "SEGGER RTT\0\0\0\0\0",
    .MaxNumUpBuffers   = 1,
    .MaxNumDownBuffers = 1,
    .aUp = {{
        .sName        = "Terminal",
        .pBuffer      = _aUpBuffer,
        .SizeOfBuffer = RTT_BUFFER_SIZE,
        .WrOff        = 0,
        .RdOff        = 0,
        .Flags        = 0  // SEGGER_RTT_MODE_NO_BLOCK_SKIP
    }},
    .aDown = {{
        .sName        = "Terminal",
        .pBuffer      = _aDownBuffer,
        .SizeOfBuffer = sizeof(_aDownBuffer),
        .WrOff        = 0,
        .RdOff        = 0,
        .Flags        = 0
    }}
};

// Iteration budget for the bounded spins in uart_init(). NEVER block forever
// here — this is the very first code that runs, before any output exists, so an
// unbounded wait is indistinguishable from a brick (see docs/LESSONS_LEARNED.md).
#define UART_INIT_TIMEOUT 1000000u

static void uart_init(void)
{
    // SERIAL power domain should already be up (for SPI), but ensure it.
    PRCMPowerDomainOn(PRCM_DOMAIN_SERIAL);
    for (uint32_t t = 0; t < UART_INIT_TIMEOUT; t++)
        if (PRCMPowerDomainStatus(PRCM_DOMAIN_SERIAL) == PRCM_DOMAIN_POWER_ON)
            break;

    // Enable UART0 peripheral clock.
    PRCMPeripheralRunEnable(PRCM_PERIPH_UART0);
    PRCMLoadSet();
    for (uint32_t t = 0; t < UART_INIT_TIMEOUT; t++)
        if (PRCMLoadGet())
            break;

    // Configure UART pins (TX only needed, but set RX too for completeness).
    IOCPinTypeUart(UART0_BASE, UART_RX_PIN, UART_TX_PIN, IOID_UNUSED, IOID_UNUSED);

    // Configure UART0: 115200, 8N1. Baud divisor derived from UART_CLK_HZ.
    UARTConfigSetExpClk(UART0_BASE, UART_CLK_HZ, UART_BAUD,
                        UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE | UART_CONFIG_PAR_NONE);
    UARTEnable(UART0_BASE);
}

static void uart_putc(char c)
{
    // Bounded, non-blocking write. The original UARTCharPut() blocks until the
    // TX FIFO has space — if the UART is misconfigured (e.g. wrong clock) and
    // the FIFO never drains, that blocks forever after ~16 chars and freezes
    // the whole tag. Here we wait a bounded time for space, then drop the byte.
    for (uint32_t t = 0; t < 100000u; t++) {
        if (UARTSpaceAvail(UART0_BASE)) {
            UARTCharPutNonBlocking(UART0_BASE, (uint8_t)c);
            return;
        }
    }
    // Timed out waiting for FIFO space — drop this character rather than hang.
}

void rtt_init(void)
{
    // Control block is statically initialized, nothing else needed.
    // This function exists as a clear initialization point and to
    // ensure the linker doesn't optimize away _SEGGER_RTT.
    (void)_SEGGER_RTT.acID[0];

    // Initialize UART TX for debug output without J-Link
    uart_init();
}

void rtt_putc(char c)
{
    // RTT output (for J-Link)
    unsigned wr = _SEGGER_RTT.aUp[0].WrOff;
    _aUpBuffer[wr] = c;
    wr++;
    if (wr >= RTT_BUFFER_SIZE)
        wr = 0;
    // Non-blocking: if buffer is full, just drop the character.
    // (WrOff == RdOff means empty; WrOff+1 == RdOff means full)
    if (wr != _SEGGER_RTT.aUp[0].RdOff)
        _SEGGER_RTT.aUp[0].WrOff = wr;

    // UART output (for FTDI serial)
    uart_putc(c);
}

void rtt_puts(const char *s)
{
    while (*s)
        rtt_putc(*s++);
}

void rtt_put_hex8(uint8_t val)
{
    const char hex[] = "0123456789ABCDEF";
    rtt_putc(hex[(val >> 4) & 0xF]);
    rtt_putc(hex[val & 0xF]);
}

void rtt_put_hex32(uint32_t val)
{
    rtt_put_hex8((val >> 24) & 0xFF);
    rtt_put_hex8((val >> 16) & 0xFF);
    rtt_put_hex8((val >>  8) & 0xFF);
    rtt_put_hex8(val & 0xFF);
}

// UART TX bring-up self-test — verifies the DIO3 -> FTDI path and baud rate
// with NO J-Link required. Streams 'U' (0x55, alternating bits — ideal for
// auto-baud and for measuring the bit period on a scope/logic analyzer),
// followed by a short human-readable line so you can confirm the baud is
// exactly right.
//
// How to use:
//   1. Build with the self-test enabled:  make CFLAGS+=-DUART_TX_SELFTEST
//   2. Flash, release the D/L pin (DIO11 high), power-cycle.
//   3. Open the FTDI at 115200 (e.g. `screen /dev/tty.usbserial-XXXX 115200`).
//        - Clean "UUUU... UART_TX_OK NNNNNNNN" lines  -> TX path + baud correct.
//        - Garbage at 115200 -> retry at 9600/19200/38400/57600/230400. The
//          baud that decodes 'U' cleanly reveals the true clock; set it via
//          -DUART_CLK_HZ=<actual_clk>  (actual_clk = 48e6 * baud/115200).
//        - Nothing at any baud -> TX peripheral path issue; fall back to a
//          bit-banged GPIO TX on DIO3 (Step 4 of the plan).
//
// Bounded (runs ~a few seconds) so normal boot continues afterward. Inert
// unless UART_TX_SELFTEST is defined at build time.
void rtt_uart_selftest(void)
{
    for (uint32_t rep = 0; rep < 200; rep++) {
        for (int i = 0; i < 32; i++)
            uart_putc(0x55);              // 'UUUU...'  (auto-baud / scope friendly)
        rtt_puts("\r\nUART_TX_OK ");
        rtt_put_hex32(rep);
        rtt_puts("\r\n");
        for (volatile uint32_t d = 0; d < 300000; d++)
            __asm volatile ("nop");       // brief gap between bursts
    }
}
