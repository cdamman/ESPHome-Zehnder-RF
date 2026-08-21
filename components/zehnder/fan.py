import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import fan
from esphome.const import CONF_CHANNEL, CONF_ID, CONF_MODEL, CONF_UPDATE_INTERVAL

from esphome.components.nrf905 import nRF905Component


DEPENDENCIES = ["nrf905"]

zehnder_ns = cg.esphome_ns.namespace("zehnder")
ZehnderRF = zehnder_ns.class_("ZehnderRF", cg.Component, fan.Fan)

CONF_NRF905 = "nrf905"

# Fired when the record of "who last commanded the fan" changes -- another remote
# pressed, a command sent from here -- and again when it goes stale a minute
# later. `on_answer` is the same for the unit's replies. Meant for refreshing a
# diagnostic the moment it changes, the way fan's own `on_state` refreshes the
# derived sensors. Queries are in neither: see the component's isPollFrame().
CONF_ON_COMMAND = "on_command"
CONF_ON_ANSWER = "on_answer"

# Optional: pin the paired identity (read off the three diagnostic entities) so it
# survives a flash erase / fresh install without re-running discovery. network_id,
# main_unit_id and my_device_id must be given together; the type fields default to
# the usual ComfoFan values (main unit = 0x01, this device = 0x03 REMOTE_CONTROL).
#
# The names mirror nRF905-API's fanconfig, so its values can be copied across
# without translating: `my_device_*` is this controller, `main_unit_*` is the fan.
# Beware that nRF905-API's JSON calls the fan the "remote" (as in the remote end of
# the link) -- its `remote_id` is the main unit's, not this device's.
#
# `my_device_id` rather than `device_id` is also a hard requirement: ESPHome reserves
# `device_id` on every entity to attach it to a sub-device, and shadowing it makes
# entity setup fail with "'HexInt' object has no attribute 'id'".
CONF_NETWORK_ID = "network_id"
CONF_MAIN_UNIT_TYPE = "main_unit_type"
CONF_MAIN_UNIT_ID = "main_unit_id"
CONF_MY_DEVICE_TYPE = "my_device_type"
CONF_MY_DEVICE_ID = "my_device_id"

# Self-healing: consecutive poll timeouts before the device auto re-pairs. On by
# default; the wall-clock delay is that many update_interval periods (10 polls at
# 30 s is ~5 min). Set 0 to disable.
CONF_SELF_HEAL_AFTER = "self_heal_after"

# Named modes shown in Home Assistant, and the boost behaviour behind the
# startBoost() / cancelBoost() calls.
CONF_PRESET_MODES = "preset_modes"
CONF_BOOST_SPEED = "boost_speed"
CONF_REVERT_SPEED = "revert_speed"
CONF_BOOST_DURATION = "boost_duration"

CONF_PAIRING_KEYS = (CONF_NETWORK_ID, CONF_MAIN_UNIT_ID, CONF_MY_DEVICE_ID)

# The nRF905 channel each fan family uses (band 1, f = 2 * (422.4 + ch / 10) MHz).
FAN_MODELS = {
    "zehnder": 118,  # 868.400 MHz
    "buva": 117,  # 868.200 MHz
}

# Speed presets this component offers, named so a config reads "3 - High: high"
# instead of a bare number. The protocol has a fourth preset (max, 4); it is left
# out on purpose -- see FAN_SPEED_COUNT in zehnder.h.
FAN_SPEEDS = {
    "low": 1,
    "medium": 2,
    "high": 3,
}

# A mode may also stop the unit (speed 0). That is deliberately *not* offered for
# boost_speed / revert_speed: those always keep the unit running.
PRESET_SPEEDS = {"off": 0, **FAN_SPEEDS}

SPEED_NAMES = ", ".join(f'"{name}"' for name in FAN_SPEEDS)


def speed_value(value):
    """A running speed: a name or 1-3. Never 0 -- those speeds keep the fan on."""
    if isinstance(value, bool):
        # YAML 1.1 reads a bare on/off/yes/no as a boolean, and bool is an int in
        # Python, so `on` would quietly pass as speed 1. Say so instead.
        raise cv.Invalid(f"a bare YAML on/off is not a speed; use {SPEED_NAMES} or 1-3")
    return cv.Any(cv.enum(FAN_SPEEDS, lower=True), cv.int_range(min=1, max=3))(value)


def preset_speed_value(value):
    """The speed a named mode maps to: a name, 0-3, or `off` for 0."""
    if value is False:
        # A bare `off` in YAML is the boolean False; that is unambiguous here, so
        # accept it as speed 0 rather than make everyone quote it.
        return 0
    if value is True:
        raise cv.Invalid(f'"on" is not a speed; use "off", {SPEED_NAMES} or 0-3')
    return cv.Any(cv.enum(PRESET_SPEEDS, lower=True), cv.int_range(min=0, max=3))(value)


