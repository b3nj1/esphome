import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import CONF_LAMBDA, CONF_OFFSET, STATE_CLASS_MEASUREMENT

from .. import (
    CONF_DECODE,
    CONF_FRAME_TYPE,
    CONF_FRAMED_RS485_ID,
    CONF_TEMPERATURE_LABEL,
    SENSOR_DECODES,
    FramedRS485Hub,
    FramedRS485Sensor,
    setup_listener,
    validate_frame_type,
)

AUTO_LOAD = ["framed_rs485"]


CONFIG_SCHEMA = sensor.sensor_schema(
    FramedRS485Sensor,
    accuracy_decimals=0,
    state_class=STATE_CLASS_MEASUREMENT,
).extend(
    {
        cv.GenerateID(CONF_FRAMED_RS485_ID): cv.use_id(FramedRS485Hub),
        cv.Required(CONF_FRAME_TYPE): validate_frame_type,
        cv.Optional(CONF_DECODE, default="uint8"): cv.one_of(
            *SENSOR_DECODES, lower=True
        ),
        cv.Optional(CONF_OFFSET, default=0): cv.positive_int,
        cv.Optional(CONF_TEMPERATURE_LABEL, default=""): cv.string_strict,
        cv.Optional(CONF_LAMBDA): cv.returning_lambda,
    }
)


async def to_code(config):
    var = await sensor.new_sensor(config)
    await setup_listener(var, config)

    cg.add(var.set_decode(SENSOR_DECODES[config[CONF_DECODE]]))
    cg.add(var.set_offset(config[CONF_OFFSET]))
    cg.add(var.set_temperature_label(config[CONF_TEMPERATURE_LABEL].lower()))

    if CONF_LAMBDA in config:
        template_ = await cg.process_lambda(
            config[CONF_LAMBDA],
            [
                (
                    cg.std_vector.template(cg.uint8).operator("const").operator("ref"),
                    "payload",
                )
            ],
            return_type=cg.optional.template(float),
        )
        cg.add(var.set_template(template_))
