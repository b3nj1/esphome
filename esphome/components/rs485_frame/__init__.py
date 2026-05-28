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

rs485_frame_ns = cg.esphome_ns.namespace("rs485_frame")
RS485FrameHub = rs485_frame_ns.class_("RS485FrameHub", cg.Component, uart.UARTDevice)
RS485FrameTrigger = rs485_frame_ns.class_(
    "RS485FrameTrigger",
    automation.Trigger.template(
        cg.std_vector.template(cg.uint8).operator("const").operator("ref")
    ),
)

SensorDecode = rs485_frame_ns.enum("SensorDecode")
KeyFormat = rs485_frame_ns.enum("KeyFormat")
CrcVariant = rs485_frame_ns.enum("CrcVariant")
CrcType = rs485_frame_ns.enum("CrcType")
QueuePolicy = rs485_frame_ns.enum("QueuePolicy")
TxGateMode = rs485_frame_ns.enum("TxGateMode")

CONF_RS485_FRAME_ID = "rs485_frame_id"
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
CONF_MAX_FRAME_TYPES = "max_frame_types"
CONF_MAX_QUEUE_SIZE = "max_queue_size"
CONF_MAX_UNIQUE_PAYLOADS = "max_unique_payloads"
CONF_MIN_SILENCE = "min_silence"
CONF_PAYLOAD_CAPTURE_BYTES = "payload_capture_bytes"
CONF_PAYLOAD_DUMP_TOP = "payload_dump_top"
CONF_PROFILE = "profile"
CONF_QUEUE_POLICY = "queue_policy"
CONF_REFERENCE_FRAME_TYPE = "reference_frame_type"
CONF_RX_ACCEPT = "rx_accept"
CONF_SNIFFER_ONLY = "sniffer_only"
CONF_SNIFFER_STATS = "sniffer_stats"
CONF_STX = "stx"
CONF_ON_FRAME = "on_frame"
CONF_IDLE_COMMAND = "idle_command"
CONF_TX = "tx"
CONF_TX_VARIANT = "tx_variant"

PROFILE_HAYWARD_WIRELESS = "hayward_aqualogic_wireless"
PROFILE_HAYWARD_WIRED_REMOTE = "hayward_aqualogic_wired_remote"
PROFILE_HAYWARD_WIRED_LOCAL = "hayward_aqualogic_wired_local"
PROFILE_JANDY_RS = "jandy_aqualink_rs"
PROFILE_GENERIC = "generic_rs485_frame"

KEY_FORMATS = {
    "wireless_12byte": KeyFormat.KEY_FORMAT_WIRELESS_12BYTE,
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

# Diagnostic-only sensor decode types. User payload decoding is handled by on_frame:.
SENSOR_DECODES = {
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


# Schema cap matches MAX_FRAME_TYPE_LEN in rs485_frame.h (StaticVector<uint8_t, 8>);
# the StaticVector::assign() truncates silently past N=8, so the schema must enforce the cap.
def validate_frame_type(value):
    return cv.All(cv.ensure_list(validate_byte), cv.Length(max=8))(value)


# Schema cap for the number of frame-type alternates per on_frame: entry. Must agree with
# MAX_FRAME_TYPE_ALTS in rs485_frame.h — the C++ StaticVector silently drops push_back past
# its cap, so the schema is the only place that surfaces "too many alternates" as an error.
MAX_FRAME_TYPE_ALTS = 4


def validate_frame_type_or_list(value):
    # on_frame: frame_type accepts either a single prefix ([0x01, 0x03]) or a list of
    # prefixes ([[0x01, 0x03], [0x01, 0x09]]) so one lambda can decode multiple related
    # frame types. Disambiguate by inspecting the first element: a list there means the
    # list-of-prefixes form, anything else (or empty) is the single-prefix form.
    # Normalize to a list-of-prefixes for code generation; the empty single-prefix form
    # (`frame_type: []` = match-all) is preserved as an empty prefix list so to_code emits
    # zero add_frame_type calls and the runtime falls back to its match-all branch.
    if value is None:
        return []
    if not isinstance(value, list):
        value = [value]
    if value and isinstance(value[0], list):
        return cv.All(
            cv.ensure_list(validate_frame_type),
            cv.Length(min=1, max=MAX_FRAME_TYPE_ALTS),
        )(value)
    single = validate_frame_type(value)
    if not single:
        return []
    return [single]


# Internal dict key used by _profile_defaults to carry a profile-specific TX gate frame
# default into validate_hub. Not a YAML option (the leading underscore signals "private"),
# and intentionally not named CONF_* because it never appears in a user-facing schema.
_PROFILE_GATE_FRAME_TYPE = "_gate_frame_type"


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
            # AllButton address 0x08 + CMD_PROBE 0x00. Jandy AllButton devices live at
            # 0x08..0x0B; a user with a different address must override this explicitly.
            _PROFILE_GATE_FRAME_TYPE: [0x08, 0x00],
        }
    return {
        CONF_KEY_FORMAT: "wireless_12byte",
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
        # tx_variant has no schema-level default because the correct default is
        # profile-dependent (Hayward wired uses "payload_only"; all others use
        # "header_inclusive"). validate_hub() always injects it from _profile_defaults()
        # if the user does not set it explicitly.
        cv.Optional(CONF_TX_VARIANT): cv.one_of(*CRC_VARIANTS, lower=True),
    }
)

