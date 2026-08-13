# Modules frozen into the firmware, so they import without a filesystem.

# machine.dht_readinto() is the C half of the DHT11/DHT22 driver; dht.py is the
# Python half that decodes the frame it captures. Shipping only the C function
# leaves "import dht" failing on a board whose flash has just been erased, so
# freeze the pair together the way ports/esp8266 does.
require("dht")

# HTTP client. requests pulls in nothing else and works over either a plain
# socket or an ssl-wrapped one, so `import requests` covers http and https
# without a filesystem present.
require("requests")

# The C module that mbedtls provides is called `tls`; `ssl` is the CPython-named
# wrapper over it, and requests imports that name.
require("ssl")
