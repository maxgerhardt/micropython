/* machine.SDCard for the CH32H417, on the SDMMC controller.
 *
 *     sd = machine.SDCard()                  # 1-bit, 20 MHz
 *     os.mount(sd, "/sd")
 *
 * The chip has two card controllers. SDIO is the familiar STM32F4-shaped one
 * (POWER/CLKCR/DTIMER/FIFO); SDMMC is WCH's own, does up to 200 MHz, and is
 * the one used here -- it is the better block, and it is the one whose pins
 * this board brings out on adjacent headers. Its signals are not on the
 * numbered AF mux: the pads come from an AFIO remap field, which is why the
 * pins below are configured as plain alternate-function push-pull with no
 * GPIO_PinAFConfig call. The three mappings are:
 *
 *              CK     CMD    D0     D1     D2     D3    D4    D5    D6   D7
 *   default    PC12   PD2    PC8    PC9    PC10   PC11  PA14  PA15  PC6  PC7
 *   partial    PD11   PD12   PB13   PC9    PB10   PB11  PA14  PA15  PC6  PC7
 *   full       PC12   PC10   PD0    PD1    PD2    PD3   PD4   PD5   PD6  PD7
 *
 * Only the default mapping is wired up here. The other two are a one-line
 * change (GPIO_PinRemapConfig) plus a pin table, and nothing else in this
 * file depends on which is selected.
 *
 * Every SDMMC pin is on VIO18 rather than VDDIO, so this needs
 * machine.vio18() to be at 3300; see machine_vio18.c. At the 1.2 V the chip
 * powers up with, a card sees no valid high level at all and CMD0 never even
 * gets a response. The constructor checks rather than letting that happen.
 *
 * Data moves by the controller's own DMA, always through sd_dma_buf. The
 * caller's buffer is essentially never usable directly: it has to be 16-byte
 * aligned and outside DTCM, and DTCM is where .bss and the whole first GC
 * heap area live -- the same limit the USB and Ethernet DMA have. A direct
 * path would be code that never runs.
 */
#include <stdbool.h>
#include <string.h>

#include "ch32h417.h"

#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/runtime.h"

#include "extmod/vfs.h"

#include "machine_pin.h"
#include "machine_vio18.h"

/* Prints every command, its flags and its response. Off in a normal build;
 * the one thing that makes a card that will not identify tractable. */
#ifndef SD_TRACE
#define SD_TRACE (0)
#endif

#define SD_BLOCK_SIZE (512)

/* Pins of the default mapping, in the order the controller uses them. */
#define SD_PIN_CK MACHINE_PIN_ID(2, 12)   /* PC12 */
#define SD_PIN_CMD MACHINE_PIN_ID(3, 2)   /* PD2 */
#define SD_PIN_D0 MACHINE_PIN_ID(2, 8)    /* PC8 */
#define SD_PIN_D1 MACHINE_PIN_ID(2, 9)    /* PC9 */
#define SD_PIN_D2 MACHINE_PIN_ID(2, 10)   /* PC10 */
#define SD_PIN_D3 MACHINE_PIN_ID(2, 11)   /* PC11 */

/* SD commands. ACMDs are sent as CMD55 followed by the command itself. */
enum {
    SD_CMD_GO_IDLE_STATE = 0,
    SD_CMD_ALL_SEND_CID = 2,
    SD_CMD_SEND_RELATIVE_ADDR = 3,
    SD_CMD_SELECT_CARD = 7,
    SD_CMD_SEND_IF_COND = 8,
    SD_CMD_SEND_CSD = 9,
    SD_CMD_STOP_TRANSMISSION = 12,
    SD_CMD_SEND_STATUS = 13,
    SD_CMD_SET_BLOCKLEN = 16,
    SD_CMD_READ_SINGLE_BLOCK = 17,
    SD_CMD_READ_MULTIPLE_BLOCK = 18,
    SD_CMD_WRITE_BLOCK = 24,
    SD_CMD_WRITE_MULTIPLE_BLOCK = 25,
    SD_CMD_APP_CMD = 55,
    SD_ACMD_SET_BUS_WIDTH = 6,
    SD_ACMD_SEND_OP_COND = 41,
};

/* Response shapes, as the CMD_SET register wants them: the length in
 * RPTY[9:8], plus whether the hardware should check the CRC and the echoed
 * command index. R3 carries neither -- the OCR response has no CRC and
 * reserved bits where the index goes -- and R2's index field is reserved too,
 * so checking either one rejects a perfectly good response. */
#define SD_RESP_NONE (0x0000)
#define SD_RESP_R1 (0x0200 | 0x0400 | 0x0800)
#define SD_RESP_R1B (0x0300 | 0x0400 | 0x0800)
#define SD_RESP_R2 (0x0100 | 0x0400)
#define SD_RESP_R3 (0x0200)
#define SD_RESP_R6 SD_RESP_R1
#define SD_RESP_R7 SD_RESP_R1

/* Bits of the R1 card status that mean the command did not do what was asked.
 * Bit 8 (READY_FOR_DATA) and bits 12:9 (CURRENT_STATE) are not errors. */
