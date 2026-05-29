from esphome import automation
import esphome.codegen as cg
from esphome.components import uart
import esphome.config_validation as cv
from esphome.const import (
    CONF_DELAY,
    CONF_ID,
    CONF_INTERVAL,
    CONF_MODE,
    CONF_PAYLOAD,
    CONF_TRIGGER_ID,
    CONF_TYPE,
)
from esphome.core import HexInt

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
SendFrameAction = rs485_frame_ns.class_("SendFrameAction", automation.Action)

SensorDecode = rs485_frame_ns.enum("SensorDecode")
CrcVariant = rs485_frame_ns.enum("CrcVariant")
CrcType = rs485_frame_ns.enum("CrcType")
QueuePolicy = rs485_frame_ns.enum("QueuePolicy")
TxGateMode = rs485_frame_ns.enum("TxGateMode")

CONF_RS485_FRAME_ID = "rs485_frame_id"
CONF_COMMAND_ENDIAN = "command_endian"
CONF_COMMAND_FORMAT = "command_format"
CONF_COMMAND_REPEAT = "command_repeat"
CONF_COMMAND_SIZE = "command_size"
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
CONF_MAX_FRAME_LENGTH = "max_frame_length"
CONF_MAX_FRAME_TYPES = "max_frame_types"
CONF_MAX_QUEUE_SIZE = "max_queue_size"
CONF_MAX_UNIQUE_PAYLOADS = "max_unique_payloads"
CONF_MIN_SILENCE = "min_silence"
CONF_PAYLOAD_CAPTURE_BYTES = "payload_capture_bytes"
CONF_PAYLOAD_DUMP_TOP = "payload_dump_top"
CONF_POSTAMBLE = "postamble"
CONF_PREAMBLE = "preamble"
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


# Schema caps for preamble / postamble byte lists. Must agree with the C++ StaticVector
# template arguments MAX_COMMAND_PREAMBLE_LEN / MAX_COMMAND_POSTAMBLE_LEN in rs485_frame.h.
MAX_COMMAND_PREAMBLE_LEN = 8
MAX_COMMAND_POSTAMBLE_LEN = 8


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


def _validate_command_format_bytes(max_len: int):
    return cv.All(cv.ensure_list(validate_byte), cv.Length(max=max_len))


# Schema for command_format: — defines how a uint32 `command:` value is serialised into
# the frame payload. A single data-driven block that can express any protocol variant
# without touching C++ (preamble bytes, command field size/endianness/repeat, postamble
# bytes). command_format: is optional: hubs without it can only transmit via the raw
# `frame_type` + `payload` button form or the rs485_frame.send_frame action — the button
# platform's _final_validate rejects the `command:` shorthand against a hub that has none.
COMMAND_FORMAT_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_PREAMBLE, default=[]): _validate_command_format_bytes(
            MAX_COMMAND_PREAMBLE_LEN
        ),
        cv.Required(CONF_COMMAND_SIZE): cv.one_of(1, 2, 4, int=True),
        cv.Optional(CONF_COMMAND_ENDIAN, default="big"): cv.one_of(
            "big", "little", lower=True
        ),
        cv.Optional(CONF_COMMAND_REPEAT, default=1): cv.int_range(min=1, max=4),
        cv.Optional(CONF_POSTAMBLE, default=[]): _validate_command_format_bytes(
            MAX_COMMAND_POSTAMBLE_LEN
        ),
    }
)


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
        # crc.type is required: there is no universally-correct default across DLE-framed
        # buses, so the user must declare the algorithm their device uses (use "none" to
        # accept any structurally-valid frame, e.g. while discovering an unknown bus).
        cv.Required(CONF_TYPE): cv.one_of(*CRC_TYPES, lower=True),
        cv.Optional(
            CONF_RX_ACCEPT, default=["header_inclusive", "payload_only"]
        ): cv.ensure_list(cv.one_of(*CRC_VARIANTS, lower=True)),
        # tx_variant selects whether the DLE+STX header bytes participate in the transmitted
        # CRC. header_inclusive is the common case; set payload_only for buses that checksum
        # only the unescaped payload. It is a mechanical (not vendor) choice, so it defaults.
        cv.Optional(CONF_TX_VARIANT, default="header_inclusive"): cv.one_of(
            *CRC_VARIANTS, lower=True
        ),
    }
)

