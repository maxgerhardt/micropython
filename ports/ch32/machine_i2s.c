/* machine.I2S on the SAI peripheral.
 *
 * The hardware half of MicroPython's I2S class; extmod/machine_i2s.c owns the
 * Python-facing behaviour, the ring buffer and the blocking / non-blocking /
 * asyncio modes, and includes this file. A port supplies only the object
 * struct and the four mp_machine_i2s_* entry points at the bottom.
 *
 * This uses SAI, NOT the peripherals the datasheet calls I2S2 and I2S3, and
 * that is deliberate. I2S2/I2S3 hang off SPI2/SPI3, and every pin either can
 * reach is in the VIO18 domain, which this board runs at about 1.2 V. That
 * cannot meet the input threshold of a 3.3 V audio device, and the device's
 * 3.3 V output into a 1.2 V pad is a large overdrive. SAI block A reaches
 * PE4/PE5/PE6, which are in the 3.3 V domain -- PE2, PE5 and PE6 measured
 * directly, see docs/hw/ch32h417-notes.md. Hence SAI, despite the names.
 */

#include <string.h>

#include "py/mphal.h"
#include "py/runtime.h"

#include "ch32h417.h"
#include "irq.h"
#include "machine_pin.h"

/* Two SAI blocks. Block A is the one broken out to 3.3 V pins here; block B's
 * clock and frame-sync pins are all VIO18, so it is only useful synchronous to
 * A -- sharing A's SCK/FS and needing just its own data pin PE3 -- or with
 * level shifting. */
#define MAX_I2S_CH32 (2)

/* DMA buffer, halved and serviced from the half-transfer and transfer-complete
 * interrupts so the DMA always owns one half while the other is copied. At
 * 48 kHz stereo 32-bit -- the fastest configuration here, 384 kB/s -- each half
 * lasts about 340 us, a comfortable interrupt rate and a small latency. */
#define SIZEOF_DMA_BUFFER_IN_BYTES (512)
#define SIZEOF_HALF_DMA_BUFFER_IN_BYTES (SIZEOF_DMA_BUFFER_IN_BYTES / 2)

/* DMA1 channels 2 and 3 belong to machine.SPI; 4 and 5 are free. One channel
 * per block is enough: an I2S object is either RX or TX, never both. */
#define I2S_DMA_A_FLAGS      ((uint32_t)0x0000F000)   // channel 4 nibble
#define I2S_DMA_A_HT         ((uint32_t)0x00004000)
#define I2S_DMA_B_FLAGS      ((uint32_t)0x000F0000)   // channel 5 nibble
#define I2S_DMA_B_HT         ((uint32_t)0x00040000)

/* DMAMUX request numbers, reference manual table 10-2. */
#define I2S_DMA_REQ_A_TX     (112)
#define I2S_DMA_REQ_A_RX     (113)
#define I2S_DMA_REQ_B_TX     (114)
#define I2S_DMA_REQ_B_RX     (115)

// MCKDIV is 6 bits; see i2s_set_rate() for what that costs at low rates.
#define I2S_MCKDIV_MAX (63)

/* In non-blocking mode the ring buffer is drained/filled faster than the DMA
 * moves data, so a slow caller cannot make it underflow. */
#define NON_BLOCKING_RATE_MULTIPLIER (4)
#define SIZEOF_NON_BLOCKING_COPY_IN_BYTES (SIZEOF_HALF_DMA_BUFFER_IN_BYTES * NON_BLOCKING_RATE_MULTIPLIER)

/* Receive always captures 32-bit stereo, whatever the user asked for, and
 * extmod/machine_i2s.c reshapes it on the way out through this map. So the
 * ring buffer always holds 8-byte frames on the RX side, and -1 means "this
 * output byte comes from nowhere". */
