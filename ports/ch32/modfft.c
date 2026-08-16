/* fft -- fixed-point FFT, for spectrum displays and simple audio analysis.
 *
 *     import fft
 *     from array import array
 *     f = fft.FFT(512)
 *     mags = array("I", bytearray(4 * f.bins()))
 *     f.magnitude(pcm, mags, stride=2)   # left channel of int16 stereo
 *
 * magnitude() reads signed 16-bit samples and writes size/2 unsigned 32-bit
 * magnitudes, one per bin from DC up to just below Nyquist. Bin k covers
 * k * rate / size Hz. stride and offset pick one channel out of interleaved
 * audio without a copy, which is the whole point: machine.AudioOut and
 * mp3.Decoder both speak interleaved int16, so a visualiser can read the same
 * buffer the player is about to play.
 *
 * Why C rather than Python: a 512-point transform is 2304 butterflies, which
 * is a few milliseconds of bytecode per frame and 0.4 ms here. At 20 frames a
 * second that is the difference between a fifth of the CPU and a hundredth --
 * and on a board that is also decoding MP3 in real time, the audio is what
 * pays for anything the display wastes.
 *
 * Arithmetic: samples and twiddle factors are int16, the working buffers are
 * int32, and no rescaling happens between stages. Each stage can at most
 * double a value, so a full-scale input reaches 2^15 * size, which for the
 * largest permitted transform is 2^27 -- comfortably inside int32. Skipping
 * the usual per-stage shift keeps quiet passages from being ground down to
 * nothing, which on a display is the difference between a spectrum and a flat
 * line.
 */
#include <math.h>

#include "py/runtime.h"

/* Below 16 the transform says nothing useful; above 4096 the scratch buffers
 * outgrow what a board this size should be spending on a graph. */
#define FFT_MIN_SIZE (16)
#define FFT_MAX_SIZE (4096)

typedef struct _fft_obj_t {
    mp_obj_base_t base;
    uint16_t size;
    uint8_t bits;
    bool window;
    int16_t *tw_re;             /* size/2 twiddle factors, Q15 */
    int16_t *tw_im;
    int16_t *win;               /* size window coefficients, Q15, or NULL */
    int32_t *re;                /* size scratch, reused every call */
    int32_t *im;
} fft_obj_t;

static const mp_obj_type_t fft_fft_type;

static uint32_t fft_bit_reverse(uint32_t x, uint32_t bits) {
    uint32_t r = 0;
    while (bits--) {
        r = (r << 1) | (x & 1);
        x >>= 1;
    }
    return r;
}

static mp_obj_t fft_fft_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_size, ARG_window };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_size, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_window, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = true} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_int_t size = args[ARG_size].u_int;
    if (size < FFT_MIN_SIZE || size > FFT_MAX_SIZE || (size & (size - 1)) != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("size must be a power of two, 16 to 4096"));
    }

    fft_obj_t *self = mp_obj_malloc(fft_obj_t, &fft_fft_type);
    self->size = size;
    self->window = args[ARG_window].u_bool;
    self->bits = 0;
    while ((size_t)1 << self->bits < (size_t)size) {
        self->bits++;
    }

    self->tw_re = m_new(int16_t, size / 2);
    self->tw_im = m_new(int16_t, size / 2);
    self->re = m_new(int32_t, size);
    self->im = m_new(int32_t, size);

    /* exp(-j*2*pi*k/size), which is the decimation-in-time convention: bin k
     * of the result then corresponds to a positive frequency of k*rate/size. */
    for (mp_int_t k = 0; k < size / 2; k++) {
        float a = -2.0f * (float)M_PI * (float)k / (float)size;
        self->tw_re[k] = (int16_t)lrintf(32767.0f * cosf(a));
        self->tw_im[k] = (int16_t)lrintf(32767.0f * sinf(a));
    }

    /* A periodic Hann window -- 1 - cos over size, not over size-1. Without a
     * window every tone that does not land exactly on a bin centre smears
     * across the whole spectrum, which on a bar display looks like a noise
     * floor that rises and falls with the music. */
    if (self->window) {
        self->win = m_new(int16_t, size);
        for (mp_int_t i = 0; i < size; i++) {
            float w = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i / (float)size));
            self->win[i] = (int16_t)lrintf(32767.0f * w);
        }
    } else {
        self->win = NULL;
    }

    return MP_OBJ_FROM_PTR(self);
}

static void fft_fft_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    fft_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "FFT(%u, %u bins, %s)",
        self->size, self->size / 2, self->window ? "hann" : "no window");
}

