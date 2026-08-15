/* machine.I2S on the I2S2/I2S3 peripherals.
 *
 * Most of the class is shared code in extmod/machine_i2s.c; this file supplies
 * the hardware half, named by MICROPY_PY_MACHINE_I2S_INCLUDEFILE.
 *
 *     from machine import I2S, Pin
 *     i2s = I2S(0, sck=Pin("PB13"), ws=Pin("PB12"), sd=Pin("PB15"),
 *               mode=I2S.TX, bits=16, format=I2S.STEREO, rate=44100, ibuf=16384)
 *     i2s.write(pcm)
 *
 * I2S(0) is I2S2 (on SPI2) and I2S(1) is I2S3 (on SPI3). They are independent:
 * unlike the SAI blocks this replaced, neither is synchronous to the other and
 * either can be used alone.
 *
 * ### Why not the SAI
 *
 * This port drove machine.I2S from the SAI until now, because every I2S2/I2S3
 * pin sits in the VIO18 domain and that measured 1.2 V -- unusable against a
 * 3.3 V audio device. VIO18 turned out to be a software-set rail (see
 * machine_vio18.c); at 3.3 V these pins are ordinary 3.3 V pins and the
 * original objection is void.
 *
 * Two things are better here. The rate divider is I2SDIV[7:0] plus an ODD
 * half-step, against the SAI's 6-bit MCKDIV, so 44.1 kHz lands within 0.16%
 * (about 3 cents) instead of over 1% (about 21 cents). And the clock source is
 * selected per peripheral rather than shared with SYSCLK's own tree.
 *
 * The SAI driver is kept verbatim at docs/hw/sai-i2s-driver-reference.c.txt,
 * including its hard-won notes on frame-sync polarity and DMA ordering, for
 * whenever SAI is worth re-adding as its own class.
 */

/* stdlib.h and string.h are needed by extmod/machine_i2s.c, which #includes
 * this file rather than the other way round -- dropping them breaks abs() and
 * memset() in code that is not even in this file. irq.h supplies
 * CH32_IRQ_HANDLER, without which the handler declarations below parse as
 * K&R-style definitions and fail on a strict compiler. */
#include <stdlib.h>
#include <string.h>

#include "py/mphal.h"
#include "py/runtime.h"

#include "ch32h417.h"
#include "irq.h"
#include "machine_pin.h"

/* Two peripherals: I2S2 and I2S3. */
#define MAX_I2S_CH32 (2)

/* 512 bytes, halved into the two ping-pong regions the DMA alternates between.
 * At 44.1 kHz stereo 16-bit that is 2.9 ms per full buffer, so about 690
 * interrupts a second -- small enough to hide a garbage collection behind and
 * large enough not to spend the core in interrupt entry. */
#define SIZEOF_DMA_BUFFER_IN_BYTES (512)
#define SIZEOF_HALF_DMA_BUFFER_IN_BYTES (SIZEOF_DMA_BUFFER_IN_BYTES / 2)

/* DMA1 channels 2 and 3 belong to machine.SPI and 1 to machine.AudioOut; 4 and
 * 5 are ours. One channel per peripheral is enough: an I2S object is either RX
 * or TX, never both. */
#define I2S_DMA_A_FLAGS ((uint32_t)0x0000F000)   /* channel 4 nibble */
#define I2S_DMA_A_HT ((uint32_t)0x00004000)
#define I2S_DMA_B_FLAGS ((uint32_t)0x000F0000)   /* channel 5 nibble */
#define I2S_DMA_B_HT ((uint32_t)0x00040000)

/* DMAMUX request numbers, reference manual table 10-2. */
#define I2S_DMA_REQ_2_TX (65)
#define I2S_DMA_REQ_2_RX (66)
#define I2S_DMA_REQ_3_TX (67)
#define I2S_DMA_REQ_3_RX (68)

/* I2SDIV is 8 bits and must be at least 2; ODD adds the half step, so the
 * effective divisor 2*I2SDIV+ODD runs from 4 to 511. */
#define I2S_DIV_MIN (2)
#define I2S_DIV_MAX (255)

/* In non-blocking mode the ring buffer is drained/filled faster than the DMA
 * moves data, so a slow caller cannot make it underflow. */
#define NON_BLOCKING_RATE_MULTIPLIER (4)
#define SIZEOF_NON_BLOCKING_COPY_IN_BYTES (SIZEOF_HALF_DMA_BUFFER_IN_BYTES * NON_BLOCKING_RATE_MULTIPLIER)