#define SD_R1_ERROR_MASK (0xFDF98008u)

typedef enum {
    SD_CARD_NONE = 0,
    SD_CARD_SDSC,   /* standard capacity, byte addressed */
    SD_CARD_SDHC,   /* high/extended capacity, block addressed */
} sd_card_type_t;

typedef struct _machine_sdcard_obj_t {
    mp_obj_base_t base;
    uint32_t block_count;
    uint32_t freq;
    uint16_t rca;
    uint8_t width;
    uint8_t card_type;
    bool initialised;
    uint32_t cid[4];
    uint32_t csd[4];
} machine_sdcard_obj_t;

const mp_obj_type_t machine_sdcard_type;

/* The one controller, so the one card. It lives on the heap and nothing else
 * refers to it once Python drops its name, so it has to be a GC root or the
 * next collection frees a live block device out from under a mounted
 * filesystem. */
#define sd_card_obj ((machine_sdcard_obj_t *)MP_STATE_PORT(machine_sdcard_obj))

/* Bounce buffer for transfers the DMA cannot do in place, which in practice
 * is all of them: the GC fills its DTCM area long before it touches the
 * shared-SRAM one, so every bytearray a program is likely to pass in lives
 * where this controller cannot reach. Eight blocks rather than one, so a
 * bounced multi-block transfer stays a multi-block transfer. */
#define SD_BOUNCE_BLOCKS (8)
static uint8_t sd_dma_buf[2][SD_BOUNCE_BLOCKS * SD_BLOCK_SIZE]
__attribute__((aligned(16), section(".sdmmcram")));
static uint8_t sd_dma_half;

#if SD_TRACE
static uint32_t sd_log[24][3];
static int sd_log_n;

static void sd_log_dump(void) {
    for (int i = 0; i < sd_log_n; i++) {
        mp_printf(&mp_plat_print, "  CMD%u fg=%04x r=%08x\n",
            (unsigned int)sd_log[i][0], (unsigned int)sd_log[i][1],
            (unsigned int)sd_log[i][2]);
    }
    sd_log_n = 0;
}
#endif

/* --- controller --- */

/* SDCLK as last programmed, so the inter-command gap can be sized in bus
 * clocks rather than guessed in microseconds. */
static uint32_t sd_clk_hz = 400000;

/* NCC: the SD specification requires at least eight clock cycles between the
 * end of one command's response and the start of the next command, and this
 * controller does not insert them. Without this the card is still releasing
 * the CMD line when the next command starts, and the response that comes back
 * is mangled: CMD55 issued straight after CMD8 reports a response-index error
 * with no CMDDONE at all and the previous command's response still sitting in
 * the register. It reproduces every time at 400 kHz, and disappeared the
 * moment a debug printf was added between commands -- which is what pointed
 * at the spacing rather than at the command.
 *
 * Eight cycles is 20 us at the 400 kHz identification clock and 0.4 us at
 * 20 MHz, so this costs nothing once the bus speeds up. */
static void sd_cmd_gap(void) {
    uint32_t us = (8000000u + sd_clk_hz - 1) / sd_clk_hz;
    mp_hal_delay_us(us < 1 ? 1 : us);
}

/* SDCK = SYSPLL / div in high-speed mode, and SYSPLL / div / 64 in low-speed
 * mode, with div in 2..31. The two ranges do not meet -- at a 400 MHz SYSPLL
 * low mode tops out at 3.1 MHz and high mode bottoms out at 12.9 MHz -- so
 * pick whichever can reach the request, preferring the one that does not
 * overshoot it. Returns the frequency actually programmed. */
static uint32_t sd_set_clock(uint32_t hz) {
    uint32_t src = SystemClock;
    uint32_t div, actual, mode;

    if (hz >= src / 31) {
        div = (src + hz - 1) / hz;
        if (div < 2) {
            div = 2;
        }
        if (div > 31) {
            div = 31;
        }
        mode = SDMMC_CLKMode;
        actual = src / div;
    } else {
        uint32_t low = src / 64;
        div = (low + hz - 1) / hz;
        if (div < 2) {
            div = 2;
        }
        if (div > 31) {
            div = 31;
        }
        mode = 0;
        actual = low / div;
    }

    SDMMC->CLK_DIV = (uint16_t)(mode | div | SDMMC_CLKOE);
    sd_clk_hz = actual;
    return actual;
}

static void sd_pin_af(uint8_t id) {
    machine_pin_clock_enable(id);
    GPIO_InitTypeDef init = { 0 };
    init.GPIO_Pin = MACHINE_PIN_MASK(id);
    init.GPIO_Mode = GPIO_Mode_AF_PP;
    init.GPIO_Speed = GPIO_Speed_Very_High;
    GPIO_Init(machine_pin_gpio(id), &init);
}

static void sd_pin_release(uint8_t id) {
    GPIO_InitTypeDef init = { 0 };
    init.GPIO_Pin = MACHINE_PIN_MASK(id);
    init.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    init.GPIO_Speed = GPIO_Speed_Low;
    GPIO_Init(machine_pin_gpio(id), &init);
}

