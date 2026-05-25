import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.const import CONF_LAMBDA, CONF_TIMEOUT

from .. import (
    BINARY_DECODES,
    CONF_BIT,
    CONF_DECODE,
    CONF_FRAME_TYPE,
    CONF_FRAMED_RS485_ID,
    CONF_MATCH_OFF,
    CONF_MATCH_ON,
    FramedRS485Hub,
    framed_rs485_ns,
    setup_listener,
    validate_frame_type,
)

AUTO_LOAD = ["framed_rs485"]

FramedRS485BinarySensor = framed_rs485_ns.class_(
    "FramedRS485BinarySensor", binary_sensor.BinarySensor
)


def _validate_binary_sensor(config):
    if config[CONF_DECODE] == "display_text_match" and CONF_MATCH_ON not in config:
        raise cv.Invalid("'match_on' is required when decode is 'display_text_match'")
    return config


CONFIG_SCHEMA = cv.All(
    binary_sensor.binary_sensor_schema(FramedRS485BinarySensor).extend(
        {
            cv.GenerateID(CONF_FRAMED_RS485_ID): cv.use_id(FramedRS485Hub),
            cv.Required(CONF_FRAME_TYPE): validate_frame_type,
            cv.Optional(CONF_DECODE, default="led_bit"): cv.one_of(
                *BINARY_DECODES, lower=True
            ),
            cv.Optional(CONF_BIT, default=0): cv.int_range(min=0, max=31),
            # display_text_match options:
            cv.Optional(CONF_MATCH_ON): cv.ensure_list(cv.string),
            cv.Optional(CONF_MATCH_OFF): cv.ensure_list(cv.string),
            cv.Optional(CONF_TIMEOUT): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_LAMBDA): cv.returning_lambda,
        }
    ),
    _validate_binary_sensor,
)


async def to_code(config):
    var = await binary_sensor.new_binary_sensor(config)
    await setup_listener(var, config)

    cg.add(var.set_decode(BINARY_DECODES[config[CONF_DECODE]]))
    cg.add(var.set_bit(config[CONF_BIT]))

    for s in config.get(CONF_MATCH_ON, []):
        cg.add(var.add_match_on(s))
    for s in config.get(CONF_MATCH_OFF, []):
        cg.add(var.add_match_off(s))
    if CONF_TIMEOUT in config:
        cg.add(var.set_timeout_ms(config[CONF_TIMEOUT].total_milliseconds))

    if CONF_LAMBDA in config:
        template_ = await cg.process_lambda(
            config[CONF_LAMBDA],
            [
                (
                    cg.std_vector.template(cg.uint8).operator("const").operator("ref"),
                    "payload",
                )
            ],
            return_type=cg.optional.template(bool),
        )
        cg.add(var.set_template(template_))