/* Receive always captures 32-bit stereo, whatever the user asked for, and
 * extmod/machine_i2s.c reshapes it on the way out through this map. So the
 * ring buffer always holds 8-byte frames on the RX side, and -1 means "this
 * output byte comes from nowhere".
 *
 * The half-words of a 32-bit sample arrive in the opposite order to the one
 * they need in memory, which is what makes this table differ from the version
 * in ports/stm32. The data register is 16 bits wide, so one sample is two
 * transfers, and the bus carries the slot most significant half first -- the
 * DMA therefore lands slot[31:16] in src[0..1] and slot[15:0] in src[2..3],
 * while a little-endian 32-bit sample wants exactly the reverse. The SAI this
 * port used previously had a 32-bit register and moved a whole sample per
 * transfer, so the question never arose there.
 *
 * Measured rather than reasoned: an INMP441 (24 bits left-justified in a
 * 32-bit slot, so the low byte should barely move) gave 93 distinct low bytes
 * as captured and 2 with the halves exchanged, with 1024/1024 samples fitting
 * in 24 bits instead of 1022. The 16-bit rows follow from the same ordering --
 * the most significant half of the slot is src[0..1], not src[2..3]. */
static const int8_t i2s_frame_map[NUM_I2S_USER_FORMATS][I2S_RX_FRAME_SIZE_IN_BYTES] = {
    {  0,  1, -1, -1, -1, -1, -1, -1 },  // Mono, 16-bit
    {  2,  3,  0,  1, -1, -1, -1, -1 },  // Mono, 32-bit
    {  0,  1, -1, -1,  2,  3, -1, -1 },  // Stereo, 16-bit
    {  2,  3,  0,  1,  6,  7,  4,  5 },  // Stereo, 32-bit
};

static int8_t get_frame_mapping_index(int8_t bits, format_t format) {
    if (format == MONO) {
        return bits == 16 ? 0 : 1;
    }
    return bits == 16 ? 2 : 3;
}

/* Owned by the port rather than extmod, and named by mpconfigport.h through
 * MICROPY_PY_MACHINE_I2S_CONSTANT_RX/TX to reach the Python-visible I2S.RX and
 * I2S.TX. */
typedef enum {
    RX,
    TX
} i2s_mode_t;

typedef struct _i2s_pin_t {
    mp_hal_pin_obj_t pin;
    uint8_t af;
} i2s_pin_t;

typedef struct _machine_i2s_obj_t {
    mp_obj_base_t base;
    uint8_t i2s_id;
    mp_hal_pin_obj_t sck;
    mp_hal_pin_obj_t ws;
    mp_hal_pin_obj_t sd;
    i2s_mode_t mode;
    int8_t bits;
    format_t format;
    int32_t rate;
    int32_t ibuf;
    mp_obj_t callback_for_non_blocking;
    io_mode_t io_mode;
    SPI_TypeDef *spi;
    DMA_Channel_TypeDef *dma;
    uint32_t dma_flags;
    uint32_t dma_ht_flag;
    IRQn_Type dma_irqn;
    int32_t actual_rate;              // what the divider could really produce
    bool slave;                       // SCK and WS come from outside
    /* Explicitly aligned, not incidentally so. The DMA moves half-words in and
     * out of here and the transmit path swaps them a half-word at a time, both
     * of which fault or misbehave on an odd address -- but the element type is
     * uint8_t, so the compiler is free to place it anywhere and will do
     * exactly that the moment a single-byte field is added above it. That is
     * not hypothetical: adding the slave flag put this buffer on an odd
     * address and the first transmit died with a misaligned load. */
    uint8_t dma_buffer[SIZEOF_DMA_BUFFER_IN_BYTES] __attribute__((aligned(4)));
    ring_buf_t ring_buffer;
    uint8_t *ring_buffer_storage;
    non_blocking_descriptor_t non_blocking_descriptor;
} machine_i2s_obj_t;

static machine_i2s_obj_t *machine_i2s_active[MAX_I2S_CH32];