static mp_obj_t fft_fft_magnitude(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_src, ARG_dst, ARG_stride, ARG_offset };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_stride, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 1} },
        { MP_QSTR_offset, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    fft_obj_t *self = MP_OBJ_TO_PTR(args[ARG_self].u_obj);
    mp_int_t size = self->size;
    mp_int_t bins = size / 2;
    mp_int_t stride = args[ARG_stride].u_int;
    mp_int_t offset = args[ARG_offset].u_int;

    if (stride < 1) {
        mp_raise_ValueError(MP_ERROR_TEXT("stride must be positive"));
    }
    if (offset < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("offset must not be negative"));
    }

    mp_buffer_info_t src;
    mp_get_buffer_raise(args[ARG_src].u_obj, &src, MP_BUFFER_READ);
    mp_buffer_info_t dst;
    mp_get_buffer_raise(args[ARG_dst].u_obj, &dst, MP_BUFFER_WRITE);

    /* The last sample read is offset + (size-1)*stride. Checked in samples
     * rather than bytes so that an odd-length buffer cannot be read half a
     * sample past its end. */
    mp_int_t avail = src.len / (mp_int_t)sizeof(int16_t);
    if (offset + (size - 1) * stride >= avail) {
        mp_raise_ValueError(MP_ERROR_TEXT("source buffer too short for size, stride and offset"));
    }
    if (dst.len < bins * (mp_int_t)sizeof(uint32_t)) {
        mp_raise_ValueError(MP_ERROR_TEXT("destination buffer must hold size/2 uint32 magnitudes"));
    }

    const int16_t *in = (const int16_t *)src.buf;
    int32_t *re = self->re;
    int32_t *im = self->im;
    const int16_t *win = self->win;
    uint32_t bits = self->bits;

    /* Load, window and bit-reverse in one pass, so nothing is walked twice. */
    for (mp_int_t i = 0; i < size; i++) {
        int32_t s = in[offset + i * stride];
        if (win != NULL) {
            s = (s * win[i]) >> 15;
        }
        uint32_t j = fft_bit_reverse(i, bits);
        re[j] = s;
        im[j] = 0;
    }

    /* Radix-2 decimation in time. The twiddle table is indexed by j*step
     * rather than being recomputed, so the whole transform costs 4 multiplies
     * per butterfly and no trigonometry at all.
     *
     * The +16384 before each shift rounds instead of truncating. A bare >>15
     * rounds toward negative infinity, and the same small bias applied at
     * every stage compounds: measured over 8 stages it cost 0.027% of the
     * amplitude, always downwards. Two adds per butterfly is a cheaper fix
     * than explaining the discrepancy to whoever measures it next. */
    for (mp_int_t len = 2; len <= size; len <<= 1) {
        mp_int_t half = len / 2;
        mp_int_t step = size / len;
        for (mp_int_t i = 0; i < size; i += len) {
            const int16_t *wr = self->tw_re;
            const int16_t *wi = self->tw_im;
            for (mp_int_t j = 0; j < half; j++, wr += step, wi += step) {
                int32_t vr = re[i + j + half];
                int32_t vi = im[i + j + half];
                int32_t tr = (int32_t)(((int64_t)vr * *wr - (int64_t)vi * *wi + 16384) >> 15);
                int32_t ti = (int32_t)(((int64_t)vr * *wi + (int64_t)vi * *wr + 16384) >> 15);
                int32_t ur = re[i + j];
                int32_t ui = im[i + j];
                re[i + j] = ur + tr;
                im[i + j] = ui + ti;
                re[i + j + half] = ur - tr;
                im[i + j + half] = ui - ti;
            }
        }
    }

    /* Magnitudes in float: the squares reach 2^54, which no int32 holds and
     * which a 24-bit mantissa carries to well under a tenth of a percent --
     * far finer than anything a display or a level meter can show. */
    uint32_t *out = (uint32_t *)dst.buf;
    for (mp_int_t k = 0; k < bins; k++) {
        float fr = (float)re[k];
        float fi = (float)im[k];
        out[k] = (uint32_t)sqrtf(fr * fr + fi * fi);
    }

    return MP_OBJ_NEW_SMALL_INT(bins);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(fft_fft_magnitude_obj, 3, fft_fft_magnitude);

/* The transform length, in samples. */
static mp_obj_t fft_fft_size(mp_obj_t self_in) {
    fft_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_NEW_SMALL_INT(self->size);
}
static MP_DEFINE_CONST_FUN_OBJ_1(fft_fft_size_obj, fft_fft_size);

/* How many magnitudes magnitude() writes, which is what a caller has to
 * allocate for. Always size/2: the upper half of a real signal's spectrum is
 * the mirror image of the lower and carries nothing new. */
static mp_obj_t fft_fft_bins(mp_obj_t self_in) {
    fft_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_NEW_SMALL_INT(self->size / 2);
}
static MP_DEFINE_CONST_FUN_OBJ_1(fft_fft_bins_obj, fft_fft_bins);

static const mp_rom_map_elem_t fft_fft_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_magnitude), MP_ROM_PTR(&fft_fft_magnitude_obj) },
    { MP_ROM_QSTR(MP_QSTR_size), MP_ROM_PTR(&fft_fft_size_obj) },
    { MP_ROM_QSTR(MP_QSTR_bins), MP_ROM_PTR(&fft_fft_bins_obj) },
};
static MP_DEFINE_CONST_DICT(fft_fft_locals_dict, fft_fft_locals_dict_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    fft_fft_type,
    MP_QSTR_FFT,
    MP_TYPE_FLAG_NONE,
    make_new, fft_fft_make_new,
    print, fft_fft_print,
    locals_dict, &fft_fft_locals_dict
    );

static const mp_rom_map_elem_t fft_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_fft) },
    { MP_ROM_QSTR(MP_QSTR_FFT), MP_ROM_PTR(&fft_fft_type) },
    /* Magnitudes are not normalised, because dividing then multiplying again
     * would only lose bits. To get 0..1 instead, divide by size * 16384 --
     * that is what a full-scale sine sitting on a bin centre reaches, and it
     * is halved again by the Hann window, which passes half the energy. */
    { MP_ROM_QSTR(MP_QSTR_FULL_SCALE), MP_ROM_INT(16384) },
};
static MP_DEFINE_CONST_DICT(fft_module_globals, fft_module_globals_table);

const mp_obj_module_t fft_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&fft_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_fft, fft_module);
