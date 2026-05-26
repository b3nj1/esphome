from esphome import automation
import esphome.codegen as cg
from esphome.components import uart
from esphome.components.const import CONF_DATA_BITS, CONF_PARITY, CONF_STOP_BITS
import esphome.config_validation as cv
from esphome.const import (
    CONF_BAUD_RATE,
    CONF_DELAY,
    CONF_ID,
    CONF_INTERVAL,
    CONF_MODE,
    CONF_TRIGGER_ID,
    CONF_TYPE,
    CONF_UART_ID,
)
from esphome.core import HexInt
import esphome.final_validate as fv

CODEOWNERS = ["@b3nj1"]
DEPENDENCIES = ["uart"]
MULTI_CONF = True

framed_rs485_ns = cg.esphome_ns.namespace("framed_rs485")
FramedRS485Hub = framed_rs485_ns.class_("FramedRS485Hub", cg.Component, uart.UARTDevice)
FramedRS485FrameTrigger = framed_rs485_ns.class_(
    "FramedRS485FrameTrigger",
    automation.Trigger.template(cg.std_vector.template(cg.uint8)),
)

SensorDecode = framed_rs485_ns.enum("SensorDecode")
KeyFormat = framed_rs485_ns.enum("KeyFormat")
CrcVariant = framed_rs485_ns.enum("CrcVariant")
CrcType = framed_rs485_ns.enum("CrcType")
QueuePolicy = framed_rs485_ns.enum("QueuePolicy")
TxGateMode = framed_rs485_ns.enum("TxGateMode")

CONF_FRAMED_RS485_ID = "framed_rs485_id"
CONF_CRC = "crc"
CONF_DECODE = "decode"
CONF_DLE = "dle"
CONF_DUMP_FRAMES = "dump_frames"
CONF_ESCAPE_BYTE = "escape_byte"
CONF_ETX = "etx"
CONF_FRAME_TIMEOUT = "frame_timeout"
CONF_FRAME_TYPE = "frame_type"
CONF_FRAMING = "framing"
CONF_GATE = "gate"
CONF_KEY_FORMAT = "key_format"
CONF_MAX_FRAME_LENGTH = "max_frame_length"
CONF_MAX_QUEUE_SIZE = "max_queue_size"
CONF_MIN_SILENCE = "min_silence"
CONF_PROFILE = "profile"
CONF_QUEUE_POLICY = "queue_policy"
CONF_RX_ACCEPT = "rx_accept"
CONF_SNIFFER_ONLY = "sniffer_only"
CONF_STX = "stx"
CONF_MATCH_ON = "match_on"
CONF_MATCH_OFF = "match_off"
CONF_ON_FRAME = "on_frame"
CONF_TEXT_LAMBDA = "text_lambda"
CONF_IDLE_COMMAND = "idle_command"
CONF_TX = "tx"
CONF_TX_VARIANT = "tx_variant"

PROFILE_HAYWARD_WIRELESS = "hayward_aqualogic_wireless"
PROFILE_HAYWARD_WIRED_REMOTE = "hayward_aqualogic_wired_remote"
PROFILE_HAYWARD_WIRED_LOCAL = "hayward_aqualogic_wired_local"
PROFILE_JANDY_RS = "jandy_aqualink_rs"
PROFILE_GENERIC = "generic_framed_rs485"

KEY_FORMATS = {
    "wireless_9byte": KeyFormat.KEY_FORMAT_WIRELESS_9BYTE,
    "wired_remote": KeyFormat.KEY_FORMAT_WIRED_REMOTE,
    "wired_local": KeyFormat.KEY_FORMAT_WIRED_LOCAL,
    "jandy_allbutton": KeyFormat.KEY_FORMAT_JANDY_ALLBUTTON,
}

CRC_VARIANTS = {
    "header_inclusive": CrcVariant.CRC_HEADER_INCLUSIVE,
    "payload_only": CrcVariant.CRC_PAYLOAD_ONLY,
}

