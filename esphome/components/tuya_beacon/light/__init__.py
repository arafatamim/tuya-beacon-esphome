import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import light
from esphome.const import CONF_OUTPUT_ID

from .. import (
    tuya_beacon_ns,
    TuyaBeacon,
    TUYA_BEACON_SCHEMA,
    CONF_TUYA_BEACON_ID,
)

CODEOWNERS = ["@arafatamim"]
DEPENDENCIES = ["tuya_beacon"]

TuyaBeaconLight = tuya_beacon_ns.class_(
    "TuyaBeaconLight",
    light.LightOutput,
    cg.Component,
)

CONF_COLD_WHITE_COLOR_TEMPERATURE = "cold_white_color_temperature"
CONF_WARM_WHITE_COLOR_TEMPERATURE = "warm_white_color_temperature"
CONF_RESYNC_INTERVAL = "resync_interval"

CONFIG_SCHEMA = (
    light.RGB_LIGHT_SCHEMA.extend(
        {
            cv.GenerateID(CONF_OUTPUT_ID): cv.declare_id(TuyaBeaconLight),
            cv.Optional(
                CONF_COLD_WHITE_COLOR_TEMPERATURE, default="153 mireds"
            ): cv.color_temperature,
            cv.Optional(
                CONF_WARM_WHITE_COLOR_TEMPERATURE, default="500 mireds"
            ): cv.color_temperature,
            # The bulb never reports its real state, so we periodically re-impose
            # HA's state to correct external drift. "0s" disables it.
            cv.Optional(
                CONF_RESYNC_INTERVAL, default="10min"
            ): cv.positive_time_period_milliseconds,
        }
    )
    .extend(TUYA_BEACON_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_OUTPUT_ID])
    await cg.register_component(var, config)
    await light.register_light(var, config)

    beacon = await cg.get_variable(config[CONF_TUYA_BEACON_ID])
    cg.add(var.set_beacon(beacon))
    cg.add(var.set_cold_white_temperature(config[CONF_COLD_WHITE_COLOR_TEMPERATURE]))
    cg.add(var.set_warm_white_temperature(config[CONF_WARM_WHITE_COLOR_TEMPERATURE]))
    cg.add(var.set_resync_interval(config[CONF_RESYNC_INTERVAL]))