TX_GATE_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_MODE, default="frame_trigger"): cv.one_of(
            *TX_GATE_MODES, lower=True
        ),
        # No schema-level default for frame_type: it is device-specific (the bus keep-alive /
        # poll frame the hub transmits after). validate_hub() requires it for frame_trigger
        # mode; idle_gap and fixed_delay modes do not use it.
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
    # Framing-byte distinctness. The framer uses DLE as the escape introducer: in-frame,
    # DLE+STX starts a frame, DLE+ETX ends it, and DLE+escape_byte is a literal DLE. If any
    # of DLE/STX/ETX collide, or escape_byte equals STX or ETX, the byte stream is no longer
    # unambiguously parseable. Reject at config time rather than emitting unframeable data.
    framing = config[CONF_FRAMING]
    dle = framing[CONF_DLE]
    stx = framing[CONF_STX]
    etx = framing[CONF_ETX]
    escape = framing[CONF_ESCAPE_BYTE]
    if len({int(dle), int(stx), int(etx)}) != 3:
        raise cv.Invalid("framing dle, stx and etx must all be distinct byte values")
    if int(escape) in (int(stx), int(etx)):
        raise cv.Invalid(
            "framing escape_byte must differ from stx and etx; otherwise an escaped DLE "
            "is indistinguishable from a frame start/end terminator"
        )

    gate = config[CONF_TX][CONF_GATE]
    sniffer_only = config[CONF_SNIFFER_ONLY]
    gate_mode = gate[CONF_MODE]

    # In frame_trigger gate mode (non-sniffer), the gate frame type must be a non-empty byte
    # list — otherwise the gate would never fire and queued commands would accumulate
    # forever. It has no default, so require it explicitly.
    if (
        gate_mode == "frame_trigger"
        and not gate.get(CONF_FRAME_TYPE)
        and not sniffer_only
    ):
        raise cv.Invalid(
            "tx.gate.frame_type is required (a non-empty byte list) when tx.gate.mode is "
            "frame_trigger; the gate would never fire and queued commands would not transmit"
        )

    if (
        config[CONF_TX][CONF_QUEUE_POLICY] == "replace_latest"
        and config[CONF_TX][CONF_MAX_QUEUE_SIZE] != 1
    ):
        raise cv.Invalid("replace_latest requires max_queue_size: 1")

    # tx.idle_command is a uint32 that the hub serialises via command_format on every gate
    # with an empty queue. Without a command_format there is no defined encoding, so reject
    # the combination rather than silently emitting an undocumented default 4-byte big-endian
    # encoding (mirrors the button platform's `command:` rule).
    if CONF_IDLE_COMMAND in config[CONF_TX] and CONF_COMMAND_FORMAT not in config:
        raise cv.Invalid(
            "tx.idle_command requires a command_format: on the hub so the value has a "
            "defined on-wire encoding"
        )

    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(RS485FrameHub),
            cv.Optional(CONF_FRAMING, default={}): FRAMING_SCHEMA,
            # crc: is required because crc.type has no sensible cross-bus default; the user
            # must declare their device's checksum (or `type: none`).
            cv.Required(CONF_CRC): CRC_SCHEMA,
            cv.Optional(CONF_TX, default={}): TX_SCHEMA,
            cv.Optional(CONF_COMMAND_FORMAT): COMMAND_FORMAT_SCHEMA,
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
    # gate.frame_type is absent for idle_gap / fixed_delay modes (validate_hub only requires
    # it for frame_trigger); default to an empty list so the hub's gate matcher never fires.
    gate_frame_type = gate.get(CONF_FRAME_TYPE, [])
    cg.add(var.set_tx_gate_mode(TX_GATE_MODES[gate[CONF_MODE]]))
    cg.add(var.set_tx_gate_frame_type(gate_frame_type))
    cg.add(var.set_tx_gate_delay(gate[CONF_DELAY].total_milliseconds))
    cg.add(var.set_tx_idle_gap(gate[CONF_MIN_SILENCE].total_milliseconds))
    cg.add(var.set_tx_fixed_interval(gate[CONF_INTERVAL].total_milliseconds))
    cg.add(var.set_queue_policy(QUEUE_POLICIES[tx[CONF_QUEUE_POLICY]]))
    cg.add(var.set_max_queue_size(tx[CONF_MAX_QUEUE_SIZE]))
    if (idle_cmd := tx.get(CONF_IDLE_COMMAND)) is not None:
        cg.add(var.set_idle_command(idle_cmd))

    # command_format is only present when the user set it explicitly. Hubs without it can
    # still transmit via the raw button form or rs485_frame.send_frame — the button
    # platform's _final_validate rejects the `command:` shorthand against such a hub.
    if (cf := config.get(CONF_COMMAND_FORMAT)) is not None:
        cg.add(
            var.set_command_format(
                cf[CONF_PREAMBLE],
                cf[CONF_COMMAND_SIZE],
                cf[CONF_COMMAND_ENDIAN] == "big",
                cf[CONF_COMMAND_REPEAT],
                cf[CONF_POSTAMBLE],
            )
        )
    cg.add(var.set_dump_frames(config[CONF_DUMP_FRAMES]))
    cg.add(var.set_sniffer_only(config[CONF_SNIFFER_ONLY]))
    cg.add(var.set_max_frame_length(config[CONF_MAX_FRAME_LENGTH]))
    cg.add(var.set_in_frame_timeout(config[CONF_FRAME_TIMEOUT].total_milliseconds))

    if (stats := config.get(CONF_SNIFFER_STATS)) is not None:
        # cg.add_define gates the SnifferStats field, includes, and hot-path call out of
        # builds that don't use sniffer_stats — production firmware pays no cost at all.
        cg.add_define("USE_RS485_FRAME_SNIFFER_STATS")
        # reference_frame_type defaults to the gate frame_type (may be empty for non
        # frame_trigger modes); the sniffer treats an empty reference as "no cadence ref".
        ref = stats.get(CONF_REFERENCE_FRAME_TYPE, gate_frame_type)
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