static void sd_controller_reset(uint8_t width) {
    RCC_HBPeriphClockCmd(RCC_HBPeriph_SDMMC, ENABLE);

    /* SWP_TBYP disables SWPMI's internal transceiver, which releases its
     * signals to the GPIO mux. That matters here because SWPMI shares pads
     * with this controller: SWPMI_IO is PC6, and SWP_TX/RX/SUP are PC7, PC8
     * and PC9 -- which are SDMMC D6, D7, D0 and D1.
     *
     * Measured: 1-bit mode passes its whole suite without this, so the
     * transceiver at its reset setting does not actually hold DAT0 on PC8.
     * It is kept anyway because the vendor's SD_GPIO_Init() does it, because
     * 4-bit mode also needs PC9 and there is no 4-bit wiring here to test it
     * on, and because it costs one register write. The clock stays on
     * afterwards rather than being gated again, since nothing says the bit
     * survives its block being gated. */
    RCC_HB1PeriphClockCmd(RCC_HB1Periph_SWPMI, ENABLE);
    SWPMI->OR |= (1u << 0);

    RCC_HBPeriphResetCmd(RCC_HBPeriph_SDMMC, ENABLE);
    RCC_HBPeriphResetCmd(RCC_HBPeriph_SDMMC, DISABLE);

    /* CONTROL comes out of reset as 0x0015: ALL_CLR and RST_LGC asserted, one
     * data line. Clearing those two is what takes the controller out of
     * reset, so this write is not merely configuration.
     *
     * NEGSMP samples CMD and data on the falling clock edge, which is what
     * the vendor driver uses at every speed. */
    SDMMC->CONTROL = (uint16_t)((width == 4 ? SDMMC_LW_MASK_0 : 0)
        | SDMMC_DMAEN | SDMMC_NEGSMP);
    SDMMC->TIMEOUT = 0x0F;
    SDMMC->INT_EN = 0;
    SDMMC->INT_FG = 0xFFFF;
    SDMMC->TRAN_MODE = 0;
    SDMMC->BLOCK_CFG = 0;
}

/* Wait for the card to release DAT0, which it holds low while it is busy
 * programming. Every transfer starts with this. */
static int sd_wait_dat0(void) {
    mp_uint_t deadline = mp_hal_ticks_ms() + 1000;
    while ((SDMMC->STATUS & SDMMC_DAT0STA) == 0) {
        if (mp_hal_ticks_ms() > deadline) {
            return -MP_ETIMEDOUT;
        }
    }
    return 0;
}

/* Send one command and wait for its response.
 *
 * Returns 0, or -MP_ETIMEDOUT if the card said nothing, or -MP_EIO if the
 * response came back malformed. */
static int sd_cmd(uint8_t idx, uint32_t arg, uint32_t resp) {
    sd_cmd_gap();
    SDMMC->INT_FG = SDMMC_IF_CMDDONE | SDMMC_IF_RE_TMOUT
        | SDMMC_IF_RECRC_WR | SDMMC_IF_REIDX_ER | SDMMC_IF_DATTMO;

    SDMMC->ARGUMENT = arg;
    SDMMC->CMD_SET = (uint16_t)((idx & SDMMC_CMDIDX_MASK) | resp);

    /* Long enough for an R1b whose card holds DAT0 low through an erase; the
     * hardware's own timeout fires well before this and reports a flag. */
    mp_uint_t deadline = mp_hal_ticks_ms() + 1000;
    uint16_t fg;
    for (;;) {
        fg = SDMMC->INT_FG;
        if (fg & (SDMMC_IF_CMDDONE | SDMMC_IF_RE_TMOUT
                  | SDMMC_IF_RECRC_WR | SDMMC_IF_REIDX_ER | SDMMC_IF_DATTMO)) {
            break;
        }
        if (mp_hal_ticks_ms() > deadline) {
            return -MP_ETIMEDOUT;
        }
    }

    #if SD_TRACE
    /* Recorded rather than printed: printing between commands changes the
     * spacing on the bus, which is the very thing worth measuring. */
    if (sd_log_n < (int)MP_ARRAY_SIZE(sd_log)) {
        sd_log[sd_log_n][0] = idx;
        sd_log[sd_log_n][1] = fg;
        sd_log[sd_log_n][2] = SDMMC->RESPONSE3;
        sd_log_n++;
    }
    #endif

    if (resp == SD_RESP_NONE) {
        /* Nothing to wait for beyond the command leaving the pin; CMDDONE is
         * still set for it, and a timeout flag here means nothing. */
        return 0;
    }
    if (fg & (SDMMC_IF_RE_TMOUT | SDMMC_IF_DATTMO)) {
        return -MP_ETIMEDOUT;
    }
    if (fg & (SDMMC_IF_RECRC_WR | SDMMC_IF_REIDX_ER)) {
        return -MP_EIO;
    }
    return 0;
}

