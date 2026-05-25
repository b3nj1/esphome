import esphome.codegen as cg
from esphome.components import number
import esphome.config_validation as cv
from esphome.const import CONF_LAMBDA, CONF_MAX_VALUE, CONF_MIN_VALUE, CONF_STEP

from .. import CONF_FRAMED_RS485_ID, FramedRS485Hub, FramedRS485Number

AUTO_LOAD = ["framed_rs485"]

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
    var = await number.new_number(
        config,
        min_value=config[CONF_MIN_VALUE],
        max_value=config[CONF_MAX_VALUE],
        step=config[CONF_STEP],
    )
    hub = await cg.get_variable(config[CONF_FRAMED_RS485_ID])
    cg.add(var.set_parent(hub))

    template_ = await cg.process_lambda(
        config[CONF_LAMBDA],
        [("float", "x")],
        return_type=cg.optional.template(cg.std_vector.template(cg.uint8)),
    )
    cg.add(var.set_template(template_))