static const int8_t i2s_frame_map[NUM_I2S_USER_FORMATS][I2S_RX_FRAME_SIZE_IN_BYTES] = {
    { -1, -1,  0,  1, -1, -1, -1, -1 },  // Mono, 16-bit
    {  0,  1,  2,  3, -1, -1, -1, -1 },  // Mono, 32-bit
    { -1, -1,  0,  1, -1, -1,  2,  3 },  // Stereo, 16-bit
    {  0,  1,  2,  3,  4,  5,  6,  7 },  // Stereo, 32-bit
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
    SAI_Block_TypeDef *block;
    DMA_Channel_TypeDef *dma;
    uint32_t dma_flags;
    uint32_t dma_ht_flag;
    IRQn_Type dma_irqn;
    int32_t actual_rate;              // what the divider could really produce
    uint8_t dma_buffer[SIZEOF_DMA_BUFFER_IN_BYTES];
    ring_buf_t ring_buffer;
    uint8_t *ring_buffer_storage;
    non_blocking_descriptor_t non_blocking_descriptor;
} machine_i2s_obj_t;

static machine_i2s_obj_t *machine_i2s_active[MAX_I2S_CH32];

/******************************************************************************/
// Pin tables
//
// Only the port E options are offered for block A. The datasheet also allows
// PC0-PC3, PB2 and PD6, but those are VIO18 pins where a 3.3 V audio device
// does not work -- listing them would only be a way to lose an afternoon.

static const i2s_pin_t i2s_sck_pins_a[] = { { &pin_E5_obj, GPIO_AF6 } };
static const i2s_pin_t i2s_ws_pins_a[] = { { &pin_E4_obj, GPIO_AF6 } };
static const i2s_pin_t i2s_sd_pins_a[] = { { &pin_E6_obj, GPIO_AF6 } };

static const i2s_pin_t i2s_sck_pins_b[] = { { &pin_A14_obj, GPIO_AF13 } };
static const i2s_pin_t i2s_ws_pins_b[] = { { &pin_A15_obj, GPIO_AF13 } };
static const i2s_pin_t i2s_sd_pins_b[] = { { &pin_E3_obj, GPIO_AF6 } };

static uint8_t i2s_find_af(const i2s_pin_t *table, size_t len, mp_hal_pin_obj_t pin) {
    for (size_t i = 0; i < len; i++) {
        if (table[i].pin == pin) {
            return table[i].af;
        }
    }
    mp_raise_ValueError(MP_ERROR_TEXT("pin cannot be used for this I2S signal"));
    return 0;
}

/* The data pin must be a FLOATING input when receiving. Do not "improve" this
 * into a pulled input.
 *
 * Reference manual table 9-6 says a pull is allowed -- "I2Sx_SD Receiver:
 * Floating input or pull-up or pull-down input" -- and on this silicon that is
 * wrong. Configuring PE6 as input-with-pull (CNF=10) leaves the pad working,
 * measurably: it still follows the microphone and still toggles. But the SAI
 * then samples nothing and every captured word is zero. Only CNF=01, floating
 * input, routes the pad to the peripheral.
 *
 * That matters because an I2S microphone tri-states SD whenever it is not
 * driving its own channel -- the INMP441 drives 24 bits of a 32-bit slot and
 * lets go for the last 8 -- and a floating input holds the last level on pin
 * capacitance, so those 8 bits read back as a copy of bit 8 rather than zero.
 * The microphone's datasheet asks for a 100k pulldown on the SD trace for
 * exactly this reason. It has to be an external resistor here; mask the low 8
 * bits in software otherwise. */
static void i2s_pin_init(mp_hal_pin_obj_t pin, uint8_t af, bool is_input) {
    machine_pin_clock_enable(pin->id);
    /* GPIO_PinAFConfig writes land in the AFIO block and are silently dropped
     * while its clock is off. */
    RCC_HB2PeriphClockCmd(RCC_HB2Periph_AFIO, ENABLE);
    (void)RCC->HB2PCENR;

    GPIO_InitTypeDef init = { 0 };
    init.GPIO_Pin = MACHINE_PIN_MASK(pin->id);
    init.GPIO_Speed = GPIO_Speed_Very_High;
    init.GPIO_Mode = is_input ? GPIO_Mode_IN_FLOATING : GPIO_Mode_AF_PP;
    GPIO_Init(machine_pin_gpio(pin->id), &init);

    GPIO_PinAFConfig(machine_pin_gpio(pin->id), MACHINE_PIN_NUM(pin->id), af);
}