/* A 48-bit response lands in RESPONSE3, not RESPONSE0: the registers are
 * numbered from the low end of the 128-bit field and a short response
 * occupies the top of it. Getting this backwards yields plausible-looking
 * zeros from every command, so it is worth stating twice. */
static uint32_t sd_resp_short(void) {
    return SDMMC->RESPONSE3;
}

static void sd_resp_long(uint32_t out[4]) {
    out[0] = SDMMC->RESPONSE3;  /* bits 127:96 */
    out[1] = SDMMC->RESPONSE2;
    out[2] = SDMMC->RESPONSE1;
    out[3] = SDMMC->RESPONSE0;  /* bits 31:0 */
}

/* R1 responses carry the card's status; a command the card refused still
 * answers, so the flags have to be looked at separately from the transport. */
static int sd_cmd_r1(uint8_t idx, uint32_t arg, uint32_t resp) {
    int ret = sd_cmd(idx, arg, resp);
    if (ret != 0) {
        return ret;
    }
    if (sd_resp_short() & SD_R1_ERROR_MASK) {
        return -MP_EIO;
    }
    return 0;
}

static int sd_app_cmd(machine_sdcard_obj_t *self, uint8_t idx, uint32_t arg, uint32_t resp) {
    int ret = sd_cmd_r1(SD_CMD_APP_CMD, (uint32_t)self->rca << 16, SD_RESP_R1);
    if (ret != 0) {
        return ret;
    }
    return sd_cmd(idx, arg, resp);
}

/* --- card bring-up --- */

static uint32_t sd_capacity_blocks(const uint32_t csd[4]) {
    uint32_t structure = csd[0] >> 30;
    if (structure >= 1) {
        /* CSD version 2.0 and 3.0: C_SIZE is bits 69:48 and counts 512 KB
         * units, less one. */
        uint32_t c_size = ((csd[1] & 0x3F) << 16) | (csd[2] >> 16);
        return (c_size + 1) * 1024;
    }
    /* CSD version 1.0: capacity is (C_SIZE + 1) << (C_SIZE_MULT + 2) blocks
     * of READ_BL_LEN bytes, which this normalises to 512-byte blocks.
     * C_SIZE is bits 73:62, C_SIZE_MULT bits 49:47, READ_BL_LEN bits 83:80. */
    uint32_t read_bl_len = (csd[1] >> 16) & 0x0F;
    uint32_t c_size = ((csd[1] & 0x3FF) << 2) | (csd[2] >> 30);
    uint32_t c_size_mult = (csd[2] >> 15) & 0x07;
    uint32_t blocks = (c_size + 1) << (c_size_mult + 2);
    if (read_bl_len > 9) {
        blocks <<= (read_bl_len - 9);
    }
    return blocks;
}

static int sd_card_identify(machine_sdcard_obj_t *self) {
    int ret;

    self->rca = 0;
    self->card_type = SD_CARD_NONE;

    /* 400 kHz for identification, as the spec requires, and at least 74
     * clocks on the bus before the first command. */
    sd_set_clock(400000);
    mp_hal_delay_ms(2);

    ret = sd_cmd(SD_CMD_GO_IDLE_STATE, 0, SD_RESP_NONE);
    if (ret != 0) {
        return ret;
    }
    mp_hal_delay_ms(2);

    /* CMD8 tells a v2 card we can supply 2.7-3.6 V and asks it to echo the
     * check pattern back. Older cards answer nothing at all, which is not an
     * error -- it is how they identify themselves. */
    bool v2 = false;
    if (sd_cmd(SD_CMD_SEND_IF_COND, 0x1AA, SD_RESP_R7) == 0) {
        if ((sd_resp_short() & 0xFFF) != 0x1AA) {
            return -MP_EIO;
        }
        v2 = true;
    } else {
        /* A card that ignored CMD8 has left the controller unhappy; start
         * over so ACMD41 sees a clean state. */
        ret = sd_cmd(SD_CMD_GO_IDLE_STATE, 0, SD_RESP_NONE);
        if (ret != 0) {
            return ret;
        }
        mp_hal_delay_ms(2);
    }

    /* ACMD41 until the card finishes its power-up. HCS (bit 30) is only
     * meaningful to a v2 card and asks for block addressing. */
    uint32_t ocr = 0;
    mp_uint_t deadline = mp_hal_ticks_ms() + 1000;
    for (;;) {
        ret = sd_app_cmd(self, SD_ACMD_SEND_OP_COND,
            (v2 ? 0x40000000u : 0u) | 0x00FF8000u, SD_RESP_R3);
        if (ret != 0) {
            return ret;
        }
        ocr = sd_resp_short();
        if (ocr & 0x80000000u) {
            break;
        }
        if (mp_hal_ticks_ms() > deadline) {
            return -MP_ETIMEDOUT;
        }
        mp_hal_delay_ms(1);
    }
    self->card_type = (ocr & 0x40000000u) ? SD_CARD_SDHC : SD_CARD_SDSC;

    ret = sd_cmd(SD_CMD_ALL_SEND_CID, 0, SD_RESP_R2);
    if (ret != 0) {
        return ret;
    }
    sd_resp_long(self->cid);

    ret = sd_cmd(SD_CMD_SEND_RELATIVE_ADDR, 0, SD_RESP_R6);
    if (ret != 0) {
        return ret;
    }
    self->rca = (uint16_t)(sd_resp_short() >> 16);
    if (self->rca == 0) {
        return -MP_EIO;
    }

    ret = sd_cmd(SD_CMD_SEND_CSD, (uint32_t)self->rca << 16, SD_RESP_R2);
    if (ret != 0) {
        return ret;
    }
    sd_resp_long(self->csd);
    self->block_count = sd_capacity_blocks(self->csd);

    ret = sd_cmd_r1(SD_CMD_SELECT_CARD, (uint32_t)self->rca << 16, SD_RESP_R1B);
    if (ret != 0) {
        return ret;
    }

    if (self->width == 4) {
        ret = sd_app_cmd(self, SD_ACMD_SET_BUS_WIDTH, 2, SD_RESP_R1);
        if (ret != 0) {
            return ret;
        }
        SDMMC->CONTROL |= SDMMC_LW_MASK_0;
    }

    /* Standard-capacity cards address by byte and can be told to use a block
     * length other than 512; high-capacity cards are fixed at 512 and ignore
     * this. Sending it either way keeps the two paths identical below. */
    ret = sd_cmd_r1(SD_CMD_SET_BLOCKLEN, SD_BLOCK_SIZE, SD_RESP_R1);
    if (ret != 0) {
        return ret;
    }

    self->freq = sd_set_clock(self->freq);
    return 0;
}

