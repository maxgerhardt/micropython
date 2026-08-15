# mp3.Decoder -- MP3 frames in, signed 16-bit stereo PCM out.
#
# Self-contained: the test signal is a 0.15 s 1 kHz tone encoded at 128 kbps,
# 44.1 kHz, stereo -- the same shape as an internet radio stream -- embedded
# below so this needs no file, no card and no network. It is a synthesised
# sine, not a recording.
#
# Hex rather than base64 on purpose. Base64 produces runs that look like words
# and codespell tried to spell-correct one in the middle of the blob; hex has
# only 0-9a-f and cannot collide with a dictionary.
#
# Run on the target: python scripts/install_file.py ports/ch32/test_mp3.py
import binascii
import struct

import mp3

TONE_HEX = (
    "49443304000000000022545353450000000e0000034c61766636322e312e313030000000"
    "0000000000000000fffb9000000000000000000000000000000000000000000000000000"
    "0000000000000000496e666f0000000f0000000700000d0e003f3f3f3f3f3f3f3f3f3f3f"
    "3f3f3f5f5f5f5f5f5f5f5f5f5f5f5f5f5f7f7f7f7f7f7f7f7f7f7f7f7f7f7f9f9f9f9f9f"
    "9f9f9f9f9f9f9f9f9f9fbfbfbfbfbfbfbfbfbfbfbfbfbfbfdfdfdfdfdfdfdfdfdfdfdfdf"
    "dfdfffffffffffffffffffffffffffff000000004c61766336322e332e00000000000000"
    "00000000002403690000000000000d0e2f5fa29c00000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000fffb9064000001"
    "fb0bd31d3c600000000d20a0000119b5a11c39ea8000000034830000000afdb6fd5eaf57"
    "abd5ecf1eefd5914ff20e2161ab13725ea311100000100c41f0ff04389c1fe083a73a7dd"
    "cbfbba7ddd3eee5c1fc3e081ca8060fe9041da7dc0010984888818948b03a2e0edc131e7"
    "29231e82e95b461868757cc324b74f4ba270c7f49d8c62e92cc010398f565d90c6f439c0"
    "cecdf03692cc7289fc0d4cb703149680cb652274a27b8194cbc060e17818c05e062e1c97"
    "92747c0c0218030e8680c3a2200806818381eb6529fe060e0a80b8100c0e0603038281ba"
    "2060000ad7dff030000c36606e206c3074216421613fff89b8315060214886ad0d5a31c2"
    "0b082dffff8ae86ad0f946384161410e91408b94870b9bfffff22c624549d3222c6cb2e9"
    "e5249d1fffffffba919dcbbf5d550c05e0074c066035cc0fe036cc0bc0164c09204fcc21"
    "a0f4cc57e1684c5929274d5962d20c38905c4c19a02f4c0bc03e4c08400f8d1483b680c4"
    "8a58f86f5affeebf99fe13a5882983f97942a6daed7178d49516d1125c29f64c8ba45dfa"
    "deb41c40d63f8f4a54808b549194fffb92643805f3ce1242877f400000000d20e000010d"
    "bc490b0d7f6840000034800000041222828cd3d2eda3166e4019dcb0c89041ca1262c919"
    "84a6bd61d1b4603d0142607e832861d93d427364094c60f701fe752c86884224348ca592"
    "47d71b9ad7f7f7aef71fc2db013530315b4c30b8b946181442cc1e36392a26fa6bb95f75"
    "130b7f370e350fb1af174ea405d14b639a61a5fb7933fb47b0c280000018003bcde8a840"
    "d111049a310b4d0aa39f10c08a019cc12d03d0c4744b74eea20b50c2580244c0d300a820"
    "0c1300940010a801a3a003268bed5b3d91c89506558fd1d536aee9870a264534873edb59"
    "2fdeaaef5d38f44485ba66c720737a628ab948174c90dd8692e165362f1f15eed600657a"
    "3bf8e8aa6074d9552089a98f5473d41813c0098b05a462ad04407bf80076616b00047070"
    "39a082464110983c3a140824bbd1dd5ddffeb59639fe3382a265a42bcd4b8a809a67540c"
    "1b1a90017757282eeb9fd844191440abb9a5113a416c1dccb533638f2d8766099a26b6bb"
    "e86ed72a4c414d45332e313030aaaaefeb44a00a1806200b800020008052580058a00623"
    "fffb92648305f3b230c2cb5f2a3000000d200000010df049080d7f884000003480000004"
    "02180483052c117314116f23d23837530aa00be3f75937e3a3473331b213080e2ebb9060"
    "6afadc7652093aab58eecbfcdc98ccd664de8eecab7defb534d16ad656d3f6a58d477551"
    "bf4f61dec7c7dcc005140492a203a0626588a98e6f9015db9bef2bd41d20397233242a33"
    "12f3240230f49302480f43062c2b9318535023fe9881f30d001d8304dc0e1302a8088301"
    "c4065300ec03b300a801a3003c0034d44212eb26c7a15300999e26d1eed3ea3c72cf8d98"
    "1c7b69332bd7767a97a348fb5889e3a5fce69aa81bf715ff9f710b3323ea7137c5711df2"
    "4b1b31d4451950d5f14f15cfb28c18b68331d53f03647aa8cec6c455fe3b530ea64c70b8"
    "ea4c414d45332e313030aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaefeaa0e821d183995041"
    "9d8099e0498f399804808c18186179986adc5c1c7544fffb9264c98ff3b0454203fb1310"
    "00000d20000001137185020dfd08c000003480000004589838c0fc18152074980c404218"
    "0960299805e0129805000304003ea42f5db9fcfc3b7f75f7765dd465e52462efb59f82ac"
    "49721e5cb07e80af6d1f6883cf23fc45bf69e56ffedcc796c7906ff3e7e0e90fc3b6229c"
    "cfb0e5233f31ec52f527fc931764236d3e7afffc1da67dba5b46eff708b1402be3c02e98"
    "086002180a0017980a804a9806609c180241959802de28185d44a09802010c185f998c40"
    "194a29938789281111ab47d4cb0ffadbbf7f9bdc831907b54a83ccfccefe1606d4b247be"
    "5489d9732a65629db72f952e733cefff8dcad7a3240445fe508ba630dc36f21eccfb8ea5"
    "e5ef1290c56f2799c1cee516cfbffff71d7cfe99278cc54c414d45332e31303055555555"
    "555555555555555555555555555555555555555555555555555555555555555555555555"
    "555555555555555555ef37c1d033a50432a0f3412835847376a130214132303a030530cf"
    "79383873c8c7306ac1f8302180df3009406c1080523401386007a2400f2cd1c1834f7ae8"
    "1a82016947b71a17fffb9264cf0ff4806bc1037f1a6000000d200000011195af040fec6d"
    "0800003480000004972c93c34862a64a54ccf22407489a99a6253eeff8a3bf411e5e5df7"
    "e10c9e33e13e7f322e313f9948b245fe8ec0b3fa633839f5cd7fd41372913667fe5ffda6"
    "3b6b36f0eeb1b6c0c2e01098156031981fe0ab981b601a181dc09d183ba0c89813a1a919"
    "0e061a198ddd771ff7e9869a1264e7988be115982aa02e182a006501a01900616258191c"
    "96064d11b981dbb2266eb335a44298b98a0d48f2272eb5aa793558c5d03e793763e95349"
    "8e20cb453659d3a6ee9acf1ea9dd5cb4b5bb2073755b5ad26a08ba94b52ce275eeb5a0eb"
    "38713433e9aec6abb33296a3c933bd352d8ea47d4a4cfbaf63871d933b40d55765548fe9"
    "a0796bd6b74994e6ca4c414d45332e313030aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaa400000000e4c49d9359926410002cd8c02862d03e638868864a760a0"
    "50c331f4c5804cce277484027f4ff75f8c770a4dfc4bcc011c1a523a8295b0c37740c2ab"
    "ab800479c32e0820679702020297ca9c297b3489bc4ae0d0060525052212fffb9264e28f"
    "f45d65c1037f1a3000000d2000000116f9af0215fa80000000348280000412d89f6a7829"
    "bc9e771b4979ae5e63540c1d3087c105a555e82af258ea4aefd4a13269412140cd0cc850"
    "7220325997d6920988d48bcfd997c5ec661c48c58c040b210c1510210a0809777fbd7ead"
    "e1cfd61cee10c36ed31d571a05672e9413afd6bf5fffcffe7ffffffca662575e725f6a76"
    "d5355ffffffffffffefff7ffffffff9f72b57a985bb18dae55adbaa73ffffe0f826018e6"
    "01d813c8584340390e556071017402104ea302a805a0ea28414a1f04a035404c048764e2"
    "08842513a339124f5319304a274672249ec064eb4f6ad5aee2e05435ac152c1d12814240"
    "d0f0d82aa0eaf0554781a5868b28f7ddf869478aae0d2cefe494788ce9651e4c414d4533"
    "2e313030aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaafffb9264ee00078b5e4c6e77400000000d20c000"
    "000d346cce3cf6000000003483800004aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaa"
)