/* Armed by ch32.i2s_slave(id, True), consulted by the next I2S() on that id.
 *
 * A slave takes SCK and WS from its pins instead of driving them, which the
 * hardware supports on either direction but machine.I2S has no argument for --
 * upstream models one master talking to a codec. It lives here as a port hook
 * rather than an extra keyword so extmod/machine_i2s.c stays untouched.
 *
 * What it is for: pointing one of this port's I2S blocks at the other, which
 * is the only way to check the transmit side on a board with no I2S input
 * device. Two masters wired together would drive SCK and WS against each
 * other, and without a shared WS the receiver frames on an arbitrary
 * boundary -- a 16-bit offset there is indistinguishable from the half-word
 * order the loopback exists to measure.
 *
 * Known limitation, measured: a slave receiver samples one SCK earlier than
 * a Philips master transmits, so every sample arrives shifted right by one
 * bit (0x11223300 came back as 0x08911980). Both ends read back identical
 * I2SSTD bits, so the delay that Philips puts between the WS edge and the
 * MSB is not being honoured in slave mode. The direction of the shift says
 * it is the slave that is early, not the master that is late, which is why
 * master mode -- the only mode machine.I2S itself can reach, and the one
 * validated against a real microphone -- is unaffected. Good enough for the
 * byte-order measurement it exists for; fix before offering slave mode as a
 * feature. */
static bool machine_i2s_slave_next[MAX_I2S_CH32];

/* Pin options, from datasheet tables 2-2-8. Every one of these is a 3.3 V pin
 * now that VIO18 is set to 3.3 V at boot; before that none of them were, which
 * is why this port used the SAI instead. */
static const i2s_pin_t i2s_ws_pins_2[] = {
    { &pin_B12_obj, GPIO_AF5 }, { &pin_B9_obj, GPIO_AF5 },
    { &pin_A11_obj, GPIO_AF5 }, { &pin_B4_obj, GPIO_AF7 },
};
static const i2s_pin_t i2s_sck_pins_2[] = {
    { &pin_B13_obj, GPIO_AF5 }, { &pin_B10_obj, GPIO_AF5 },
    { &pin_A9_obj, GPIO_AF5 }, { &pin_A12_obj, GPIO_AF5 },
    { &pin_D3_obj, GPIO_AF5 },
};
static const i2s_pin_t i2s_sd_pins_2[] = {
    { &pin_B15_obj, GPIO_AF5 }, { &pin_C1_obj, GPIO_AF5 },
    { &pin_C3_obj, GPIO_AF5 },
};

static const i2s_pin_t i2s_ws_pins_3[] = {
    { &pin_A4_obj, GPIO_AF6 }, { &pin_A15_obj, GPIO_AF6 },
};
static const i2s_pin_t i2s_sck_pins_3[] = {
    { &pin_B3_obj, GPIO_AF6 }, { &pin_C10_obj, GPIO_AF6 },
    { &pin_A14_obj, GPIO_AF1 },
};
static const i2s_pin_t i2s_sd_pins_3[] = {
    { &pin_B2_obj, GPIO_AF7 }, { &pin_B5_obj, GPIO_AF7 },
    { &pin_C12_obj, GPIO_AF6 }, { &pin_D6_obj, GPIO_AF5 },
    { &pin_A13_obj, GPIO_AF1 },
};

#define I2S_PIN_COUNT(t) (sizeof(t) / sizeof((t)[0]))

static uint8_t i2s_find_af(const i2s_pin_t *table, size_t len, mp_hal_pin_obj_t pin) {
    for (size_t i = 0; i < len; i++) {
        if (table[i].pin == pin) {
            return table[i].af;
        }
    }
    mp_raise_ValueError(MP_ERROR_TEXT("pin cannot be used for this I2S signal"));
    return 0;
}

/******************************************************************************/
// Clocking

/* Sample rate is I2SxCLK / (base * (2 * I2SDIV + ODD)) with the master clock
 * output disabled, where base is 32 for a 16-bit channel and 64 for a 32-bit
 * one. I2SDIV is a whole byte and ODD contributes a half step, so the divisor
 * moves in steps of one part in a few hundred -- fine enough that 44.1 kHz
 * lands about 3 cents sharp rather than the SAI's 21.
 *
 * The object reports what it actually got rather than echoing the request --
 * print(i2s) shows both, and ch32.i2s_actual_rate() returns it -- because
 * silently running a fraction of a percent fast is exactly the sort of thing
 * that turns into a pitch-shifted recording nobody can account for. */
static uint8_t i2s_channel_bits(machine_i2s_obj_t *self) {
    /* Receive always runs 32-bit channels because extmod's frame map expects
     * 8-byte frames and narrows them itself; transmit uses what was asked. */
    return self->mode == RX ? 32 : (uint8_t)self->bits;
}