/* --- data transfer --- */

/* Wait for the card to leave the programming state after a write. The
 * controller's own R1b handling covers the busy signalling on DAT0, but a
 * multi-block write ends with CMD12 and the card can still be programming
 * when the next command arrives. */
static int sd_wait_ready(machine_sdcard_obj_t *self) {
    mp_uint_t deadline = mp_hal_ticks_ms() + 1000;
    for (;;) {
        int ret = sd_cmd(SD_CMD_SEND_STATUS, (uint32_t)self->rca << 16, SD_RESP_R1);
        if (ret != 0) {
            return ret;
        }
        uint32_t status = sd_resp_short();
        if (status & SD_R1_ERROR_MASK) {
            return -MP_EIO;
        }
        if (status & (1u << 8)) {   /* READY_FOR_DATA */
            return 0;
        }
        if (mp_hal_ticks_ms() > deadline) {
            return -MP_ETIMEDOUT;
        }
    }
}

/* Wait for one data phase to reach a flag worth acting on. Returns the flag
 * word, or 0 if the deadline passed first. */
static uint16_t sd_wait_data_flag(void) {
    mp_uint_t deadline = mp_hal_ticks_ms() + 2000;
    for (;;) {
        uint16_t fg = SDMMC->INT_FG & (SDMMC_IF_TRANDONE | SDMMC_IF_BKGAP
            | SDMMC_IF_TRANERR | SDMMC_IF_DATTMO | SDMMC_IF_FIFO_OV);
        if (fg != 0) {
            return fg;
        }
        if (mp_hal_ticks_ms() > deadline) {
            return 0;
        }
    }
}

static int sd_data_flag_error(uint16_t fg) {
    if (fg == 0) {
        return -MP_ETIMEDOUT;
    }
    if (fg & SDMMC_IF_DATTMO) {
        return -MP_ETIMEDOUT;
    }
    if (fg & (SDMMC_IF_TRANERR | SDMMC_IF_FIFO_OV)) {
        return -MP_EIO;
    }
    return 0;
}

/* Blocks the controller has transferred so far in this run. The SDK's
 * sd_blocks_done() reads the same field, but nothing else here
 * needs the SDK's SDMMC unit, so it is not linked in. */
static uint32_t sd_blocks_done(void) {
    return SDMMC->STATUS & SDMMC_MASK_BLOCK_NUM;
}

/* Arm the data engine: reset it, set the direction, then the geometry, then
 * the address -- and the address write is also the trigger.
 *
 * The zero write to BLOCK_CFG is not idempotent configuration, it is the
 * reset of the block counter, and skipping it leaves a transfer inheriting
 * the previous one's state. */
static void sd_arm(const uint8_t *buf, uint32_t nblocks, bool write) {
    /* Order the memcpy that filled the buffer against the register writes
     * that hand it to another bus master. This was not what fixed the write
     * path -- adding it changed nothing -- but handing a buffer to a DMA
     * engine without a barrier is not something to leave to luck. */
    __asm volatile ("fence" ::: "memory");
    SDMMC->BLOCK_CFG = 0;
    SDMMC->TRAN_MODE = write ? SDMMC_DMA_DIR : 0;
    SDMMC->BLOCK_CFG = (SD_BLOCK_SIZE << 16) | nblocks;
    SDMMC->DMA_BEG1 = (uint32_t)buf;
}

