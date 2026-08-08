# Target wiring for the CH32H417.
#
# No external connections are needed for the tests enabled here.

from machine import Pin

# Only SPI1 is listed. Its pins (SCK=PA5, MISO=PA6, MOSI=PA7) are the ones
# known to be free and in the 3.3 V domain on the CH32H417QEU6 board; SPI4's
# default pins double as the SERDES differential pairs, and the SPI2/SPI3
# options land on pins a board is likely to have used for something else.
# One instance is enough for machine_spi_rate.py, which only measures timing.
spi_standalone_args_list = [(1,)]

# Park SPI1's chip select high before any of that traffic starts. The test
# drives SCK and MOSI with no regard for CS, so a device left selected -- or
# one whose CS is floating, which is common on radio breakouts -- would see
# hundreds of bytes of nonsense clocked into it. PA4 is SPI1's natural NSS.
Pin("PA4", Pin.OUT).value(1)