TX_GATE_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_MODE, default="frame_trigger"): cv.one_of(
            *TX_GATE_MODES, lower=True
        ),
        # No schema-level default for frame_type — _profile_defaults() supplies a
        # profile-appropriate value (Hayward keep-alive [0x01,0x01], Jandy probe
        # [0x08,0x00], etc.) in validate_hub. Required for frame_trigger mode.
        cv.Optional(CONF_FRAME_TYPE): validate_frame_type,
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

# Upper bound for max_frame_length: protects against pathological YAML that would reserve
# ~4 KB scratch buffers per hub. ESPHome's largest framed protocols (Jandy iAqualinkTouch)
# stay well under 512 bytes; 1024 leaves ample headroom.
MAX_FRAME_LENGTH_UPPER = 1024
# Upper bound for max_queue_size with FIFO. Replace_latest is independently constrained to 1.
MAX_QUEUE_SIZE_UPPER = 32
# Upper bound for sniffer_stats max_frame_types. Must agree with SNIFFER_MAX_FRAME_TYPES_UPPER
# in sniffer_stats.h — the C++ side caps the FixedVector capacity at that constant, so a
# larger schema value would silently truncate.
SNIFFER_MAX_FRAME_TYPES_UPPER = 64

# Upper bound for sniffer_stats max_unique_payloads. Per-entry heap allocation grows linearly
# in this value; 64 is a comfortable ceiling for "lots of distinct display screens" without
# making it easy to OOM an ESP8266 via a typo.
SNIFFER_MAX_UNIQUE_PAYLOADS_UPPER = 64

# Upper bound for sniffer_stats payload_capture_bytes. The sniffer truncates each captured
# payload to this length; 256 covers the widest legal frame in any supported protocol while
# keeping the table memory bounded. A frame longer than this is still uniquely identified
# by its first 256 bytes — collisions on the first 256 bytes are astronomically unlikely.
SNIFFER_PAYLOAD_CAPTURE_BYTES_UPPER = 256

TX_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_GATE, default={}): TX_GATE_SCHEMA,
        cv.Optional(CONF_QUEUE_POLICY, default="replace_latest"): cv.one_of(
            *QUEUE_POLICIES, lower=True
        ),
        # cv.positive_int allows 0, which would cause modulo-by-zero in the ring buffer.
        cv.Optional(CONF_MAX_QUEUE_SIZE, default=1): cv.int_range(
            min=1, max=MAX_QUEUE_SIZE_UPPER
        ),
        cv.Optional(CONF_IDLE_COMMAND): validate_u32,
    }
)