/* A read is armed before its command, because the card starts sending as soon
 * as it has answered. Then it runs to completion on its own. */
static int sd_read_data(uint32_t nblocks) {
    for (;;) {
        uint16_t fg = sd_wait_data_flag();
        int err = sd_data_flag_error(fg);
        if (err != 0) {
            return err;
        }
        if (fg & SDMMC_IF_TRANDONE) {
            return 0;
        }
        /* BKGAP after each block. On a single-block read the two can arrive
         * together or BKGAP can arrive alone, so it counts as done there. */
        if (nblocks == 1 && (fg & SDMMC_IF_BKGAP)) {
            return 0;
        }
        SDMMC->INT_FG = SDMMC_IF_BKGAP;
    }
}

/* A write is armed *after* its command, and every block after the first has
 * to be kicked off by hand.
 *
 * Both halves are load-bearing and neither is symmetric with the read path.
 * Arming a write before its command -- which is what a read needs, and what
 * symmetry suggests -- makes the command itself time out. And writing
 * DMA_BEG1 once for the whole run sends the first block and stops: the
 * reference manual says a multi-block write continues only when WRITE_CONT or
 * DMA_BEG1 is written again. */
static int sd_write_data(uint32_t nblocks) {
    /* The block counter still reads the previous transfer's total until the
     * engine picks up the new arming, so wait for it to fall back to zero
     * before believing it. Without this the loop below sees a count that
     * already satisfies it, returns while the block is still on the wire, and
     * the next command lands in the middle of a transfer and times out --
     * with the write itself having reported success. The vendor driver opens
     * its own loop with the same wait. */
    mp_uint_t deadline = mp_hal_ticks_ms() + 200;
    while (sd_blocks_done() != 0) {
        if (mp_hal_ticks_ms() > deadline) {
            return -MP_ETIMEDOUT;
        }
    }

    while (sd_blocks_done() < nblocks) {
        uint16_t fg = sd_wait_data_flag();
        int err = sd_data_flag_error(fg);
        if (err != 0) {
            return err;
        }
        SDMMC->INT_FG = fg;
        if (sd_blocks_done() >= nblocks) {
            break;
        }
        SDMMC->WRITE_CONT = 0;
    }
    SDMMC->BLOCK_CFG = 0;
    return 0;
}

/* One contiguous run of blocks, straight into or out of `buf`, which has
 * already been checked to be somewhere the DMA can reach. */
static int sd_transfer_dma(machine_sdcard_obj_t *self, uint32_t block,
    uint8_t *buf, uint32_t nblocks, bool write) {
    uint32_t addr = self->card_type == SD_CARD_SDHC ? block : block * SD_BLOCK_SIZE;
    int ret;

    /* The card holds DAT0 low while it is still programming a previous write;
     * starting a transfer into that produces a data timeout. */
    ret = sd_wait_dat0();
    if (ret != 0) {
        return ret;
    }
    SDMMC->INT_FG = 0xFFFF;

    if (write) {
        ret = sd_cmd_r1(nblocks == 1 ? SD_CMD_WRITE_BLOCK : SD_CMD_WRITE_MULTIPLE_BLOCK,
            addr, SD_RESP_R1);
        if (ret != 0) {
            return ret;
        }
        sd_arm(buf, nblocks, true);
        ret = sd_write_data(nblocks);
    } else {
        sd_arm(buf, nblocks, false);
        ret = sd_cmd_r1(nblocks == 1 ? SD_CMD_READ_SINGLE_BLOCK : SD_CMD_READ_MULTIPLE_BLOCK,
            addr, SD_RESP_R1);
        if (ret != 0) {
            return ret;
        }
        ret = sd_read_data(nblocks);
    }

    if (nblocks > 1) {
        /* CMD12 ends an open-ended multi-block transfer. It is owed even when
         * the transfer failed -- more so, in fact, since the card is then
         * still streaming. */
        int stop = sd_cmd_r1(SD_CMD_STOP_TRANSMISSION, 0, SD_RESP_R1B);
        if (ret == 0) {
            ret = stop;
        }
    }
    if (ret == 0 && write) {
        ret = sd_wait_ready(self);
    }
    return ret;
}

static int sd_transfer(machine_sdcard_obj_t *self, uint32_t block,
    uint8_t *buf, uint32_t nblocks, bool write) {
    if (!self->initialised) {
        return -MP_ENODEV;
    }
    if (block + nblocks > self->block_count) {
        return -MP_EFAULT;
    }
    if (nblocks == 0) {
        return 0;
    }

    /* Always through a bounce buffer, and alternating between two of them.
     *
     * Both halves of that are deliberate. Every buffer a Python program is
     * likely to hand over is in DTCM, which this DMA cannot reach at all, so
     * the copy is unavoidable in practice and a direct path would be dead
     * code that is never exercised. And the alternation is what makes the
     * engine reload its address: writing DMA_BEG1 with the value it already
     * holds starts a transfer without moving the pointer back to the start of
     * the buffer, so a second transfer from the same address sends whatever
     * followed the first one. Two buffers used in turn mean the register
     * always changes. */
    for (uint32_t done = 0; done < nblocks;) {
        uint32_t n = nblocks - done;
        if (n > SD_BOUNCE_BLOCKS) {
            n = SD_BOUNCE_BLOCKS;
        }
        uint8_t *chunk = buf + done * SD_BLOCK_SIZE;
        uint8_t *dma = sd_dma_buf[sd_dma_half];
        sd_dma_half ^= 1;
        if (write) {
            memcpy(dma, chunk, n * SD_BLOCK_SIZE);
        }
        int ret = sd_transfer_dma(self, block + done, dma, n, write);
        if (ret != 0) {
            return ret;
        }
        if (!write) {
            memcpy(chunk, dma, n * SD_BLOCK_SIZE);
        }
        done += n;
    }
    return 0;
}

