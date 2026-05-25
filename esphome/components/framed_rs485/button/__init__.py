import esphome.codegen as cg
from esphome.components import button
import esphome.config_validation as cv
from esphome.const import CONF_COMMAND

from .. import CONF_FRAMED_RS485_ID, FramedRS485Hub, framed_rs485_ns, validate_u32

AUTO_LOAD = ["framed_rs485"]

FramedRS485Button = framed_rs485_ns.class_("FramedRS485Button", button.Button)

CONFIG_SCHEMA = button.button_schema(FramedRS485Button).extend(
    {
        cv.GenerateID(CONF_FRAMED_RS485_ID): cv.use_id(FramedRS485Hub),
        cv.Required(CONF_COMMAND): validate_u32,
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_FRAMED_RS485_ID])
    await button.new_button(config, hub, config[CONF_COMMAND])