static uint32_t i2s_set_rate(machine_i2s_obj_t *self) {
    RCC_ClocksTypeDef clocks;
    RCC_GetClocksFreq(&clocks);
    uint32_t clk = clocks.SYSCLK_Frequency;
    uint32_t base = (i2s_channel_bits(self) == 16) ? 32u : 64u;

    /* Nearest whole divisor, then split into the byte and the half step. */
    uint32_t n = (clk + (base * (uint32_t)self->rate) / 2u) / (base * (uint32_t)self->rate);
    if (n < I2S_DIV_MIN * 2u) {
        n = I2S_DIV_MIN * 2u;
    }
    if (n > I2S_DIV_MAX * 2u + 1u) {
        mp_raise_ValueError(MP_ERROR_TEXT("rate too low for this clock"));
    }

    self->actual_rate = (int32_t)(clk / (base * n));
    return n;
}

/******************************************************************************/
// DMA

/* Receive: raw bytes straight into the ring buffer. No format conversion here
 * -- the hardware is always 32-bit stereo on this path and i2s_frame_map does
 * the reshaping later. Dropping the whole half when it will not fit is
 * deliberate: a partial frame would desynchronise left from right for every
 * sample after it. */
static void i2s_empty_dma(machine_i2s_obj_t *self, uint8_t *half) {
    if (ringbuf_available_space(&self->ring_buffer) >= SIZEOF_HALF_DMA_BUFFER_IN_BYTES) {
        for (uint32_t i = 0; i < SIZEOF_HALF_DMA_BUFFER_IN_BYTES; i++) {
            ringbuf_push(&self->ring_buffer, half[i]);
        }
    }
}

/* Transmit: the bus always carries two slots, so a mono stream is written into
 * both of them -- otherwise it would come out of one channel only, at half the
 * expected rate. */
static void i2s_feed_dma(machine_i2s_obj_t *self, uint8_t *half) {
    if (ringbuf_available_data(&self->ring_buffer) < SIZEOF_HALF_DMA_BUFFER_IN_BYTES) {
        // Underflow: send silence rather than repeating whatever was here.
        memset(half, 0, SIZEOF_HALF_DMA_BUFFER_IN_BYTES);
        return;
    }

    if (self->format == MONO) {
        uint32_t sample = (uint32_t)self->bits / 8u;
        uint32_t frame = sample * 2u;
        for (uint32_t i = 0; i < SIZEOF_HALF_DMA_BUFFER_IN_BYTES / frame; i++) {
            for (uint32_t b = 0; b < sample; b++) {
                ringbuf_pop(&self->ring_buffer, &half[i * frame + b]);
                half[i * frame + sample + b] = half[i * frame + b];
            }
        }
    } else {
        for (uint32_t i = 0; i < SIZEOF_HALF_DMA_BUFFER_IN_BYTES; i++) {
            ringbuf_pop(&self->ring_buffer, &half[i]);
        }
    }

    /* Mirror of the ordering described at i2s_frame_map: the bus carries the
     * most significant half of a slot first, so a 32-bit sample has to be
     * handed to the 16-bit data register high half first, which is the
     * opposite of how it sits in memory. Sending it as-is transmits the two
     * halves swapped. 16-bit samples are a single transfer and need nothing.
     *
     * The transmit direction has no microphone to measure against; this
     * follows from the half-word order measured on receive, which shares the
     * data register and the same shift chain. */
    if (self->bits == 32) {
        uint16_t *w = (uint16_t *)(void *)half;
        for (uint32_t i = 0; i < SIZEOF_HALF_DMA_BUFFER_IN_BYTES / 4; i++) {
            uint16_t t = w[i * 2];
            w[i * 2] = w[i * 2 + 1];
            w[i * 2 + 1] = t;
        }
    }
}

