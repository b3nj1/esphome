import esphome.codegen as cg
from esphome.components import number
import esphome.config_validation as cv
from esphome.const import CONF_LAMBDA, CONF_MAX_VALUE, CONF_MIN_VALUE, CONF_STEP

from .. import CONF_FRAMED_RS485_ID, FramedRS485Hub, framed_rs485_ns

AUTO_LOAD = ["framed_rs485"]

FramedRS485Number = framed_rs485_ns.class_("FramedRS485Number", number.Number)

CONFIG_SCHEMA = number.number_schema(FramedRS485Number).extend(
    {
        cv.GenerateID(CONF_FRAMED_RS485_ID): cv.use_id(FramedRS485Hub),
        cv.Required(CONF_MIN_VALUE): cv.float_,
        cv.Required(CONF_MAX_VALUE): cv.float_,
        cv.Required(CONF_STEP): cv.positive_float,
        cv.Required(CONF_LAMBDA): cv.returning_lambda,
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_FRAMED_RS485_ID])
    var = await number.new_number(
        config,
        hub,
        min_value=config[CONF_MIN_VALUE],
        max_value=config[CONF_MAX_VALUE],
        step=config[CONF_STEP],
    )

    template_ = await cg.process_lambda(
        config[CONF_LAMBDA],
        [(cg.float_, "x")],
        return_type=cg.optional.template(cg.std_vector.template(cg.uint8)),
    )
    cg.add(var.set_template(template_))