# Bounds mirrored from zehnder.h (FAN_TIMER_MIN/MAX_MINUTES).
TIMER_MIN_MINUTES = 1
TIMER_MAX_MINUTES = 240


def _validate_pairing(config):
    present = [k for k in CONF_PAIRING_KEYS if k in config]
    if present and len(present) != len(CONF_PAIRING_KEYS):
        raise cv.Invalid(
            f"To pin the pairing, set all of {', '.join(CONF_PAIRING_KEYS)} together "
            f"(missing: {', '.join(k for k in CONF_PAIRING_KEYS if k not in config)})"
        )
    return config


CONFIG_SCHEMA = cv.All(
    fan.fan_schema(ZehnderRF)
    .extend(
        {
            cv.Required(CONF_NRF905): cv.use_id(nRF905Component),
            cv.Optional(CONF_UPDATE_INTERVAL, default="30s"): cv.update_interval,
            # Which fan family: sets the RF channel. Override with `channel` if
            # your unit sits on another one.
            cv.Optional(CONF_MODEL, default="zehnder"): cv.enum(
                FAN_MODELS, lower=True
            ),
            cv.Optional(CONF_CHANNEL): cv.int_range(min=0, max=127),
            # Named modes, in the order they should appear: "<name>: <speed>".
            # A mode mapped to `off` (speed 0) stops the unit -- that is the only
            # way to stop it, since the entity's on/off drives the boost.
            cv.Optional(CONF_PRESET_MODES): cv.All(
                cv.Schema({cv.string_strict: preset_speed_value}), cv.Length(min=1)
            ),
            # Boost: which speed it runs at, which speed cancelling falls back to,
            # and its default duration (the `number` entity can change it live).
            cv.Optional(CONF_BOOST_SPEED, default="high"): speed_value,
            cv.Optional(CONF_REVERT_SPEED, default="medium"): speed_value,
            cv.Optional(CONF_BOOST_DURATION, default="30min"): cv.All(
                cv.positive_time_period_minutes,
                cv.Range(
                    min=cv.TimePeriod(minutes=TIMER_MIN_MINUTES),
                    max=cv.TimePeriod(minutes=TIMER_MAX_MINUTES),
                ),
            ),
            cv.Optional(CONF_NETWORK_ID): cv.hex_uint32_t,
            cv.Optional(CONF_MAIN_UNIT_TYPE, default=0x01): cv.hex_uint8_t,
            cv.Optional(CONF_MAIN_UNIT_ID): cv.hex_uint8_t,
            cv.Optional(CONF_MY_DEVICE_TYPE, default=0x03): cv.hex_uint8_t,
            cv.Optional(CONF_MY_DEVICE_ID): cv.hex_uint8_t,
            cv.Optional(CONF_SELF_HEAL_AFTER, default=10): cv.uint32_t,
            cv.Optional(CONF_ON_COMMAND): automation.validate_automation(
                single=True
            ),
            cv.Optional(CONF_ON_ANSWER): automation.validate_automation(single=True),
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    _validate_pairing,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await fan.register_fan(var, config)

    nrf905 = await cg.get_variable(config[CONF_NRF905])
    cg.add(var.set_rf(nrf905))

    cg.add(var.set_update_interval(config[CONF_UPDATE_INTERVAL]))
    cg.add(var.set_self_heal_threshold(config[CONF_SELF_HEAL_AFTER]))

    # An explicit channel wins over the one implied by the model.
    cg.add(var.set_channel(config.get(CONF_CHANNEL, config[CONF_MODEL])))

    for name, speed in config.get(CONF_PRESET_MODES, {}).items():
        cg.add(var.add_preset(name, speed))

    cg.add(var.set_boost_speed(config[CONF_BOOST_SPEED]))
    cg.add(var.set_revert_speed(config[CONF_REVERT_SPEED]))
    cg.add(var.set_timer_minutes(config[CONF_BOOST_DURATION].total_minutes))

    if CONF_ON_COMMAND in config:
        await automation.build_automation(
            var.get_command_trigger(), [], config[CONF_ON_COMMAND]
        )

    if CONF_ON_ANSWER in config:
        await automation.build_automation(
            var.get_answer_trigger(), [], config[CONF_ON_ANSWER]
        )

    if all(k in config for k in CONF_PAIRING_KEYS):
        cg.add(
            var.set_paired_config(
                config[CONF_NETWORK_ID],
                config[CONF_MAIN_UNIT_TYPE],
                config[CONF_MAIN_UNIT_ID],
                config[CONF_MY_DEVICE_TYPE],
                config[CONF_MY_DEVICE_ID],
            )
        )