# rs485_frame.send_frame: queue an arbitrary frame for transmission. Both frame_type
# and payload are templatable lists of bytes so callers can compute them at action time
# (e.g. from a trigger's payload, a global, or a sensor reading). The hub takes care of
# DLE-framing, byte-stuffing, and CRC according to the hub's crc.type and crc.tx_variant.
# This is the most flexible transmit path: it needs no command_format on the hub and can
# emit anything (probe frames, vendor-specific commands, device-discovery sequences, or a
# value computed at runtime) that the `command:` button shorthand cannot express.
SEND_FRAME_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(RS485FrameHub),
        cv.Required(CONF_FRAME_TYPE): cv.templatable(validate_frame_type),
        cv.Required(CONF_PAYLOAD): cv.templatable(cv.ensure_list(validate_byte)),
    }
)


@automation.register_action(
    "rs485_frame.send_frame", SendFrameAction, SEND_FRAME_SCHEMA, synchronous=True
)
async def send_frame_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    frame_type = await cg.templatable(
        config[CONF_FRAME_TYPE], args, cg.std_vector.template(cg.uint8)
    )
    cg.add(var.set_frame_type(frame_type))
    payload = await cg.templatable(
        config[CONF_PAYLOAD], args, cg.std_vector.template(cg.uint8)
    )
    cg.add(var.set_payload(payload))
    return var