static void i2s_dma_irq_handler(uint8_t id) {
    machine_i2s_obj_t *self = machine_i2s_active[id];
    if (self == NULL) {
        return;
    }

    uint32_t flags = DMA1->INTFR & self->dma_flags;
    DMA1->INTFCR = self->dma_flags;

    /* Half-transfer means the DMA has finished the first half and moved to the
     * second, so the first half is ours; transfer-complete is the mirror of
     * that. Servicing the wrong half would race the DMA. */
    uint8_t *half = (flags & self->dma_ht_flag)
        ? &self->dma_buffer[0]
        : &self->dma_buffer[SIZEOF_HALF_DMA_BUFFER_IN_BYTES];

    if (self->mode == TX) {
        if (self->io_mode == NON_BLOCKING && self->non_blocking_descriptor.copy_in_progress) {
            copy_appbuf_to_ringbuf_non_blocking(self);
        }
        i2s_feed_dma(self, half);
    } else {
        i2s_empty_dma(self, half);
        if (self->io_mode == NON_BLOCKING && self->non_blocking_descriptor.copy_in_progress) {
            fill_appbuf_from_ringbuf_non_blocking(self);
        }
    }
}

void CH32_IRQ_HANDLER(DMA1_Channel4_IRQHandler);
void DMA1_Channel4_IRQHandler(void) {
    i2s_dma_irq_handler(0);
}

void CH32_IRQ_HANDLER(DMA1_Channel5_IRQHandler);
void DMA1_Channel5_IRQHandler(void) {
    i2s_dma_irq_handler(1);
}

static void i2s_dma_init(machine_i2s_obj_t *self) {
    RCC_HBPeriphClockCmd(RCC_HBPeriph_DMA1, ENABLE);
    (void)RCC->HBPCENR;

    DMA_Cmd(self->dma, DISABLE);
    DMA1->INTFCR = self->dma_flags;

    DMA_InitTypeDef init = { 0 };
    init.DMA_PeripheralBaseAddr = (uint32_t)&self->spi->DATAR;
    init.DMA_Memory0BaseAddr = (uint32_t)self->dma_buffer;
    init.DMA_DIR = (self->mode == RX) ? DMA_DIR_PeripheralSRC : DMA_DIR_PeripheralDST;
    /* The I2S data register is 16 bits wide -- a 32-bit sample is two accesses
     * -- so the DMA moves half-words and the buffer length is in transfers,
     * not bytes. This is the one place the SAI differed: its register was 32
     * bits and it moved words. */
    init.DMA_BufferSize = SIZEOF_DMA_BUFFER_IN_BYTES / 2;
    init.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    init.DMA_MemoryInc = DMA_MemoryInc_Enable;
    init.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord;
    init.DMA_MemoryDataSize = DMA_MemoryDataSize_HalfWord;
    init.DMA_Mode = DMA_Mode_Circular;
    init.DMA_Priority = DMA_Priority_VeryHigh;
    init.DMA_M2M = DMA_M2M_Disable;
    DMA_Init(self->dma, &init);

    uint8_t req;
    if (self->i2s_id == 0) {
        req = (self->mode == RX) ? I2S_DMA_REQ_2_RX : I2S_DMA_REQ_2_TX;
        DMA_MuxChannelConfig(DMA_MuxChannel4, req);
    } else {
        req = (self->mode == RX) ? I2S_DMA_REQ_3_RX : I2S_DMA_REQ_3_TX;
        DMA_MuxChannelConfig(DMA_MuxChannel5, req);
    }

    DMA_ITConfig(self->dma, DMA_IT_TC | DMA_IT_HT, ENABLE);
    NVIC_SetPriority(self->dma_irqn, 2);
    NVIC_EnableIRQ(self->dma_irqn);
    DMA_Cmd(self->dma, ENABLE);
}

/******************************************************************************/
// I2S peripheral

static void i2s_periph_init(machine_i2s_obj_t *self) {
    RCC_HB1PeriphClockCmd(
        self->i2s_id == 0 ? RCC_HB1Periph_SPI2 : RCC_HB1Periph_SPI3, ENABLE);
    (void)RCC->HB1PCENR;

    I2S_Cmd(self->spi, DISABLE);

    uint8_t chbits = i2s_channel_bits(self);
    I2S_InitTypeDef init = { 0 };
    if (self->slave) {
        init.I2S_Mode = (self->mode == RX) ? I2S_Mode_SlaveRx : I2S_Mode_SlaveTx;
    } else {
        init.I2S_Mode = (self->mode == RX) ? I2S_Mode_MasterRx : I2S_Mode_MasterTx;
    }
    init.I2S_Standard = I2S_Standard_Phillips;
    init.I2S_DataFormat = (chbits == 16) ? I2S_DataFormat_16b : I2S_DataFormat_32b;
    init.I2S_MCLKOutput = I2S_MCLKOutput_Disable;
    init.I2S_AudioFreq = (uint32_t)self->rate;
    init.I2S_CPOL = I2S_CPOL_Low;
    I2S_Init(self->spi, &init);

    if (self->slave) {
        /* The prescaler drives nothing here -- the bus clock arrives on the
         * pins -- so there is no divisor to choose and no rate to refuse. The
         * rate that will actually be delivered is whatever the master sends. */
        self->actual_rate = self->rate;
    } else {
        /* I2S_Init() derives the prescaler itself, rounding through a decimal
         * intermediate. Overwrite it with the nearest divisor so the delivered
         * rate is the best the hardware can do and actual_rate describes it
         * exactly. */
        uint32_t n = i2s_set_rate(self);
        self->spi->I2SPR = (uint16_t)((n >> 1) | ((n & 1u) ? SPI_I2SPR_ODD : 0u));
    }

    SPI_I2S_DMACmd(self->spi,
        (self->mode == RX) ? SPI_I2S_DMAReq_Rx : SPI_I2S_DMAReq_Tx, ENABLE);
}

