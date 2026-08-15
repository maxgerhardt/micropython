/* machine.AudioOut -- streaming stereo audio out of the two 12-bit DACs.
 *
 *     a = machine.AudioOut(rate=44100)
 *     a.write(pcm)          # signed 16-bit, stereo, interleaved
 *
 * machine.DAC writes one sample at a time from Python, which is fine for a
 * control voltage and useless for audio. This is the streaming path: a timer
 * paces the conversions, DMA feeds them, and write() only has to keep a ring
 * buffer topped up.
 *
 * The chain, all of which the hardware already supports:
 *
 *   TIM6 update -> TRGO -> both DAC channels trigger together
 *                       -> DMA request 103 (DAC1) via the DMAMUX
 *                       -> one 32-bit word from the ring into DAC->RD12BDHR
 *
 * RD12BDHR is the dual 12-bit right-aligned register: channel 1 in bits 11:0
 * and channel 2 in bits 27:16. So a stereo frame is a single 32-bit write and
 * a single DMA transfer, and the two channels can never drift apart -- which
 * is the whole reason to use it rather than a DMA channel per side.
 *
 * DAC1 is PA4 (left) and DAC2 is PA5 (right), both on VDDIO. They are raw DAC
 * pins with no output buffer worth the name at audio rates: feed them into a
 * high-impedance input, and put an RC low-pass on each to get rid of the
 * 44.1 kHz staircase before it reaches an amplifier.
 */
#include <stdbool.h>
#include <string.h>

#include "ch32h417.h"

#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/runtime.h"

#include "machine_pin.h"
#include "machine_timer.h"

/* Reference manual table 10-2. DAC2 is 104 and unused: in dual mode the DAC1
 * request carries both channels. */
#define AUDIO_DMA_REQ_DAC1 (103)

/* DMA1 channels 2 and 3 are machine.SPI, 4 and 5 are machine.I2S. 1 is free. */
#define AUDIO_DMA_CHANNEL (DMA1_Channel1)
#define AUDIO_DMA_MUX_CHANNEL (DMA_MuxChannel1)

/* Mid-scale on both channels: what an idle or starved output sits at. A DAC
 * resting at zero would slam the amplifier to one rail. */
#define AUDIO_SILENCE (0x08000800u)

#define AUDIO_RATE_MIN (1000)
#define AUDIO_RATE_MAX (192000)

/* Frames, not bytes. 4096 stereo frames is 93 ms at 44.1 kHz -- enough that a
 * garbage collection or a slow network read cannot be heard. */
#define AUDIO_IBUF_DEFAULT (4096)
#define AUDIO_IBUF_MIN (256)

typedef struct _machine_audioout_obj_t {
    mp_obj_base_t base;
    uint32_t *ring;         /* one 32-bit dual-DAC word per stereo frame */
    uint32_t frames;        /* length of ring, in words */
    uint32_t wr;            /* next word write() will fill */
    uint32_t rate;
    uint32_t underruns;
    /* State for spotting an underrun between two write() calls: what was still
     * queued when the last one returned, and when that was. */
    uint32_t queued_at_write;
    uint32_t last_write_us;
    /* An empty ring is normal before the first write() and a fault after it,
     * so underruns are only counted once something has actually been queued. */
    bool primed;
    bool running;
} machine_audioout_obj_t;

const mp_obj_type_t machine_audioout_type;

#define audioout_obj ((machine_audioout_obj_t *)MP_STATE_PORT(machine_audioout_obj))

/* --- hardware --- */

/* Where the DMA is reading from, as an index into the ring.
 *
 * CNTR counts down from `frames` and reloads, so the number already consumed
 * out of this pass is frames - CNTR. */
static uint32_t audio_dma_pos(machine_audioout_obj_t *self) {
    uint32_t left = DMA_GetCurrDataCounter(AUDIO_DMA_CHANNEL);
    if (left == 0 || left > self->frames) {
        return 0;
    }
    return self->frames - left;
}

/* Words that write() may fill without overtaking the DMA. One is held back so
 * that a full ring is distinguishable from an empty one. */
static uint32_t audio_free(machine_audioout_obj_t *self) {
    uint32_t rd = audio_dma_pos(self);
    uint32_t used = (self->wr + self->frames - rd) % self->frames;
    return self->frames - used - 1;
}