/******************************************************************************/
// Clocking

/* Bit clock is SAI_CK / (MCKDIV * 2) with the master-clock divider disabled,
 * which is the mode to use here: an INMP441-class microphone derives all its
 * timing from SCK and needs no MCLK at all.
 *
 * A stereo frame is two slots of `bits`, so SCK = rate * bits * 2.
 *
 * The divider is only 6 bits, so not every rate is reachable and none is
 * exact. The object reports what it actually got rather than echoing the
 * request -- print(i2s) shows both -- because silently running a fraction of a
 * percent fast is exactly the sort of thing that turns into a pitch-shifted
 * recording nobody can account for. */
/* Slot width the hardware actually runs at, which is not always what the user
 * asked for: receive is always 32-bit stereo, because extmod's frame map
 * expects 8-byte frames and narrows them itself. Transmit uses the requested
 * width directly. */
static uint8_t i2s_hw_bits(machine_i2s_obj_t *self) {
    return self->mode == RX ? 32 : (uint8_t)self->bits;
}

static uint32_t i2s_set_rate(machine_i2s_obj_t *self) {
    RCC_ClocksTypeDef clocks;
    RCC_GetClocksFreq(&clocks);
    uint32_t sai_ck = clocks.HCLK_Frequency;

    uint32_t sck = (uint32_t)self->rate * (uint32_t)i2s_hw_bits(self) * 2u;
    uint32_t div = (sai_ck + sck) / (2u * sck);   // rounded to nearest
    if (div == 0) {
        div = 1;
    }
    if (div > I2S_MCKDIV_MAX) {
        /* Below roughly 12 kHz at a 100 MHz SAI clock the divider runs out.
         * Refusing beats quietly delivering a different sample rate. */
        mp_raise_ValueError(MP_ERROR_TEXT("rate too low"));
    }

    self->actual_rate = (int32_t)(sai_ck / (2u * div) / ((uint32_t)i2s_hw_bits(self) * 2u));
    return div;
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

/* Transmit: the SAI always sends two slots, so a mono stream is written into
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
    init.DMA_PeripheralBaseAddr = (uint32_t)&self->block->DATAR;
    init.DMA_Memory0BaseAddr = (uint32_t)self->dma_buffer;
    init.DMA_DIR = (self->mode == RX) ? DMA_DIR_PeripheralSRC : DMA_DIR_PeripheralDST;
    /* The SAI data register is 32 bits wide whatever the slot size, so the DMA
     * always moves words; the buffer length is in transfers, not bytes. */
    init.DMA_BufferSize = SIZEOF_DMA_BUFFER_IN_BYTES / 4;
    init.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    init.DMA_MemoryInc = DMA_MemoryInc_Enable;
    init.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Word;
    init.DMA_MemoryDataSize = DMA_MemoryDataSize_Word;
    init.DMA_Mode = DMA_Mode_Circular;
    init.DMA_Priority = DMA_Priority_VeryHigh;
    init.DMA_M2M = DMA_M2M_Disable;
    DMA_Init(self->dma, &init);

    uint8_t req;
    if (self->i2s_id == 0) {
        req = (self->mode == RX) ? I2S_DMA_REQ_A_RX : I2S_DMA_REQ_A_TX;
        DMA_MuxChannelConfig(DMA_MuxChannel4, req);
    } else {
        req = (self->mode == RX) ? I2S_DMA_REQ_B_RX : I2S_DMA_REQ_B_TX;
        DMA_MuxChannelConfig(DMA_MuxChannel5, req);
    }

    DMA_ITConfig(self->dma, DMA_IT_TC | DMA_IT_HT, ENABLE);
    NVIC_SetPriority(self->dma_irqn, 2);
    NVIC_EnableIRQ(self->dma_irqn);
    DMA_Cmd(self->dma, ENABLE);
}

