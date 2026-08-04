import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import CONF_PORT

from . import kasa_plug_ns

CONF_HOST = "host"

# async_tcp provides the cross-platform AsyncClient used for the outbound TCP
# connection (AsyncTCP libraries on ESP32/ESP8266/RP2040/LibreTiny, a
# socket-based implementation elsewhere). json parses the plug's replies.
DEPENDENCIES = ["network"]
AUTO_LOAD = ["async_tcp", "json"]

KasaPlugSwitch = kasa_plug_ns.class_(
    "KasaPlugSwitch", switch.Switch, cg.PollingComponent
)

CONFIG_SCHEMA = (
    switch.switch_schema(KasaPlugSwitch)
    .extend(
        {
            # IP address (or resolvable hostname) of the Kasa plug on the LAN.
            cv.Required(CONF_HOST): cv.string,
            # Kasa devices listen on TCP 9999; exposed for the rare custom setup.
            cv.Optional(CONF_PORT, default=9999): cv.port,
        }
    )
    .extend(cv.polling_component_schema("30s"))
)


async def to_code(config):
    var = await switch.new_switch(config)
    await cg.register_component(var, config)

    cg.add(var.set_host(config[CONF_HOST]))
    cg.add(var.set_port(config[CONF_PORT]))
