import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC, STATE_CLASS_TOTAL_INCREASING

from .. import (
    CONF_DECODE,
    CONF_FRAMED_RS485_ID,
    SENSOR_DECODES,
    FramedRS485Hub,
    framed_rs485_ns,
)

AUTO_LOAD = ["framed_rs485"]

FramedRS485Sensor = framed_rs485_ns.class_(
    "FramedRS485Sensor", sensor.Sensor, cg.Component
)


CONFIG_SCHEMA = sensor.sensor_schema(
    FramedRS485Sensor,
    accuracy_decimals=0,
    state_class=STATE_CLASS_TOTAL_INCREASING,
    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
).extend(
    {
        cv.GenerateID(CONF_FRAMED_RS485_ID): cv.use_id(FramedRS485Hub),
        cv.Required(CONF_DECODE): cv.one_of(*SENSOR_DECODES, lower=True),
    }
)


async def to_code(config):
    var = await sensor.new_sensor(config)
    await cg.register_component(var, config)
    hub = await cg.get_variable(config[CONF_FRAMED_RS485_ID])
    cg.add(var.set_parent(hub))
    cg.add(var.set_decode(SENSOR_DECODES[config[CONF_DECODE]]))