/******************************************************************************/
// SAI

static void i2s_sai_init(machine_i2s_obj_t *self) {
    RCC_HB2PeriphClockCmd(RCC_HB2Periph_SAI, ENABLE);
    (void)RCC->HB2PCENR;

    SAI_Cmd(self->block, DISABLE);

    SAI_InitTypeDef init;
    init.SAI_AudioMode = (self->mode == RX) ? SAI_Mode_MasterRx : SAI_Mode_MasterTx;
    init.SAI_Protocol = SAI_Free_Protocol;
    init.SAI_DataSize = (i2s_hw_bits(self) == 16) ? SAI_DataSize_16b : SAI_DataSize_32b;
    init.SAI_FirstBit = SAI_FirstBit_MSB;
    /* I2S drives data on the falling edge and samples on the rising edge, so a
     * receiver strobes rising and a transmitter falling. */
    init.SAI_ClockStrobing = (self->mode == RX) ? SAI_ClockStrobing_RisingEdge
                                                : SAI_ClockStrobing_FallingEdge;
    init.SAI_Synchro = SAI_Asynchronous;
    init.SAI_OutDRIV = SAI_Output_NotReleased;
    // No master clock; disabling the divider is what puts SCK straight on MCKDIV.
    init.SAI_NoDivider = SAI_MasterDivider_Disabled;
    init.SAI_MasterDivider = i2s_set_rate(self);
    init.SAI_FIFOThreshold = SAI_FIFOThreshold_HalfFull;
    SAI_Init(self->block, &init);

    /* Philips I2S framing: the frame is two slots wide, frame sync is low for
     * the left slot and lasts exactly half the frame, and data starts one bit
     * clock after the edge. */
    SAI_FrameInitTypeDef frame;
    frame.SAI_FrameLength = i2s_hw_bits(self) * 2;
    frame.SAI_ActiveFrameLength = i2s_hw_bits(self);
    frame.SAI_FSDefinition = I2S_FS_ChannelIdentification;
    /* Active low, because I2S holds WS low for the left channel and slot 0 is
     * the one that begins at the FS active edge. A microphone with L/R tied to
     * ground transmits in the left channel, so its data lands in slot 0 --
     * which is the slot MONO reads.
     *
     * This briefly looked wrong during bring-up: the data appeared in slot 1
     * and flipping the polarity appeared to fix it. It did not. The real fault
     * was the SAI being enabled before the DMA was armed, which let the
     * capture start half a frame late and swapped the slots; flipping the
     * polarity only cancelled that out, and only sometimes. Fix the ordering,
     * not the polarity -- see mp_machine_i2s_init_helper(). */
    frame.SAI_FSPolarity = SAI_FS_ActiveLow;
    frame.SAI_FSOffset = SAI_FS_BeforeFirstBit;
    SAI_FrameInit(self->block, &frame);

    SAI_SlotInitTypeDef slot;
    slot.SAI_FirstBitOffset = 0;
    slot.SAI_SlotSize = SAI_SlotSize_DataSize;
    slot.SAI_SlotNumber = 2;
    slot.SAI_SlotActive = SAI_SlotActive_0 | SAI_SlotActive_1;
    SAI_SlotInit(self->block, &slot);

    SAI_DMACmd(self->block, ENABLE);
    /* Deliberately NOT enabled here. The block is started only once the DMA is
     * armed -- see the ordering note in mp_machine_i2s_init_helper(). */
}

/******************************************************************************/
// Port entry points required by extmod/machine_i2s.c

