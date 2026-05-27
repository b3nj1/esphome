import esphome.codegen as cg
from esphome.components import button
import esphome.config_validation as cv
from esphome.const import CONF_COMMAND

from .. import CONF_RS485_FRAME_ID, RS485FrameHub, rs485_frame_ns, validate_u32

AUTO_LOAD = ["rs485_frame"]

RS485FrameButton = rs485_frame_ns.class_("RS485FrameButton", button.Button)

CONFIG_SCHEMA = button.button_schema(RS485FrameButton).extend(
    {
        cv.GenerateID(CONF_RS485_FRAME_ID): cv.use_id(RS485FrameHub),
        cv.Required(CONF_COMMAND): validate_u32,
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_RS485_FRAME_ID])
    await button.new_button(config, hub, config[CONF_COMMAND])
