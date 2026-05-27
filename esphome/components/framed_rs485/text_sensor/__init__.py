import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from .. import CONF_FRAMED_RS485_ID, FramedRS485Hub, framed_rs485_ns

AUTO_LOAD = ["framed_rs485"]

FramedRS485TextSensor = framed_rs485_ns.class_(
    "FramedRS485TextSensor", text_sensor.TextSensor, cg.Component
)

# The text_sensor platform exposes a single diagnostic: the most recent validated frame
# type (first two payload bytes) as a 4-character hex string. User text decoding is done
# via on_frame: + a template text_sensor.
CONFIG_SCHEMA = text_sensor.text_sensor_schema(
    FramedRS485TextSensor,
    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
).extend(
    {
        cv.GenerateID(CONF_FRAMED_RS485_ID): cv.use_id(FramedRS485Hub),
    }
)


async def to_code(config):
    var = await text_sensor.new_text_sensor(config)
    await cg.register_component(var, config)
    hub = await cg.get_variable(config[CONF_FRAMED_RS485_ID])
    cg.add(var.set_parent(hub))