static void i2s_pin_init(mp_hal_pin_obj_t pin, uint8_t af, bool input) {
    machine_pin_clock_enable(pin->id);
    RCC_HB2PeriphClockCmd(RCC_HB2Periph_AFIO, ENABLE);

    GPIO_InitTypeDef init = { 0 };
    init.GPIO_Pin = MACHINE_PIN_MASK(pin->id);
    /* Reference manual table 9-6 allows a pull on a receiving SD pin. On this
     * silicon that is wrong -- the same trap the SAI driver hit: with a pull
     * configured the pad still follows the signal but the peripheral samples
     * nothing, because only the floating-input mode routes the pad inward. */
    init.GPIO_Mode = input ? GPIO_Mode_IN_FLOATING : GPIO_Mode_AF_PP;
    init.GPIO_Speed = GPIO_Speed_Very_High;
    GPIO_Init(machine_pin_gpio(pin->id), &init);
    if (!input) {
        GPIO_PinAFConfig(machine_pin_gpio(pin->id), MACHINE_PIN_NUM(pin->id), af);
    } else {
        GPIO_PinAFConfig(machine_pin_gpio(pin->id), MACHINE_PIN_NUM(pin->id), af);
    }
}

/******************************************************************************/
// extmod interface

static void mp_machine_i2s_init_helper(machine_i2s_obj_t *self, mp_arg_val_t *args) {
    mp_hal_pin_obj_t sck = args[ARG_sck].u_obj == MP_OBJ_NULL
        ? NULL : mp_hal_get_pin_obj(args[ARG_sck].u_obj);
    mp_hal_pin_obj_t ws = args[ARG_ws].u_obj == MP_OBJ_NULL
        ? NULL : mp_hal_get_pin_obj(args[ARG_ws].u_obj);
    mp_hal_pin_obj_t sd = args[ARG_sd].u_obj == MP_OBJ_NULL
        ? NULL : mp_hal_get_pin_obj(args[ARG_sd].u_obj);
    if (sck == NULL || ws == NULL || sd == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("sck, ws and sd are all required"));
    }

    i2s_mode_t mode;
    if (args[ARG_mode].u_int == (mp_int_t)RX) {
        mode = RX;
    } else if (args[ARG_mode].u_int == (mp_int_t)TX) {
        mode = TX;
    } else {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid mode"));
    }

    int8_t bits = args[ARG_bits].u_int;
    if (bits != 16 && bits != 32) {
        mp_raise_ValueError(MP_ERROR_TEXT("bits must be 16 or 32"));
    }

    format_t format;
    if (args[ARG_format].u_int == (mp_int_t)MONO) {
        format = MONO;
    } else if (args[ARG_format].u_int == (mp_int_t)STEREO) {
        format = STEREO;
    } else {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid format"));
    }

    int32_t rate = args[ARG_rate].u_int;
    if (rate <= 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid rate"));
    }

    int32_t ring_buffer_len = args[ARG_ibuf].u_int;
    if (ring_buffer_len <= 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid ibuf"));
    }

    /* Everything is validated, and the pins resolved, before anything is
     * committed -- a bad argument leaves a working object untouched rather
     * than half reconfigured. */
    const i2s_pin_t *sck_tab = self->i2s_id == 0 ? i2s_sck_pins_2 : i2s_sck_pins_3;
    const i2s_pin_t *ws_tab = self->i2s_id == 0 ? i2s_ws_pins_2 : i2s_ws_pins_3;
    const i2s_pin_t *sd_tab = self->i2s_id == 0 ? i2s_sd_pins_2 : i2s_sd_pins_3;
    size_t sck_len = self->i2s_id == 0 ? I2S_PIN_COUNT(i2s_sck_pins_2) : I2S_PIN_COUNT(i2s_sck_pins_3);
    size_t ws_len = self->i2s_id == 0 ? I2S_PIN_COUNT(i2s_ws_pins_2) : I2S_PIN_COUNT(i2s_ws_pins_3);
    size_t sd_len = self->i2s_id == 0 ? I2S_PIN_COUNT(i2s_sd_pins_2) : I2S_PIN_COUNT(i2s_sd_pins_3);
    uint8_t sck_af = i2s_find_af(sck_tab, sck_len, sck);
    uint8_t ws_af = i2s_find_af(ws_tab, ws_len, ws);
    uint8_t sd_af = i2s_find_af(sd_tab, sd_len, sd);

    self->ring_buffer_storage = m_new(uint8_t, ring_buffer_len);
    ringbuf_init(&self->ring_buffer, self->ring_buffer_storage, ring_buffer_len);

    self->sck = sck;
    self->ws = ws;
    self->sd = sd;
    self->mode = mode;
    self->bits = bits;
    self->format = format;
    self->rate = rate;
    self->ibuf = ring_buffer_len;
    self->callback_for_non_blocking = MP_OBJ_NULL;
    self->io_mode = BLOCKING;
    self->slave = machine_i2s_slave_next[self->i2s_id];
    self->non_blocking_descriptor.copy_in_progress = false;
    memset(self->dma_buffer, 0, SIZEOF_DMA_BUFFER_IN_BYTES);

    if (self->i2s_id == 0) {
        self->spi = SPI2;
        self->dma = DMA1_Channel4;
        self->dma_flags = I2S_DMA_A_FLAGS;
        self->dma_ht_flag = I2S_DMA_A_HT;
        self->dma_irqn = DMA1_Channel4_IRQn;
    } else {
        self->spi = SPI3;
        self->dma = DMA1_Channel5;
        self->dma_flags = I2S_DMA_B_FLAGS;
        self->dma_ht_flag = I2S_DMA_B_HT;
        self->dma_irqn = DMA1_Channel5_IRQn;
    }

    /* A slave receives the bus clock and frame select rather than sourcing
     * them, so those two pads must not be driven -- wiring a slave to a master
     * with them configured as outputs shorts two drivers together. */
    i2s_pin_init(sck, sck_af, self->slave);
    i2s_pin_init(ws, ws_af, self->slave);
    i2s_pin_init(sd, sd_af, mode == RX);

    machine_i2s_active[self->i2s_id] = self;
    i2s_periph_init(self);

    /* Arm the DMA before enabling the peripheral. Enabling first lets the FIFO
     * move a sample during the gap, and the DMA then starts on the second slot
     * of a frame, leaving left and right swapped for the life of the object.
     * That was true of the SAI and there is no reason to find out the hard way
     * whether it is true here too. */
    i2s_dma_init(self);
    I2S_Cmd(self->spi, ENABLE);
}

