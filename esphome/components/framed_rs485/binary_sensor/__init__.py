import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.const import CONF_LAMBDA, CONF_TIMEOUT

from .. import (
    CONF_FRAME_TYPE,
    CONF_FRAMED_RS485_ID,
    CONF_MATCH_OFF,
    CONF_MATCH_ON,
    CONF_TEXT_LAMBDA,
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
    has_lambda = CONF_LAMBDA in config
    has_match = CONF_MATCH_ON in config or CONF_MATCH_OFF in config
    if not has_lambda and not has_match:
        raise cv.Invalid(
            "binary_sensor requires either 'lambda' or at least one of 'match_on'/'match_off'"
        )
    if has_lambda and has_match:
        raise cv.Invalid("'lambda' and 'match_on'/'match_off' are mutually exclusive")
    if has_lambda and CONF_TEXT_LAMBDA in config:
        raise cv.Invalid("'text_lambda' is only valid in match mode, not with 'lambda'")
    if has_lambda and CONF_TIMEOUT in config:
        raise cv.Invalid("'timeout' is only valid in match mode, not with 'lambda'")
    return config


CONFIG_SCHEMA = cv.All(
    binary_sensor.binary_sensor_schema(FramedRS485BinarySensor).extend(
        {
            cv.GenerateID(CONF_FRAMED_RS485_ID): cv.use_id(FramedRS485Hub),
            cv.Required(CONF_FRAME_TYPE): validate_frame_type,
            # --- Direct lambda mode ---
            cv.Optional(CONF_LAMBDA): cv.returning_lambda,
            # --- Latching text-match mode ---
            cv.Optional(CONF_MATCH_ON): cv.ensure_list(cv.string),
            cv.Optional(CONF_MATCH_OFF): cv.ensure_list(cv.string),
            cv.Optional(CONF_TIMEOUT): cv.positive_time_period_milliseconds,
            # Optional custom text extractor for latching-match mode.
            # Signature: optional<std::string>(const std::vector<uint8_t> &payload)
            cv.Optional(CONF_TEXT_LAMBDA): cv.returning_lambda,
        }
    ),
    _validate_binary_sensor,
)


async def to_code(config):
    var = await binary_sensor.new_binary_sensor(config)
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
            return_type=cg.optional.template(bool),
        )
        cg.add(var.set_template(template_))
    else:
        for s in config.get(CONF_MATCH_ON, []):
            cg.add(var.add_match_on(s))
        for s in config.get(CONF_MATCH_OFF, []):
            cg.add(var.add_match_off(s))
        if CONF_TIMEOUT in config:
            cg.add(var.set_timeout_ms(config[CONF_TIMEOUT].total_milliseconds))
        if CONF_TEXT_LAMBDA in config:
            text_template = await cg.process_lambda(
                config[CONF_TEXT_LAMBDA],
                [
                    (
                        cg.std_vector.template(cg.uint8)
                        .operator("const")
                        .operator("ref"),
                        "payload",
                    )
                ],
                return_type=cg.optional.template(cg.std_string),
            )
            cg.add(var.set_text_lambda(text_template))
