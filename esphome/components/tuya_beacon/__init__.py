import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32_ble_tracker
from esphome.const import CONF_ID, CONF_MAC_ADDRESS

CODEOWNERS = ["@arafatamim"]
DEPENDENCIES = ["esp32_ble_tracker"]
AUTO_LOAD = ["esp32_ble"]
MULTI_CONF = True

tuya_beacon_ns = cg.esphome_ns.namespace("tuya_beacon")
TuyaBeacon = tuya_beacon_ns.class_(
    "TuyaBeacon",
    cg.Component,
    esp32_ble_tracker.ESPBTDeviceListener,
)

CONF_LOCAL_KEY = "local_key"
CONF_CONTROLLER_NAME = "controller_name"
CONF_INITIAL_SEQ = "initial_seq"
CONF_TUYA_BEACON_ID = "tuya_beacon_id"

# Exported so platforms (light, switch…) can reference a TuyaBeacon instance.
TUYA_BEACON_SCHEMA = cv.Schema(
    {cv.Required(CONF_TUYA_BEACON_ID): cv.use_id(TuyaBeacon)}
)


def _validate_local_key(value):
    value = cv.string_strict(value)
    if len(value) != 16:
        raise cv.Invalid("local_key must be exactly 16 ASCII characters")
    return value


def _validate_controller_name(value):
    value = cv.string_strict(value)
    if not 1 <= len(value) <= 8:
        raise cv.Invalid(
            "controller_name must be 1–8 characters "
            "(31-byte legacy-advert budget: 3+Flags + (2+N)+Name + 18+UUID = ≤31)"
        )
    return value


CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(TuyaBeacon),
            cv.Required(CONF_LOCAL_KEY): _validate_local_key,
            cv.Required(CONF_CONTROLLER_NAME): _validate_controller_name,
            cv.Required(CONF_MAC_ADDRESS): cv.mac_address,
            cv.Optional(CONF_INITIAL_SEQ, default=1): cv.uint32_t,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(esp32_ble_tracker.ESP_BLE_DEVICE_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await esp32_ble_tracker.register_ble_device(var, config)

    cg.add(var.set_local_key(config[CONF_LOCAL_KEY]))
    cg.add(var.set_controller_name(config[CONF_CONTROLLER_NAME]))
    mac = config[CONF_MAC_ADDRESS]
    cg.add(var.set_mac_address(mac.as_hex))
    cg.add(var.set_initial_seq(config[CONF_INITIAL_SEQ]))
