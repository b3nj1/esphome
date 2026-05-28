import esphome.codegen as cg
from esphome.components import button
import esphome.config_validation as cv
from esphome.const import CONF_COMMAND, CONF_PAYLOAD
import esphome.final_validate as fv

from .. import (
    CONF_FRAME_TYPE,
    CONF_KEY_FORMAT,
    CONF_PROFILE,
    CONF_RS485_FRAME_ID,
    PROFILE_GENERIC,
    RS485FrameHub,
    rs485_frame_ns,
    validate_byte,
    validate_frame_type,
    validate_u32,
)

AUTO_LOAD = ["rs485_frame"]

RS485FrameButton = rs485_frame_ns.class_("RS485FrameButton", button.Button)


def _validate_button(config):
    # Exactly one of `command:` (uses the hub's key_format) or the raw pair
    # (`frame_type:` + `payload:`, bypasses key_format) must be supplied.
    has_command = CONF_COMMAND in config
    has_frame_type = CONF_FRAME_TYPE in config
    has_payload = CONF_PAYLOAD in config

    if has_command and (has_frame_type or has_payload):
        raise cv.Invalid(
            "rs485_frame button: 'command' is mutually exclusive with 'frame_type'/'payload'. "
            "Use 'command' to encode via the hub's key_format, or use 'frame_type' + 'payload' "
            "to emit a raw frame."
        )
    if has_frame_type != has_payload:
        raise cv.Invalid(
            "rs485_frame button raw form requires both 'frame_type' and 'payload'."
        )
    if not has_command and not has_frame_type:
        raise cv.Invalid(
            "rs485_frame button requires either 'command' or 'frame_type' + 'payload'."
        )
    return config


CONFIG_SCHEMA = cv.All(
    button.button_schema(RS485FrameButton).extend(
        {
            cv.GenerateID(CONF_RS485_FRAME_ID): cv.use_id(RS485FrameHub),
            cv.Optional(CONF_COMMAND): validate_u32,
            cv.Optional(CONF_FRAME_TYPE): validate_frame_type,
            cv.Optional(CONF_PAYLOAD): cv.ensure_list(validate_byte),
        }
    ),
    _validate_button,
)


def _final_validate(config):
    # The `command:` form is only meaningful when the hub has a key_format. Every named
    # profile (hayward_*, jandy_*) supplies a key_format via _profile_defaults; only
    # generic_rs485_frame leaves it unset (it inherits wireless_12byte as an unrelated
    # placeholder default). Reject `command:` against a generic hub at config time so
    # users see a clear error instead of silently emitting a 12-byte Hayward wireless
    # frame from a hub that has nothing to do with Hayward wireless.
    if CONF_COMMAND not in config:
        return config

    full_config = fv.full_config.get()
    hub_path = full_config.get_path_for_id(config[CONF_RS485_FRAME_ID])[:-1]
    hub_config = full_config.get_config_for_path(hub_path)
    if (
        hub_config.get(CONF_PROFILE) == PROFILE_GENERIC
        and CONF_KEY_FORMAT not in hub_config
    ):
        raise cv.Invalid(
            f"rs485_frame button 'command' requires the hub to have a 'key_format' set. "
            f"profile: {PROFILE_GENERIC} hubs have no built-in key_format — use the raw "
            f"'frame_type' + 'payload' form, or set 'key_format' explicitly on the hub."
        )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    hub = await cg.get_variable(config[CONF_RS485_FRAME_ID])
    if CONF_COMMAND in config:
        await button.new_button(config, hub, config[CONF_COMMAND])
    else:
        await button.new_button(
            config, hub, config[CONF_FRAME_TYPE], config[CONF_PAYLOAD]
        )
