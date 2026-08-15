/* mp3 -- MP3 frame decoder, for internet radio and for playing files.
 *
 *     import mp3
 *     d = mp3.Decoder()
 *     used, frames = d.decode(inbuf, outbuf)
 *
 * One call decodes at most one MP3 frame. `used` is how many input bytes to
 * drop, `frames` is how many stereo sample-frames landed in outbuf as signed
 * 16-bit interleaved -- which is exactly what machine.AudioOut.write() takes,
 * so a player is a loop with no conversion in it.
 *
 * used > 0 with frames == 0 is normal and not an error: it is the decoder
 * skipping an ID3 tag or resynchronising after junk. used == 0 means the
 * buffer does not yet hold a whole frame, so read more and call again.
 *
 * The decoder is minimp3 (lib/minimp3, CC0). It is a single header, its state
 * is about 6.7K which lives in this object on the heap, and it emits int16
 * natively rather than float, so nothing has to be converted afterwards.
 */
#include "py/mperrno.h"
#include "py/runtime.h"

/* No SIMD: the header's dispatch only knows x86 and ARM NEON, and on anything
 * else it falls back to scalar code anyway -- saying so explicitly stops it
 * probing for intrinsics that do not exist on RISC-V. */
#define MINIMP3_NO_SIMD
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

typedef struct _mp3_decoder_obj_t {
    mp_obj_base_t base;
    mp3dec_t dec;
    int rate;
    int channels;
    int bitrate_kbps;
} mp3_decoder_obj_t;

static const mp_obj_type_t mp3_decoder_type;

static mp_obj_t mp3_decoder_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 0, 0, false);
    mp3_decoder_obj_t *self = mp_obj_malloc(mp3_decoder_obj_t, &mp3_decoder_type);
    mp3dec_init(&self->dec);
    self->rate = 0;
    self->channels = 0;
    self->bitrate_kbps = 0;
    return MP_OBJ_FROM_PTR(self);
}

static void mp3_decoder_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    mp3_decoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->rate == 0) {
        mp_printf(print, "Decoder(no frame decoded yet)");
        return;
    }
    mp_printf(print, "Decoder(%d Hz, %d ch, %d kbps)",
        self->rate, self->channels, self->bitrate_kbps);
}

static mp_obj_t mp3_decoder_decode(mp_obj_t self_in, mp_obj_t in_in, mp_obj_t out_in) {
    mp3_decoder_obj_t *self = MP_OBJ_TO_PTR(self_in);

    mp_buffer_info_t src;
    mp_get_buffer_raise(in_in, &src, MP_BUFFER_READ);
    mp_buffer_info_t dst;
    mp_get_buffer_raise(out_in, &dst, MP_BUFFER_WRITE);

    /* A frame is up to 1152 samples on each of two channels. Anything shorter
     * could be overrun by a perfectly valid frame, so refuse it here rather
     * than let the decoder write past the end. */
    if (dst.len < MINIMP3_MAX_SAMPLES_PER_FRAME * (int)sizeof(int16_t)) {
        mp_raise_ValueError(MP_ERROR_TEXT("output buffer must hold 1152 stereo frames"));
    }

    mp3dec_frame_info_t info;
    int samples = mp3dec_decode_frame(&self->dec, src.buf, src.len, dst.buf, &info);

    if (info.hz != 0) {
        self->rate = info.hz;
        self->channels = info.channels;
        self->bitrate_kbps = info.bitrate_kbps;
    }

    mp_obj_t ret[2] = {
        MP_OBJ_NEW_SMALL_INT(info.frame_bytes),
        MP_OBJ_NEW_SMALL_INT(samples),
    };
    return mp_obj_new_tuple(2, ret);
}
static MP_DEFINE_CONST_FUN_OBJ_3(mp3_decoder_decode_obj, mp3_decoder_decode);

/* (rate, channels, bitrate_kbps) of the last frame, or None before one has
 * been decoded. A stream can change these mid-flight, so a player should ask
 * again rather than cache the first answer. */
static mp_obj_t mp3_decoder_info(mp_obj_t self_in) {
    mp3_decoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->rate == 0) {
        return mp_const_none;
    }
    mp_obj_t t[3] = {
        MP_OBJ_NEW_SMALL_INT(self->rate),
        MP_OBJ_NEW_SMALL_INT(self->channels),
        MP_OBJ_NEW_SMALL_INT(self->bitrate_kbps),
    };
    return mp_obj_new_tuple(3, t);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp3_decoder_info_obj, mp3_decoder_info);

/* Throw away the decoder's history. Needed after a seek or a dropped chunk:
 * an MP3 frame can depend on the previous one's bit reservoir, so decoding
 * across a gap without this produces a burst of noise. */
static mp_obj_t mp3_decoder_reset(mp_obj_t self_in) {
    mp3_decoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp3dec_init(&self->dec);
    self->rate = 0;
    self->channels = 0;
    self->bitrate_kbps = 0;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp3_decoder_reset_obj, mp3_decoder_reset);

static const mp_rom_map_elem_t mp3_decoder_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_decode), MP_ROM_PTR(&mp3_decoder_decode_obj) },
    { MP_ROM_QSTR(MP_QSTR_info), MP_ROM_PTR(&mp3_decoder_info_obj) },
    { MP_ROM_QSTR(MP_QSTR_reset), MP_ROM_PTR(&mp3_decoder_reset_obj) },
};
static MP_DEFINE_CONST_DICT(mp3_decoder_locals_dict, mp3_decoder_locals_dict_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    mp3_decoder_type,
    MP_QSTR_Decoder,
    MP_TYPE_FLAG_NONE,
    make_new, mp3_decoder_make_new,
    print, mp3_decoder_print,
    locals_dict, &mp3_decoder_locals_dict
    );

static const mp_rom_map_elem_t mp3_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_mp3) },
    { MP_ROM_QSTR(MP_QSTR_Decoder), MP_ROM_PTR(&mp3_decoder_type) },
    /* Sizes a caller needs to allocate correctly: the largest frame the
     * decoder can emit, and the largest input frame it can be asked to find. */
    { MP_ROM_QSTR(MP_QSTR_MAX_SAMPLES_PER_FRAME), MP_ROM_INT(MINIMP3_MAX_SAMPLES_PER_FRAME) },
    { MP_ROM_QSTR(MP_QSTR_MAX_FRAME_BYTES), MP_ROM_INT(MINIMP3_MAX_SAMPLES_PER_FRAME * 2) },
};
static MP_DEFINE_CONST_DICT(mp3_module_globals, mp3_module_globals_table);

const mp_obj_module_t mp3_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mp3_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_mp3, mp3_module);