# Schema for sniffer_stats: — an optional diagnostic that buckets RX frames by frame_type
# and logs cadence + unique-payload histograms on a periodic interval. Compiled out unless
# the YAML block is present (see USE_RS485_FRAME_SNIFFER_STATS in sniffer_stats.h).
SNIFFER_STATS_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_INTERVAL, default="30s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_MAX_FRAME_TYPES, default=32): cv.int_range(
            min=1, max=SNIFFER_MAX_FRAME_TYPES_UPPER
        ),
        # Per-frame-type unique-payload capacity. Bumped from the original 8 to 16 so
        # protocols with many display screens or per-button responses don't fill the
        # bucket immediately and start counting everything into +overflow.
        cv.Optional(CONF_MAX_UNIQUE_PAYLOADS, default=16): cv.int_range(
            min=1, max=SNIFFER_MAX_UNIQUE_PAYLOADS_UPPER
        ),
        # How many payload bytes are captured per unique sample. 32 covers most decode
        # use cases including Hayward display frames; bump for AquaLogic / iAqualinkTouch
        # variants that ship longer frames. Capped at SNIFFER_PAYLOAD_CAPTURE_BYTES_UPPER.
        cv.Optional(CONF_PAYLOAD_CAPTURE_BYTES, default=32): cv.int_range(
            min=1, max=SNIFFER_PAYLOAD_CAPTURE_BYTES_UPPER
        ),
        # payload_dump_top=0 disables the hex/ASCII dump that follows the table; only the
        # summary row per frame_type is logged in that case. Capped at MAX_QUEUE_SIZE_UPPER
        # (32) just to keep log volume bounded — there is no inherent upper limit.
        cv.Optional(CONF_PAYLOAD_DUMP_TOP, default=0): cv.int_range(min=0, max=32),
        # reference_frame_type defaults to the active tx.gate.frame_type (typically the
        # bus keep-alive) when omitted; supplied here when you want d-ref measured against
        # something other than the gate.
        cv.Optional(CONF_REFERENCE_FRAME_TYPE): validate_frame_type,
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

    # gate.frame_type default is profile-dependent. _profile_defaults supplies a default
    # for Jandy (the probe frame); other profiles fall back to the Hayward keepalive.
    gate = config[CONF_TX][CONF_GATE]
    if CONF_FRAME_TYPE not in gate:
        gate[CONF_FRAME_TYPE] = defaults.get(_PROFILE_GATE_FRAME_TYPE, [0x01, 0x01])

    sniffer_only = config[CONF_SNIFFER_ONLY]
    gate_mode = gate[CONF_MODE]

    # When in frame_trigger gate mode (non-sniffer), the gate frame type must not be
    # empty — otherwise the gate would never fire and queued commands would accumulate
    # forever. Setup-time runtime warning is too late; reject at config time.
    if gate_mode == "frame_trigger" and not gate[CONF_FRAME_TYPE] and not sniffer_only:
        raise cv.Invalid(
            "tx.gate.frame_type must be a non-empty byte list when tx.gate.mode is "
            "frame_trigger; the gate would never fire and queued commands would not transmit"
        )

    if (
        config[CONF_TX][CONF_QUEUE_POLICY] == "replace_latest"
        and config[CONF_TX][CONF_MAX_QUEUE_SIZE] != 1
    ):
        raise cv.Invalid("replace_latest requires max_queue_size: 1")

    # Jandy AllButton emulation only works if the AllButton device ACKs every probe.
    # Without idle_command the device would only respond when a real button is queued,
    # and the master would mark the AllButton offline between presses.
    if (
        profile == PROFILE_JANDY_RS
        and not sniffer_only
        and CONF_IDLE_COMMAND not in config[CONF_TX]
    ):
        raise cv.Invalid(
            "jandy_aqualink_rs profile requires tx.idle_command (typically 0x00) unless "
            "sniffer_only is true; the AllButton emulator must ACK every probe or the "
            "master will mark it offline"
        )

    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(RS485FrameHub),
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
            # Minimum legal RX frame = DLE STX FT0 FT1 DLE ETX = 6 bytes (no CRC).
            cv.Optional(CONF_MAX_FRAME_LENGTH, default=128): cv.int_range(
                min=6, max=MAX_FRAME_LENGTH_UPPER
            ),
            cv.Optional(
                CONF_FRAME_TIMEOUT, default="50ms"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_ON_FRAME): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(RS485FrameTrigger),
                    cv.Required(CONF_FRAME_TYPE): validate_frame_type_or_list,
                }
            ),
            cv.Optional(CONF_SNIFFER_STATS): SNIFFER_STATS_SCHEMA,
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
            # UART schema provides validated defaults for all four keys (baud_rate is
            # required; the rest default in UART_DEVICE_SCHEMA).
            if uart_config.get(key) != expected:
                raise cv.Invalid(
                    f"RS485 Frame jandy_aqualink_rs profile requires uart {key}: {expected}; "
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
        if uart_config.get(key) != expected:
            raise cv.Invalid(
                f"RS485 Frame Hayward profiles require uart {key}: {expected}; "
                f"use profile: {PROFILE_GENERIC} for other serial settings"
            )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


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
        # validate_frame_type_or_list normalizes the YAML to a list-of-prefixes; empty
        # list = match-all, in which case we emit zero add_frame_type calls and the
        # trigger's matches() falls through to its empty-list branch.
        for prefix in conf[CONF_FRAME_TYPE]:
            cg.add(trigger.add_frame_type(prefix))
        cg.add(var.register_trigger(trigger))
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

    if (stats := config.get(CONF_SNIFFER_STATS)) is not None:
        # cg.add_define gates the SnifferStats field, includes, and hot-path call out of
        # builds that don't use sniffer_stats — production firmware pays no cost at all.
        cg.add_define("USE_RS485_FRAME_SNIFFER_STATS")
        ref = stats.get(CONF_REFERENCE_FRAME_TYPE, gate[CONF_FRAME_TYPE])
        cg.add(
            var.enable_sniffer_stats(
                stats[CONF_MAX_FRAME_TYPES],
                stats[CONF_INTERVAL].total_milliseconds,
                stats[CONF_PAYLOAD_DUMP_TOP],
                stats[CONF_MAX_UNIQUE_PAYLOADS],
                stats[CONF_PAYLOAD_CAPTURE_BYTES],
                ref,
            )
        )
