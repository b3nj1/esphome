import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import CONF_LAMBDA

from .. import (
    CONF_FRAME_TYPE,
    CONF_FRAMED_RS485_ID,
    FramedRS485Hub,
    framed_rs485_ns,
    setup_listener,
    validate_frame_type,
)

AUTO_LOAD = ["framed_rs485"]

FramedRS485TextSensor = framed_rs485_ns.class_(
    "FramedRS485TextSensor", text_sensor.TextSensor
)

CONFIG_SCHEMA = text_sensor.text_sensor_schema(FramedRS485TextSensor).extend(
    {
        cv.GenerateID(CONF_FRAMED_RS485_ID): cv.use_id(FramedRS485Hub),
        cv.Required(CONF_FRAME_TYPE): validate_frame_type,
        # Optional custom decode lambda. When omitted the sensor publishes
        # the last seen frame type as a two-byte hex string (e.g. "01 02").
        cv.Optional(CONF_LAMBDA): cv.returning_lambda,
    }
)


async def to_code(config):
    var = await text_sensor.new_text_sensor(config)
    await setup_listener(var, config)

    if CONF_LAMBDA in config:
        template_ = await cg.process_lambda(
            config[CONF_LAMBDA],
            [
                (
                    cg.std_vector.template(cg.uint8).operator("const").operator("ref"),
                    "payload",
                )
            ],
            return_type=cg.optional.template(cg.std_string),
        )
        cg.add(var.set_template(template_))