static machine_i2s_obj_t *mp_machine_i2s_make_new_instance(mp_int_t i2s_id) {
    if (i2s_id < 0 || i2s_id >= MAX_I2S_CH32) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid id"));
    }

    machine_i2s_obj_t *self;
    if (MP_STATE_PORT(machine_i2s_obj[i2s_id]) == NULL) {
        self = mp_obj_malloc(machine_i2s_obj_t, &machine_i2s_type);
        MP_STATE_PORT(machine_i2s_obj[i2s_id]) = self;
        self->i2s_id = i2s_id;
        self->spi = NULL;
    } else {
        self = MP_STATE_PORT(machine_i2s_obj[i2s_id]);
        mp_machine_i2s_deinit(self);
    }
    return self;
}

/* Stop the hardware, without touching the heap.
 *
 * Separated from mp_machine_i2s_deinit() so the soft-reset path can use it:
 * there the ring buffer is about to be reclaimed wholesale and freeing it
 * individually would be pointless, but the DMA absolutely must be stopped
 * first. */
static void i2s_hw_stop(machine_i2s_obj_t *self) {
    NVIC_DisableIRQ(self->dma_irqn);
    DMA_ITConfig(self->dma, DMA_IT_TC | DMA_IT_HT, DISABLE);
    DMA_Cmd(self->dma, DISABLE);
    SPI_I2S_DMACmd(self->spi,
        (self->mode == RX) ? SPI_I2S_DMAReq_Rx : SPI_I2S_DMAReq_Tx, DISABLE);
    I2S_Cmd(self->spi, DISABLE);

    /* Reset the peripheral through RCC, not just its enable bit.
     *
     * I2S_Cmd(DISABLE) stops the block wherever it happens to be in a frame,
     * and what it leaves behind is not a state the next I2S_Cmd(ENABLE)
     * recovers from: the first object of a session worked and the second
     * reset the board. machine_can.c hit the same shape -- a controller that
     * would not come back from a control-register reset and needed the RCC
     * one -- so this is the known cure on this part. */
    uint32_t rcc_bit = (self->i2s_id == 0)
        ? RCC_HB1Periph_SPI2 : RCC_HB1Periph_SPI3;
    RCC_HB1PeriphResetCmd(rcc_bit, ENABLE);
    RCC_HB1PeriphResetCmd(rcc_bit, DISABLE);

    machine_i2s_active[self->i2s_id] = NULL;
}

