"""TP-Link Kasa smart plug external component for ESPHome.

This component controls TP-Link Kasa smart plugs (e.g. HS103, HS105, KP115)
over the local network, without any cloud dependency. It is a port of the
KasaSmartPlug Arduino library (https://github.com/kj831ca/KasaSmartPlug) to the
ESPHome component API.
"""

import esphome.codegen as cg

CODEOWNERS = ["@tonymacdonald1995"]

# The C++ namespace that all of this component's symbols live in.
kasa_plug_ns = cg.esphome_ns.namespace("kasa_plug")