TONE = binascii.unhexlify(TONE_HEX)

passed = 0
failed = 0


def check(name, cond):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print("FAIL", name)


def raises(name, exc, fn):
    try:
        fn()
    except exc:
        check(name, True)
        return
    except Exception as e:
        check(name + " (raised %s)" % type(e).__name__, False)
        return
    check(name + " (raised nothing)", False)


check("mp3.Decoder exists", hasattr(mp3, "Decoder"))
check("frame size constant", mp3.MAX_SAMPLES_PER_FRAME == 1152 * 2)
check("test vector decoded from hex", len(TONE) > 2000)

d = mp3.Decoder()
check("info() is None before any frame", d.info() is None)
check("repr says nothing decoded yet", "no frame" in repr(d))

pcm = bytearray(mp3.MAX_SAMPLES_PER_FRAME * 2)

# An output buffer that cannot hold a full frame must be refused rather than
# overrun: a valid frame is 1152 samples on each of two channels.
raises("short output buffer refused", ValueError, lambda: d.decode(TONE, bytearray(64)))

# Input too short to hold a frame produces no audio. It does not follow that
# nothing is consumed: minimp3 discards bytes it cannot make a frame header
# out of, so this returns (1, 0) rather than (0, 0). That is the documented
# "used > 0, got == 0" resynchronising case, and it matters that it consumes
# something -- a decoder that returned (0, 0) on junk would let a caller loop
# forever on a stream that never syncs.
used, got = d.decode(TONE[:1], pcm)
check("a single byte produces no audio", got == 0)
check("a single byte of junk is consumed, not stalled on", used == 1)