/* Leave the unwritten part of the ring holding silence.
 *
 * The DMA never stops. It circles the ring whether or not anything has been
 * queued, so whatever sits ahead of the write pointer is what gets played when
 * the producer falls behind. Left alone that is the previous pass's audio, and
 * a stalled stream is heard as the last fraction of a second repeating over
 * and over -- which sounds like a decoder fault rather than what it is. Filled
 * with silence it is heard as a gap: less alarming, and unmistakably a gap.
 *
 * Only the free region is touched, so this races neither the DMA reading the
 * queued frames nor a later write(), which lays real frames over the silence
 * from the same position. The DMA advancing during the loop only frees more
 * words, so at worst a few are left holding stale audio until the next call.
 *
 * This does mean a producer that stops calling write() altogether -- rather
 * than merely falling behind -- gets one ring of silence and then hears the
 * ring repeat, because nothing is left running to blank it. Stopping properly
 * goes through deinit(), which halts the DMA.
 */
static void audio_blank_free(machine_audioout_obj_t *self) {
    uint32_t space = audio_free(self);
    uint32_t pos = self->wr;
    for (uint32_t i = 0; i < space; i++) {
        self->ring[pos] = AUDIO_SILENCE;
        pos = (pos + 1) % self->frames;
    }
}

static void audio_timer_start(uint32_t rate) {
    RCC_HB1PeriphClockCmd(RCC_HB1Periph_TIM6, ENABLE);
    TIM_DeInit(TIM6);

    /* TIM6 counts at HCLK, not at the core clock. Those are not the same
     * thing on this part -- the V5F runs at 400 MHz while HCLK is 100 MHz --
     * and using SystemCoreClock here made every rate come out exactly 4x too
     * slow. machine.Timer asks the same way. */
    RCC_ClocksTypeDef clocks;
    RCC_GetClocksFreq(&clocks);
    uint32_t clk = clocks.HCLK_Frequency;
    uint32_t psc = 0;
    while ((clk / (psc + 1)) / rate > 0xFFFF) {
        psc++;
    }
    uint32_t period = (clk / (psc + 1) + rate / 2) / rate;
    if (period < 2) {
        period = 2;
    }

    TIM_TimeBaseInitTypeDef tb = { 0 };
    tb.TIM_Prescaler = (uint16_t)psc;
    tb.TIM_CounterMode = TIM_CounterMode_Up;
    tb.TIM_Period = (uint16_t)(period - 1);
    tb.TIM_ClockDivision = TIM_CKD_DIV1;
    TIM_TimeBaseInit(TIM6, &tb);

    /* Update event drives TRGO, which is what the DAC trigger listens to. */
    TIM_SelectOutputTrigger(TIM6, TIM_TRGOSource_Update);
    TIM_Cmd(TIM6, ENABLE);
}

static void audio_start(machine_audioout_obj_t *self) {
    machine_pin_clock_enable(MACHINE_PIN_ID(0, 4));
    GPIO_InitTypeDef gpio = { 0 };
    gpio.GPIO_Pin = GPIO_Pin_4 | GPIO_Pin_5;
    gpio.GPIO_Mode = GPIO_Mode_AIN;
    GPIO_Init(GPIOA, &gpio);

    RCC_HB1PeriphClockCmd(RCC_HB1Periph_DAC, ENABLE);
    RCC_HBPeriphClockCmd(RCC_HBPeriph_DMA1, ENABLE);

    DAC_InitTypeDef dac = { 0 };
    dac.DAC_Trigger = DAC_Trigger_T6_TRGO;
    dac.DAC_WaveGeneration = DAC_WaveGeneration_None;
    dac.DAC_OutputBuffer = DAC_OutputBuffer_Enable;
    DAC_Init(DAC_Channel_1, &dac);
    DAC_Init(DAC_Channel_2, &dac);

    DMA_DeInit(AUDIO_DMA_CHANNEL);
    DMA_InitTypeDef dma = { 0 };
    dma.DMA_PeripheralBaseAddr = (uint32_t)&DAC->RD12BDHR;
    dma.DMA_Memory0BaseAddr = (uint32_t)self->ring;
    dma.DMA_DIR = DMA_DIR_PeripheralDST;
    dma.DMA_BufferSize = self->frames;
    dma.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    dma.DMA_MemoryInc = DMA_MemoryInc_Enable;
    dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Word;
    dma.DMA_MemoryDataSize = DMA_MemoryDataSize_Word;
    dma.DMA_Mode = DMA_Mode_Circular;
    dma.DMA_Priority = DMA_Priority_VeryHigh;
    dma.DMA_M2M = DMA_M2M_Disable;
    DMA_Init(AUDIO_DMA_CHANNEL, &dma);
    DMA_MuxChannelConfig(AUDIO_DMA_MUX_CHANNEL, AUDIO_DMA_REQ_DAC1);
    DMA_Cmd(AUDIO_DMA_CHANNEL, ENABLE);

    /* Only channel 1's DMA is enabled. In dual mode its request moves one
     * word into RD12BDHR, which lands in both channels' holding registers, so
     * enabling channel 2's as well would fetch a second word per frame and
     * consume the buffer twice as fast. */
    DAC_DMACmd(DAC_Channel_1, ENABLE);
    DAC_Cmd(DAC_Channel_1, ENABLE);
    DAC_Cmd(DAC_Channel_2, ENABLE);

    audio_timer_start(self->rate);

    /* Start writing where the DMA is reading, rather than at word 0.
     *
     * The ring is pre-filled with silence and the DMA begins consuming it the
     * instant it is enabled, so by the time Python gets a turn the reader has
     * already lapped past word 0. Leaving the writer behind it would report
     * an almost-full ring and make the first write() block for a whole buffer
     * period -- 93 ms at the default size -- before a single real sample
     * reached the pins. Starting level with the reader means the ring is
     * immediately writable end to end and playback starts at once. */
    self->wr = audio_dma_pos(self);
    self->running = true;
}

