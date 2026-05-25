import esphome.codegen as cg
from esphome.components import button
import esphome.config_validation as cv
from esphome.const import CONF_COMMAND

from .. import CONF_FRAMED_RS485_ID, FramedRS485Button, FramedRS485Hub, validate_u32

AUTO_LOAD = ["framed_rs485"]

CONFIG_SCHEMA = button.button_schema(FramedRS485Button).extend(
    {
        cv.GenerateID(CONF_FRAMED_RS485_ID): cv.use_id(FramedRS485Hub),
        cv.Required(CONF_COMMAND): validate_u32,
    }
)


async def to_code(config):
    var = await button.new_button(config)
    hub = await cg.get_variable(config[CONF_FRAMED_RS485_ID])
    cg.add(var.set_parent(hub))
    cg.add(var.set_command_value(config[CONF_COMMAND]))