CRC_TYPES = {
    "none": CrcType.CRC_TYPE_NONE,
    "sum8": CrcType.CRC_TYPE_SUM8,
    "sum16": CrcType.CRC_TYPE_SUM16,
    "xor8": CrcType.CRC_TYPE_XOR8,
    "crc16_modbus": CrcType.CRC_TYPE_CRC16_MODBUS,
}

QUEUE_POLICIES = {
    "replace_latest": QueuePolicy.QUEUE_REPLACE_LATEST,
    "fifo": QueuePolicy.QUEUE_FIFO,
}

TX_GATE_MODES = {
    "frame_trigger": TxGateMode.TX_GATE_FRAME_TRIGGER,
    "idle_gap": TxGateMode.TX_GATE_IDLE_GAP,
    "fixed_delay": TxGateMode.TX_GATE_FIXED_DELAY,
}

SENSOR_DECODES = {
    "uint8": SensorDecode.SENSOR_DECODE_UINT8,
    "uint16_be": SensorDecode.SENSOR_DECODE_UINT16_BE,
    "uint16_le": SensorDecode.SENSOR_DECODE_UINT16_LE,
    "uint32_be": SensorDecode.SENSOR_DECODE_UINT32_BE,
    "uint32_le": SensorDecode.SENSOR_DECODE_UINT32_LE,
    "bcd": SensorDecode.SENSOR_DECODE_BCD,
    "frames_received": SensorDecode.SENSOR_DECODE_FRAMES_RECEIVED,
    "crc_failures": SensorDecode.SENSOR_DECODE_CRC_FAILURES,
    "commands_sent": SensorDecode.SENSOR_DECODE_COMMANDS_SENT,
    "command_drops": SensorDecode.SENSOR_DECODE_COMMAND_DROPS,
    "last_keepalive_ms": SensorDecode.SENSOR_DECODE_LAST_KEEPALIVE_MS,
    "queue_depth": SensorDecode.SENSOR_DECODE_QUEUE_DEPTH,
}


def validate_byte(value):
    value = cv.hex_uint8_t(value)
    return HexInt(int(value))


def validate_u32(value):
    value = cv.hex_uint32_t(value)
    return HexInt(int(value))


def validate_frame_type(value):
    return cv.ensure_list(validate_byte)(value)


def _profile_defaults(profile):
    if profile == PROFILE_HAYWARD_WIRED_REMOTE:
        return {
            CONF_KEY_FORMAT: "wired_remote",
            CONF_TX_VARIANT: "payload_only",
        }
    if profile == PROFILE_HAYWARD_WIRED_LOCAL:
        return {
            CONF_KEY_FORMAT: "wired_local",
            CONF_TX_VARIANT: "payload_only",
        }
    if profile == PROFILE_JANDY_RS:
        return {
            CONF_KEY_FORMAT: "jandy_allbutton",
            CONF_TX_VARIANT: "header_inclusive",
            CONF_TYPE: "sum8",
            CONF_RX_ACCEPT: ["header_inclusive"],
        }
    return {
        CONF_KEY_FORMAT: "wireless_9byte",
        CONF_TX_VARIANT: "header_inclusive",
    }


FRAMING_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_DLE, default=0x10): validate_byte,
        cv.Optional(CONF_STX, default=0x02): validate_byte,
        cv.Optional(CONF_ETX, default=0x03): validate_byte,
        cv.Optional(CONF_ESCAPE_BYTE, default=0x00): validate_byte,
    }
)

CRC_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_TYPE, default="sum16"): cv.one_of(*CRC_TYPES, lower=True),
        cv.Optional(
            CONF_RX_ACCEPT, default=["header_inclusive", "payload_only"]
        ): cv.ensure_list(cv.one_of(*CRC_VARIANTS, lower=True)),
        cv.Optional(CONF_TX_VARIANT): cv.one_of(*CRC_VARIANTS, lower=True),
    }
)