static void mp_machine_i2s_deinit(machine_i2s_obj_t *self) {
    // spi doubles as the "is initialised" flag.
    if (self->spi != NULL) {
        i2s_hw_stop(self);
        m_free(self->ring_buffer_storage);
        self->ring_buffer_storage = NULL;
        self->spi = NULL;
    }
}

/* Called from the soft-reset path in main.c. Stopping the hardware is only
 * half of what this has to do; clearing the root pointer is the other half,
 * and leaving it set was a genuine crash.
 *
 * A soft reset re-runs gc_init(), which discards the whole heap, and mp_init()
 * does not clear root pointers -- so MP_STATE_PORT(machine_i2s_obj[]) survives
 * pointing into memory that is now free. The next I2S() takes the "already
 * exists" branch in mp_machine_i2s_make_new_instance() and adopts that dangling
 * pointer as the object, without even the mp_obj_malloc that would have set
 * base.type. Every field then reads back whatever the new program has since
 * allocated there: self->i2s_id is garbage, so machine_i2s_active[self->i2s_id]
 * writes off the end of a two-entry array, and the damage surfaces later as an
 * illegal instruction at a nonsense address.
 *
 * It reproduced with a period of two, which is what identified it: a fault
 * resets the board, so the run after a fault began with a zeroed .bss and a
 * NULL root pointer and passed, while the run after a *successful* one began
 * with the stale pointer and died. Iterating MP_STATE_PORT here rather than
 * machine_i2s_active is the point -- an object the script deinit()ed itself has
 * already cleared the latter, which is exactly the case that crashed. */
void machine_i2s_deinit_all(void) {
    for (uint8_t id = 0; id < MAX_I2S_CH32; id++) {
        machine_i2s_obj_t *self = MP_STATE_PORT(machine_i2s_obj[id]);
        if (self != NULL) {
            mp_machine_i2s_deinit(self);
            MP_STATE_PORT(machine_i2s_obj[id]) = NULL;
        }
        machine_i2s_active[id] = NULL;
    }
}

static void mp_machine_i2s_irq_update(machine_i2s_obj_t *self) {
    (void)self;
}

// Behind ch32.i2s_actual_rate(); see the comment there.
uint32_t machine_i2s_actual_rate(mp_int_t i2s_id) {
    if (i2s_id < 0 || i2s_id >= MAX_I2S_CH32) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid id"));
    }
    machine_i2s_obj_t *self = MP_STATE_PORT(machine_i2s_obj[i2s_id]);
    if (self == NULL || self->spi == NULL) {
        return 0;
    }
    return (uint32_t)self->actual_rate;
}

/* Behind ch32.i2s_slave(); see machine_i2s_slave_next. Sticky, so a script can
 * arm it once and then build the object with a plain I2S() call. */
void machine_i2s_set_slave(mp_int_t i2s_id, bool slave) {
    if (i2s_id < 0 || i2s_id >= MAX_I2S_CH32) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid id"));
    }
    machine_i2s_slave_next[i2s_id] = slave;
}

MP_REGISTER_ROOT_POINTER(void *machine_i2s_obj[MAX_I2S_CH32]);