/* --- object --- */

static void sd_deinit(machine_sdcard_obj_t *self) {
    if (!self->initialised) {
        return;
    }
    self->initialised = false;
    SDMMC->CLK_DIV = 0;
    RCC_HBPeriphResetCmd(RCC_HBPeriph_SDMMC, ENABLE);
    RCC_HBPeriphResetCmd(RCC_HBPeriph_SDMMC, DISABLE);
    RCC_HBPeriphClockCmd(RCC_HBPeriph_SDMMC, DISABLE);

    sd_pin_release(SD_PIN_CK);
    sd_pin_release(SD_PIN_CMD);
    sd_pin_release(SD_PIN_D0);
    if (self->width == 4) {
        sd_pin_release(SD_PIN_D1);
        sd_pin_release(SD_PIN_D2);
        sd_pin_release(SD_PIN_D3);
    }
}

void machine_sdcard_deinit_all(void) {
    if (sd_card_obj != NULL) {
        sd_deinit(sd_card_obj);
        MP_STATE_PORT(machine_sdcard_obj) = NULL;
    }
}

static mp_obj_t machine_sdcard_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_slot, ARG_width, ARG_freq };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_slot,  MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 1} },
        { MP_QSTR_width, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 1} },
        { MP_QSTR_freq,  MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 20000000} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (args[ARG_slot].u_int != 1) {
        mp_raise_ValueError(MP_ERROR_TEXT("there is one SDMMC controller, so one slot"));
    }
    if (args[ARG_width].u_int != 1 && args[ARG_width].u_int != 4) {
        mp_raise_ValueError(MP_ERROR_TEXT("width must be 1 or 4"));
    }
    if (args[ARG_freq].u_int < 100000) {
        mp_raise_ValueError(MP_ERROR_TEXT("freq is too low to identify a card"));
    }
    if (ch32_vio18_get() < CH32_VIO18_2V5) {
        /* Every SDMMC pin is on VIO18. Below 2.5 V a 3.3 V card reads the
         * clock and command lines as permanently low and nothing responds --
         * a failure that looks like bad wiring and is not. */
        mp_raise_msg(&mp_type_OSError,
            MP_ERROR_TEXT("SDMMC pins are on VIO18; call machine.vio18(3300) first"));
    }

    machine_sdcard_obj_t *self = sd_card_obj;
    if (self == NULL) {
        self = mp_obj_malloc(machine_sdcard_obj_t, &machine_sdcard_type);
        MP_STATE_PORT(machine_sdcard_obj) = self;
    } else {
        sd_deinit(self);
    }
    self->width = (uint8_t)args[ARG_width].u_int;
    self->freq = (uint32_t)args[ARG_freq].u_int;
    self->block_count = 0;

    sd_pin_af(SD_PIN_CK);
    sd_pin_af(SD_PIN_CMD);
    sd_pin_af(SD_PIN_D0);
    if (self->width == 4) {
        sd_pin_af(SD_PIN_D1);
        sd_pin_af(SD_PIN_D2);
        sd_pin_af(SD_PIN_D3);
    }
    sd_controller_reset(self->width);

    int ret = sd_card_identify(self);
    #if SD_TRACE
    sd_log_dump();
    #endif
    if (ret != 0) {
        sd_deinit(self);
        mp_raise_OSError(-ret);
    }
    self->initialised = true;
    return MP_OBJ_FROM_PTR(self);
}

static void machine_sdcard_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    machine_sdcard_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->initialised) {
        mp_printf(print, "SDCard(deinit)");
        return;
    }
    mp_printf(print, "SDCard(%s, %u blocks, width=%u, freq=%u)",
        self->card_type == SD_CARD_SDHC ? "SDHC" : "SDSC",
        (unsigned int)self->block_count, self->width, (unsigned int)self->freq);
}

static mp_obj_t machine_sdcard_info(mp_obj_t self_in) {
    machine_sdcard_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->initialised) {
        return mp_const_none;
    }
    mp_obj_t tuple[3] = {
        mp_obj_new_int_from_ull((uint64_t)self->block_count * SD_BLOCK_SIZE),
        MP_OBJ_NEW_SMALL_INT(SD_BLOCK_SIZE),
        MP_OBJ_NEW_SMALL_INT(self->card_type),
    };
    return mp_obj_new_tuple(3, tuple);
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_sdcard_info_obj, machine_sdcard_info);