TX_GATE_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_MODE, default="frame_trigger"): cv.one_of(
            *TX_GATE_MODES, lower=True
        ),
        # Schema caps gate.frame_type at 8 bytes to match the fixed dump_config buffer.
        cv.Optional(CONF_FRAME_TYPE, default=[0x01, 0x01]): cv.All(
            validate_frame_type, cv.Length(max=8)
        ),
        # delay=0 is valid (no delay after the gate frame before transmitting).
        cv.Optional(CONF_DELAY, default="0ms"): cv.positive_time_period_milliseconds,
        cv.Optional(
            CONF_MIN_SILENCE, default="4ms"
        ): cv.positive_time_period_milliseconds,
        cv.Optional(
            CONF_INTERVAL, default="100ms"
        ): cv.positive_time_period_milliseconds,
    }
)

TX_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_GATE, default={}): TX_GATE_SCHEMA,
        cv.Optional(CONF_QUEUE_POLICY, default="replace_latest"): cv.one_of(
            *QUEUE_POLICIES, lower=True
        ),
        cv.Optional(CONF_MAX_QUEUE_SIZE, default=1): cv.positive_int,
        cv.Optional(CONF_IDLE_COMMAND): validate_u32,
    }
)


def validate_hub(config):
    profile = config[CONF_PROFILE]
    defaults = _profile_defaults(profile)

    if CONF_KEY_FORMAT not in config:
        config[CONF_KEY_FORMAT] = defaults[CONF_KEY_FORMAT]

    if CONF_TX_VARIANT not in config[CONF_CRC]:
        config[CONF_CRC][CONF_TX_VARIANT] = defaults[CONF_TX_VARIANT]
    if CONF_TYPE in defaults and CONF_TYPE not in config[CONF_CRC]:
        config[CONF_CRC][CONF_TYPE] = defaults[CONF_TYPE]
    if CONF_RX_ACCEPT in defaults and CONF_RX_ACCEPT not in config[CONF_CRC]:
        config[CONF_CRC][CONF_RX_ACCEPT] = defaults[CONF_RX_ACCEPT]

    if (
        config[CONF_TX][CONF_QUEUE_POLICY] == "replace_latest"
        and config[CONF_TX][CONF_MAX_QUEUE_SIZE] != 1
    ):
        raise cv.Invalid("replace_latest requires max_queue_size: 1")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(FramedRS485Hub),
            cv.Optional(CONF_PROFILE, default=PROFILE_HAYWARD_WIRELESS): cv.one_of(
                PROFILE_HAYWARD_WIRELESS,
                PROFILE_HAYWARD_WIRED_REMOTE,
                PROFILE_HAYWARD_WIRED_LOCAL,
                PROFILE_JANDY_RS,
                PROFILE_GENERIC,
                lower=True,
            ),
            cv.Optional(CONF_FRAMING, default={}): FRAMING_SCHEMA,
            cv.Optional(CONF_CRC, default={}): CRC_SCHEMA,
            cv.Optional(CONF_TX, default={}): TX_SCHEMA,
            cv.Optional(CONF_KEY_FORMAT): cv.one_of(*KEY_FORMATS, lower=True),
            cv.Optional(CONF_DUMP_FRAMES, default=False): cv.boolean,
            cv.Optional(CONF_SNIFFER_ONLY, default=False): cv.boolean,
            cv.Optional(CONF_MAX_FRAME_LENGTH, default=128): cv.positive_int,
            cv.Optional(
                CONF_FRAME_TIMEOUT, default="50ms"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_ON_FRAME): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(
                        FramedRS485FrameTrigger
                    ),
                    cv.Required(CONF_FRAME_TYPE): validate_frame_type,
                }
            ),
        }
    )
    .extend(uart.UART_DEVICE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA),
    validate_hub,
)