# --- decode the whole thing.
total_frames = 0
total_samples = 0
peak = 0
pos = 0
while pos < len(TONE):
    used, got = d.decode(TONE[pos:], pcm)
    if used == 0:
        break
    pos += used
    if got == 0:
        continue  # skipped a tag or resynchronised
    total_frames += 1
    total_samples += got
    for i in range(0, got * 4, 128):  # sample the output rather than scan it all
        (v,) = struct.unpack_from("<h", pcm, i)
        if abs(v) > peak:
            peak = abs(v)

rate, channels, kbps = d.info()
print(
    "  %d Hz, %d ch, %d kbps; %d frames, %d samples, peak %d"
    % (rate, channels, kbps, total_frames, total_samples, peak)
)

check("sample rate is 44100", rate == 44100)
check("stereo", channels == 2)
check("bitrate is 128 kbps", kbps == 128)
check("decoded several frames", total_frames >= 4)
# 0.15 s at 44.1 kHz is 6615 samples; the encoder pads with a frame or two.
check("roughly the right amount of audio", 5000 < total_samples < 12000)
# A 1 kHz tone at full scale should be loud. Silence here would mean the
# decoder ran without error and produced nothing, which is the failure that
# looks like success.
check("output is not silence", peak > 3000)
check("output is not clipping garbage", peak <= 32767)

# --- reset forgets everything.
d.reset()
check("info() is None again after reset", d.info() is None)

# A second decoder must be independent of the first.
d2 = mp3.Decoder()
used, got = d2.decode(TONE, pcm)
check("a fresh decoder works on its own", used > 0)

print("%u passed, %u failed" % (passed, failed))
