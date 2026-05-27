import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC, STATE_CLASS_TOTAL_INCREASING

from .. import (
    CONF_DECODE,
    CONF_RS485_FRAME_ID,
    SENSOR_DECODES,
    RS485FrameHub,
    rs485_frame_ns,
)

AUTO_LOAD = ["rs485_frame"]

RS485FrameSensor = rs485_frame_ns.class_(
    "RS485FrameSensor", sensor.Sensor, cg.Component
)


CONFIG_SCHEMA = sensor.sensor_schema(
    RS485FrameSensor,
    accuracy_decimals=0,
    state_class=STATE_CLASS_TOTAL_INCREASING,
    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
).extend(
    {
        cv.GenerateID(CONF_RS485_FRAME_ID): cv.use_id(RS485FrameHub),
        cv.Required(CONF_DECODE): cv.one_of(*SENSOR_DECODES, lower=True),
    }
)


async def to_code(config):
    var = await sensor.new_sensor(config)
    await cg.register_component(var, config)
    hub = await cg.get_variable(config[CONF_RS485_FRAME_ID])
    cg.add(var.set_parent(hub))
    cg.add(var.set_decode(SENSOR_DECODES[config[CONF_DECODE]]))