def _final_validate(config):
    profile = config[CONF_PROFILE]
    if profile == PROFILE_GENERIC:
        return config

    full_config = fv.full_config.get()
    uart_path = full_config.get_path_for_id(config[CONF_UART_ID])[:-1]
    uart_config = full_config.get_config_for_path(uart_path)

    if profile == PROFILE_JANDY_RS:
        required = {
            CONF_BAUD_RATE: 9600,
            CONF_DATA_BITS: 8,
            CONF_PARITY: "NONE",
            CONF_STOP_BITS: 1,
        }
        for key, expected in required.items():
            # UART schema always provides validated defaults, so direct access is safe.
            if uart_config[key] != expected:
                raise cv.Invalid(
                    f"Framed RS-485 jandy_aqualink_rs profile requires uart {key}: {expected}; "
                    f"use profile: {PROFILE_GENERIC} for other serial settings"
                )
        return config

    required = {
        CONF_BAUD_RATE: 19200,
        CONF_DATA_BITS: 8,
        CONF_PARITY: "NONE",
        CONF_STOP_BITS: 2,
    }
    for key, expected in required.items():
        if uart_config[key] != expected:
            raise cv.Invalid(
                f"Framed RS-485 Hayward profiles require uart {key}: {expected}; "
                f"use profile: {PROFILE_GENERIC} for other serial settings"
            )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def setup_listener(var, config):
    cg.add(var.set_frame_type(config[CONF_FRAME_TYPE]))
    hub = await cg.get_variable(config[CONF_FRAMED_RS485_ID])
    cg.add(hub.register_listener(var))


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    framing = config[CONF_FRAMING]
    cg.add(
        var.set_framing(
            framing[CONF_DLE],
            framing[CONF_STX],
            framing[CONF_ETX],
            framing[CONF_ESCAPE_BYTE],
        )
    )

    crc = config[CONF_CRC]
    cg.add(var.set_crc_type(CRC_TYPES[crc[CONF_TYPE]]))
    cg.add(var.set_accept_header_crc("header_inclusive" in crc[CONF_RX_ACCEPT]))
    cg.add(var.set_accept_payload_crc("payload_only" in crc[CONF_RX_ACCEPT]))
    cg.add(var.set_tx_crc_variant(CRC_VARIANTS[crc[CONF_TX_VARIANT]]))

    tx = config[CONF_TX]
    gate = tx[CONF_GATE]
    cg.add(var.set_tx_gate_mode(TX_GATE_MODES[gate[CONF_MODE]]))
    cg.add(var.set_tx_gate_frame_type(gate[CONF_FRAME_TYPE]))
    cg.add(var.set_tx_gate_delay(gate[CONF_DELAY].total_milliseconds))
    cg.add(var.set_tx_idle_gap(gate[CONF_MIN_SILENCE].total_milliseconds))
    cg.add(var.set_tx_fixed_interval(gate[CONF_INTERVAL].total_milliseconds))
    cg.add(var.set_queue_policy(QUEUE_POLICIES[tx[CONF_QUEUE_POLICY]]))
    cg.add(var.set_max_queue_size(tx[CONF_MAX_QUEUE_SIZE]))
    if (idle_cmd := tx.get(CONF_IDLE_COMMAND)) is not None:
        cg.add(var.set_idle_command(idle_cmd))

    cg.add(var.set_key_format(KEY_FORMATS[config[CONF_KEY_FORMAT]]))
    cg.add(var.set_dump_frames(config[CONF_DUMP_FRAMES]))
    cg.add(var.set_sniffer_only(config[CONF_SNIFFER_ONLY]))
    cg.add(var.set_max_frame_length(config[CONF_MAX_FRAME_LENGTH]))
    cg.add(var.set_in_frame_timeout(config[CONF_FRAME_TIMEOUT].total_milliseconds))

    for conf in config.get(CONF_ON_FRAME, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
        cg.add(trigger.set_frame_type(conf[CONF_FRAME_TYPE]))
        cg.add(var.register_listener(trigger))
        await automation.build_automation(
            trigger,
            [
                (
                    cg.std_vector.template(cg.uint8).operator("const").operator("ref"),
                    "payload",
                )
            ],
            conf,
        )