static void audio_stop(machine_audioout_obj_t *self) {
    if (!self->running) {
        return;
    }
    self->running = false;
    TIM_Cmd(TIM6, DISABLE);
    DMA_Cmd(AUDIO_DMA_CHANNEL, DISABLE);
    DAC_DMACmd(DAC_Channel_1, DISABLE);

    /* Park both channels at mid-scale rather than at whatever sample the DMA
     * stopped on, so switching off does not leave a DC offset on the output. */
    DAC->RD12BDHR = AUDIO_SILENCE;
    DAC_Cmd(DAC_Channel_1, DISABLE);
    DAC_Cmd(DAC_Channel_2, DISABLE);
    ch32_timer_release(6, CH32_TIMER_AUDIO);
}

void machine_audioout_deinit_all(void) {
    if (audioout_obj != NULL) {
        audio_stop(audioout_obj);
        MP_STATE_PORT(machine_audioout_obj) = NULL;
    }
}

/* --- object --- */

static mp_obj_t machine_audioout_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_rate, ARG_ibuf };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_rate, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 44100} },
        { MP_QSTR_ibuf, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = AUDIO_IBUF_DEFAULT} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_int_t rate = args[ARG_rate].u_int;
    mp_int_t frames = args[ARG_ibuf].u_int;
    if (rate < AUDIO_RATE_MIN || rate > AUDIO_RATE_MAX) {
        mp_raise_ValueError(MP_ERROR_TEXT("rate must be 1000 to 192000"));
    }
    if (frames < AUDIO_IBUF_MIN) {
        mp_raise_ValueError(MP_ERROR_TEXT("ibuf is too small to cover a hiccup"));
    }

    uint8_t held = ch32_timer_owner(6);
    if (held != CH32_TIMER_FREE && held != CH32_TIMER_AUDIO) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("timer 6 is held by %s"), ch32_timer_owner_name(held));
    }

    machine_audioout_obj_t *self = audioout_obj;
    if (self != NULL) {
        audio_stop(self);
    } else {
        self = mp_obj_malloc(machine_audioout_obj_t, &machine_audioout_type);
        MP_STATE_PORT(machine_audioout_obj) = self;
    }
    ch32_timer_claim(6, CH32_TIMER_AUDIO);

    self->rate = (uint32_t)rate;
    self->frames = (uint32_t)frames;
    self->wr = 0;
    self->underruns = 0;
    self->queued_at_write = 0;
    self->last_write_us = 0;
    self->primed = false;
    self->ring = m_new(uint32_t, self->frames);
    for (uint32_t i = 0; i < self->frames; i++) {
        self->ring[i] = AUDIO_SILENCE;
    }

    audio_start(self);
    return MP_OBJ_FROM_PTR(self);
}

static void machine_audioout_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    machine_audioout_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "AudioOut(rate=%u, ibuf=%u frames%s)",
        (unsigned int)self->rate, (unsigned int)self->frames,
        self->running ? "" : ", stopped");
}

/* Signed 16-bit stereo in, dual 12-bit DAC words out.
 *
 * Blocks until every frame is in the ring. The DMA is reading it the whole
 * time, so this is a producer against a hardware consumer: fill what is free,
 * then wait for more to become free.
 */