static void mp_machine_i2s_init_helper(machine_i2s_obj_t *self, mp_arg_val_t *args) {
    mp_hal_pin_obj_t sck = args[ARG_sck].u_obj == MP_OBJ_NULL ? NULL : mp_hal_get_pin_obj(args[ARG_sck].u_obj);
    mp_hal_pin_obj_t ws = args[ARG_ws].u_obj == MP_OBJ_NULL ? NULL : mp_hal_get_pin_obj(args[ARG_ws].u_obj);
    mp_hal_pin_obj_t sd = args[ARG_sd].u_obj == MP_OBJ_NULL ? NULL : mp_hal_get_pin_obj(args[ARG_sd].u_obj);
    if (sck == NULL || ws == NULL || sd == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("sck, ws and sd are required"));
    }

    i2s_mode_t mode = args[ARG_mode].u_int;
    if (mode != RX && mode != TX) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid mode"));
    }

    int8_t bits = args[ARG_bits].u_int;
    if (bits != 16 && bits != 32) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid bits"));
    }

    format_t format = args[ARG_format].u_int;
    if (format != MONO && format != STEREO) {
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
    const i2s_pin_t *sck_tab = self->i2s_id == 0 ? i2s_sck_pins_a : i2s_sck_pins_b;
    const i2s_pin_t *ws_tab = self->i2s_id == 0 ? i2s_ws_pins_a : i2s_ws_pins_b;
    const i2s_pin_t *sd_tab = self->i2s_id == 0 ? i2s_sd_pins_a : i2s_sd_pins_b;
    uint8_t sck_af = i2s_find_af(sck_tab, 1, sck);
    uint8_t ws_af = i2s_find_af(ws_tab, 1, ws);
    uint8_t sd_af = i2s_find_af(sd_tab, 1, sd);

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
    self->non_blocking_descriptor.copy_in_progress = false;
    self->io_mode = BLOCKING;
    memset(self->dma_buffer, 0, sizeof(self->dma_buffer));

    if (self->i2s_id == 0) {
        self->block = SAI_Block_A;
        self->dma = DMA1_Channel4;
        self->dma_flags = I2S_DMA_A_FLAGS;
        self->dma_ht_flag = I2S_DMA_A_HT;
        self->dma_irqn = DMA1_Channel4_IRQn;
    } else {
        self->block = SAI_Block_B;
        self->dma = DMA1_Channel5;
        self->dma_flags = I2S_DMA_B_FLAGS;
        self->dma_ht_flag = I2S_DMA_B_HT;
        self->dma_irqn = DMA1_Channel5_IRQn;
    }

    i2s_pin_init(sck, sck_af, false);
    i2s_pin_init(ws, ws_af, false);
    i2s_pin_init(sd, sd_af, self->mode == RX);

    machine_i2s_active[self->i2s_id] = self;

    /* Order matters, and getting it wrong is subtle. The SAI must be started
     * AFTER the DMA is armed. Enabling it first lets its FIFO begin filling in
     * the window before the DMA is running, so the first word the DMA collects
     * can be the second slot of a frame rather than the first -- and from then
     * on left and right are swapped for the life of the object. It is not even
     * consistently wrong: it depends on how long that window happens to be,
     * which is why MONO looked fine while STEREO came out reversed. */
    i2s_sai_init(self);
    i2s_dma_init(self);
    SAI_FlushFIFO(self->block);
    SAI_Cmd(self->block, ENABLE);
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
        self->block = NULL;
    } else {
        self = MP_STATE_PORT(machine_i2s_obj[i2s_id]);
        mp_machine_i2s_deinit(self);
    }
    return self;
}

static void mp_machine_i2s_deinit(machine_i2s_obj_t *self) {
    // block doubles as the "is initialised" flag.
    if (self->block != NULL) {
        NVIC_DisableIRQ(self->dma_irqn);
        DMA_ITConfig(self->dma, DMA_IT_TC | DMA_IT_HT, DISABLE);
        DMA_Cmd(self->dma, DISABLE);
        SAI_Cmd(self->block, DISABLE);
        SAI_FlushFIFO(self->block);
        machine_i2s_active[self->i2s_id] = NULL;
        m_free(self->ring_buffer_storage);
        self->ring_buffer_storage = NULL;
        self->block = NULL;
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
    if (self == NULL || self->block == NULL) {
        return 0;
    }
    return (uint32_t)self->actual_rate;
}

MP_REGISTER_ROOT_POINTER(void *machine_i2s_obj[MAX_I2S_CH32]);