static mp_obj_t machine_sdcard_cid(mp_obj_t self_in) {
    machine_sdcard_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_bytes((const uint8_t *)self->cid, sizeof(self->cid));
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_sdcard_cid_obj, machine_sdcard_cid);

static mp_obj_t machine_sdcard_csd(mp_obj_t self_in) {
    machine_sdcard_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_bytes((const uint8_t *)self->csd, sizeof(self->csd));
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_sdcard_csd_obj, machine_sdcard_csd);

static mp_obj_t machine_sdcard_deinit(mp_obj_t self_in) {
    sd_deinit(MP_OBJ_TO_PTR(self_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_sdcard_deinit_obj, machine_sdcard_deinit);

static mp_obj_t machine_sdcard_readblocks(mp_obj_t self_in, mp_obj_t block_num, mp_obj_t buf_in) {
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(buf_in, &bufinfo, MP_BUFFER_WRITE);
    if (bufinfo.len % SD_BLOCK_SIZE != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("buffer must be a whole number of blocks"));
    }
    int ret = sd_transfer(MP_OBJ_TO_PTR(self_in), mp_obj_get_int(block_num),
        bufinfo.buf, bufinfo.len / SD_BLOCK_SIZE, false);
    return MP_OBJ_NEW_SMALL_INT(ret);
}
static MP_DEFINE_CONST_FUN_OBJ_3(machine_sdcard_readblocks_obj, machine_sdcard_readblocks);

static mp_obj_t machine_sdcard_writeblocks(mp_obj_t self_in, mp_obj_t block_num, mp_obj_t buf_in) {
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(buf_in, &bufinfo, MP_BUFFER_READ);
    if (bufinfo.len % SD_BLOCK_SIZE != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("buffer must be a whole number of blocks"));
    }
    int ret = sd_transfer(MP_OBJ_TO_PTR(self_in), mp_obj_get_int(block_num),
        bufinfo.buf, bufinfo.len / SD_BLOCK_SIZE, true);
    return MP_OBJ_NEW_SMALL_INT(ret);
}
static MP_DEFINE_CONST_FUN_OBJ_3(machine_sdcard_writeblocks_obj, machine_sdcard_writeblocks);

static mp_obj_t machine_sdcard_ioctl(mp_obj_t self_in, mp_obj_t cmd_in, mp_obj_t arg_in) {
    machine_sdcard_obj_t *self = MP_OBJ_TO_PTR(self_in);
    (void)arg_in;
    switch (mp_obj_get_int(cmd_in)) {
        case MP_BLOCKDEV_IOCTL_INIT:
            return MP_OBJ_NEW_SMALL_INT(self->initialised ? 0 : -1);
        case MP_BLOCKDEV_IOCTL_DEINIT:
            /* Not sd_deinit(): os.umount() issues this, and tearing the
             * controller down there would make a remount fail. The card has
             * no write cache to flush, so there is nothing owed. */
            return MP_OBJ_NEW_SMALL_INT(0);
        case MP_BLOCKDEV_IOCTL_SYNC:
            return MP_OBJ_NEW_SMALL_INT(0);
        case MP_BLOCKDEV_IOCTL_BLOCK_COUNT:
            return MP_OBJ_NEW_SMALL_INT(self->block_count);
        case MP_BLOCKDEV_IOCTL_BLOCK_SIZE:
            return MP_OBJ_NEW_SMALL_INT(SD_BLOCK_SIZE);
        default:
            return mp_const_none;
    }
}
static MP_DEFINE_CONST_FUN_OBJ_3(machine_sdcard_ioctl_obj, machine_sdcard_ioctl);

static const mp_rom_map_elem_t machine_sdcard_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_info), MP_ROM_PTR(&machine_sdcard_info_obj) },
    { MP_ROM_QSTR(MP_QSTR_cid), MP_ROM_PTR(&machine_sdcard_cid_obj) },
    { MP_ROM_QSTR(MP_QSTR_csd), MP_ROM_PTR(&machine_sdcard_csd_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&machine_sdcard_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&machine_sdcard_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_readblocks), MP_ROM_PTR(&machine_sdcard_readblocks_obj) },
    { MP_ROM_QSTR(MP_QSTR_writeblocks), MP_ROM_PTR(&machine_sdcard_writeblocks_obj) },
    { MP_ROM_QSTR(MP_QSTR_ioctl), MP_ROM_PTR(&machine_sdcard_ioctl_obj) },
};
static MP_DEFINE_CONST_DICT(machine_sdcard_locals_dict, machine_sdcard_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    machine_sdcard_type,
    MP_QSTR_SDCard,
    MP_TYPE_FLAG_NONE,
    make_new, machine_sdcard_make_new,
    print, machine_sdcard_print,
    locals_dict, &machine_sdcard_locals_dict
    );

MP_REGISTER_ROOT_POINTER(void *machine_sdcard_obj);