static mp_obj_t machine_audioout_write(mp_obj_t self_in, mp_obj_t buf_in) {
    machine_audioout_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->running) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("AudioOut is deinitialised"));
    }

    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(buf_in, &bufinfo, MP_BUFFER_READ);
    if (bufinfo.len % 4 != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("need whole stereo frames of 16-bit samples"));
    }

    const int16_t *pcm = bufinfo.buf;
    uint32_t total = bufinfo.len / 4;
    uint32_t done = 0;

    /* Did the DMA run out of queued frames since the last call?
     *
     * Not answerable from the ring pointers. The moment the read pointer
     * passes the write pointer the difference between them wraps, so an empty
     * ring and a full one are the same reading a few microseconds apart --
     * measured, not assumed: checking for "everything free" caught nothing at
     * all, because it is only true for the instant the two are equal.
     *
     * The clock has no such ambiguity. The DMA consumes exactly `rate` frames
     * a second whatever the producer is doing, so comparing elapsed time
     * against what was left queued says plainly whether it ran dry, and stays
     * right however long the gap was. */
    if (self->primed) {
        uint32_t elapsed = mp_hal_ticks_us() - self->last_write_us;
        uint64_t consumed = ((uint64_t)elapsed * self->rate) / 1000000u;
        if (consumed > (uint64_t)self->queued_at_write) {
            self->underruns++;
        }
    }

    while (done < total) {
        uint32_t space = audio_free(self);
        if (space == 0) {
            /* The ring is full, which is the good case: the output is ahead
             * of the producer. Let the scheduler run -- this is where a
             * network read or a keyboard interrupt gets its chance. */
            mp_event_handle_nowait();
            continue;
        }
        uint32_t n = total - done;
        if (n > space) {
            n = space;
        }
        for (uint32_t i = 0; i < n; i++) {
            /* Signed 16-bit to unsigned 12-bit: shift the range up by half
             * scale, then drop the low four bits the DAC cannot represent. */
            uint32_t l = (uint32_t)(pcm[2 * (done + i)] + 32768) >> 4;
            uint32_t r = (uint32_t)(pcm[2 * (done + i) + 1] + 32768) >> 4;
            self->ring[self->wr] = l | (r << 16);
            self->wr = (self->wr + 1) % self->frames;
        }
        done += n;
    }

    audio_blank_free(self);
    /* Recorded after blanking, so the timestamp and the queue depth describe
     * the same moment: the one the next call measures the gap from. */
    self->queued_at_write = self->frames - 1 - audio_free(self);
    self->last_write_us = mp_hal_ticks_us();
    self->primed = true;
    return MP_OBJ_NEW_SMALL_INT(bufinfo.len);
}
static MP_DEFINE_CONST_FUN_OBJ_2(machine_audioout_write_obj, machine_audioout_write);

/* Frames that can be written without blocking. A player uses this to decide
 * whether it has time to decode another chunk. */
static mp_obj_t machine_audioout_free(mp_obj_t self_in) {
    machine_audioout_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_NEW_SMALL_INT(self->running ? audio_free(self) : 0);
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_audioout_free_obj, machine_audioout_free);

/* How many times write() has found the ring empty since construction.
 *
 * Non-zero means the producer could not keep up and silence went out in place
 * of audio. Worth having because an underrun is otherwise inaudible as
 * anything but a gap, and a gap sounds much like a stall anywhere else in the
 * chain -- this says which end of it to look at.
 */
static mp_obj_t machine_audioout_underruns(mp_obj_t self_in) {
    machine_audioout_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_int_from_uint(self->underruns);
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_audioout_underruns_obj, machine_audioout_underruns);

static mp_obj_t machine_audioout_deinit(mp_obj_t self_in) {
    audio_stop(MP_OBJ_TO_PTR(self_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_audioout_deinit_obj, machine_audioout_deinit);

static const mp_rom_map_elem_t machine_audioout_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&machine_audioout_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_free), MP_ROM_PTR(&machine_audioout_free_obj) },
    { MP_ROM_QSTR(MP_QSTR_underruns), MP_ROM_PTR(&machine_audioout_underruns_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&machine_audioout_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&machine_audioout_deinit_obj) },
};
static MP_DEFINE_CONST_DICT(machine_audioout_locals_dict, machine_audioout_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    machine_audioout_type,
    MP_QSTR_AudioOut,
    MP_TYPE_FLAG_NONE,
    make_new, machine_audioout_make_new,
    print, machine_audioout_print,
    locals_dict, &machine_audioout_locals_dict
    );

MP_REGISTER_ROOT_POINTER(void *machine_audioout_obj);
