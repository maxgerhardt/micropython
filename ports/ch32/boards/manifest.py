# Modules frozen into the firmware, so they import without a filesystem.

# machine.dht_readinto() is the C half of the DHT11/DHT22 driver; dht.py is the
# Python half that decodes the frame it captures. Shipping only the C function
# leaves "import dht" failing on a board whose flash has just been erased, so
# freeze the pair together the way ports/esp8266 does.
require("dht")
