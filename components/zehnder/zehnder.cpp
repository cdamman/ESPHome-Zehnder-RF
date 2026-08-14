#include <cmath>
#include <cstring>

#include "zehnder.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"

namespace esphome {
namespace zehnder {

#define MAX_TRANSMIT_TIME 2000

static const char *const TAG = "zehnder";

typedef struct __attribute__((packed)) {
  uint32_t networkId;
} RfPayloadNetworkJoinOpen;

typedef struct __attribute__((packed)) {
  uint32_t networkId;
} RfPayloadNetworkJoinRequest;

typedef struct __attribute__((packed)) {
  uint32_t networkId;
} RfPayloadNetworkJoinAck;

typedef struct __attribute__((packed)) {
  uint8_t speed;    // 0x07 current speed preset
  uint8_t voltage;  // 0x08 output as a percentage (30 = 30% = 3.0 V)
  uint8_t flags;    // 0x09 status bits; bit 0 = a timer is running
} RfPayloadFanSettings;

typedef struct __attribute__((packed)) {
  uint8_t speed;
} RfPayloadFanSetSpeed;

typedef struct __attribute__((packed)) {
  uint8_t speed;
  uint8_t timer;
} RfPayloadFanSetTimer;

typedef struct __attribute__((packed)) {
  uint8_t rx_type;          // 0x00 RX Type
  uint8_t rx_id;            // 0x01 RX ID
  uint8_t tx_type;          // 0x02 TX Type
  uint8_t tx_id;            // 0x03 TX ID
  uint8_t ttl;              // 0x04 Time-To-Live
  uint8_t command;          // 0x05 Frame type
  uint8_t parameter_count;  // 0x06 Number of parameters

  union {
    uint8_t parameters[9];                           // 0x07 - 0x0F Depends on command
    RfPayloadFanSetSpeed setSpeed;                   // Command 0x02
    RfPayloadFanSetTimer setTimer;                   // Command 0x03
    RfPayloadNetworkJoinRequest networkJoinRequest;  // Command 0x04
    RfPayloadNetworkJoinOpen networkJoinOpen;        // Command 0x06
    RfPayloadFanSettings fanSettings;                // Command 0x07
    RfPayloadNetworkJoinAck networkJoinAck;          // Command 0x0C
  } payload;
} RfFrame;

ZehnderRF::ZehnderRF(void) {}

fan::FanTraits ZehnderRF::get_traits() {
  // No speed support: a unit with three fixed speeds has nothing to slide, so Home
  // Assistant gets the on/off (the boost) and the named modes, and no percentage.
  // speed_count_ still bounds what setSpeed() will send.
  fan::FanTraits traits(false, false, false, this->speed_count_);
  // Hand the named modes registered from YAML to the traits object, so the API /
  // Home Assistant offers them and FanCall can validate an incoming preset.
  this->wire_preset_modes_(traits);
  return traits;
}

void ZehnderRF::add_preset(const char *name, const uint8_t speed) {
  this->presets_.push_back(Preset{name, speed});
}

const char *ZehnderRF::presetForSpeed_(const uint8_t speed) const {
  for (const auto &preset : this->presets_) {
    if (preset.speed == speed) {
      return preset.name;
    }
  }
  return nullptr;
}

int ZehnderRF::speedForPreset_(const char *const name) const {
  if (name == nullptr) {
    return -1;
  }
  for (const auto &preset : this->presets_) {
    if (strcmp(preset.name, name) == 0) {
      return (int) preset.speed;
    }
  }
  return -1;  // unknown mode -- speed 0 is a valid mode, so it cannot mean this
}

void ZehnderRF::publishFanState_(const uint8_t speed) {
  this->speed = speed;
  // The entity's on/off *is* the boost, so it reads on only while the fan runs
  // at the boost speed. Stopping the unit is a mode, not the off switch.
  this->state = (speed == this->boost_speed_);
  // Keep the reported mode in step with the speed, including for changes made
  // from a physical remote. A speed with no matching entry clears the mode.
  this->set_preset_mode_(this->presetForSpeed_(speed));
  this->publish_state();
}

void ZehnderRF::control(const fan::FanCall &call) {
  // A plain on/off command drives the boost: on -> boost speed with the timer,
  // off -> back to the fall-back speed. Turning the unit off entirely is a mode
  // of its own (speed 0), never the off command -- a ventilation unit that stops
  // because someone tapped a toggle is not what anyone wants.
  if (call.get_state().has_value() && !call.has_preset_mode() && !call.get_speed().has_value()) {
    ESP_LOGD(TAG, "Control has state: %u -> boost %s", *call.get_state(), *call.get_state() ? "on" : "off");
    if (*call.get_state()) {
      this->startBoost(0);
    } else {
      this->cancelBoost();
    }
    return;
  }

  uint8_t speed = (uint8_t) this->speed;

  if (call.get_speed().has_value()) {
    speed = (uint8_t) *call.get_speed();
    ESP_LOGD(TAG, "Control has speed: %u", speed);
  }
  // A named mode selects the speed it is mapped to. FanCall has already checked
  // the name against the traits.
  if (call.has_preset_mode()) {
    const int presetSpeed = this->speedForPreset_(call.get_preset_mode());
    if (presetSpeed >= 0) {
      speed = (uint8_t) presetSpeed;
      ESP_LOGD(TAG, "Control has mode: %s -> speed %u", call.get_preset_mode(), speed);
    }
  }

  switch (this->state_) {
    case StateIdle:
      // Set speed, without a timer: picking a mode is not a boost.
      this->setSpeed(speed, 0);

      this->lastFanQuery_ = millis();  // Update time
      break;

    default:
      break;
  }

  // On this unit a speed *is* a mode, so report the mode that goes with the
  // speed even when the request came in as a plain speed.
  this->publishFanState_(speed);
}

void ZehnderRF::setup() {
  ESP_LOGCONFIG(TAG, "ZEHNDER '%s':", this->get_name().c_str());

  // Clear config
  memset(&this->config_, 0, sizeof(Config));

  uint32_t hash = fnv1_hash("zehnderrf");
  this->pref_ = global_preferences->make_preference<Config>(hash, true);
  if (this->pref_.load(&this->config_)) {
    ESP_LOGD(TAG, "Config load ok");
  }

  // YAML-pinned identity overrides the flash-loaded config, so the pairing
  // survives a flash erase / fresh install with no dependency on discovery.
  this->applyYamlPin_();

  // Set nRF905 config
  nrf905::Config rfConfig;
  rfConfig = this->rf_->getConfig();

  rfConfig.band = true;
  rfConfig.channel = this->channel_;  // 118 = Zehnder (868.400 MHz), 117 = BUVA (868.200 MHz)

  // // CRC 16
  rfConfig.crc_enable = true;
  rfConfig.crc_bits = 16;

  // // TX power 10
  rfConfig.tx_power = 10;

  // // RX power normal
  rfConfig.rx_power = nrf905::PowerNormal;

  rfConfig.rx_address = 0x89816EA9;  // ZEHNDER_NETWORK_LINK_ID;
  rfConfig.rx_address_width = 4;
  rfConfig.rx_payload_width = 16;

  rfConfig.tx_address_width = 4;
  rfConfig.tx_payload_width = 16;

  rfConfig.xtal_frequency = 16000000;  // defaults for now
  rfConfig.clkOutFrequency = nrf905::ClkOut500000;
  rfConfig.clkOutEnable = false;

  // Write config back
  this->rf_->updateConfig(&rfConfig);
  this->rf_->writeTxAddress(0x89816EA9);

  this->speed_count_ = FAN_SPEED_COUNT;

  // Publish the named modes registered from YAML. The names are string literals
  // from codegen, so handing out the pointers is safe.
  if (!this->presets_.empty()) {
    std::vector<const char *> names;
    names.reserve(this->presets_.size());
    for (const auto &preset : this->presets_) {
      names.push_back(preset.name);
    }
    this->set_supported_preset_modes(names);
  }

  this->rf_->setOnTxReady([this](void) {
    ESP_LOGD(TAG, "Tx Ready");
    if (this->rfState_ == RfStateTxBusy) {
      if (this->retries_ >= 0) {
        this->msgSendTime_ = millis();
        this->rfState_ = RfStateRxWait;
      } else {
        this->rfState_ = RfStateIdle;
      }
    }
  });

  this->rf_->setOnRxComplete([this](const uint8_t *const pData, const uint8_t dataLength) {
    ESP_LOGV(TAG, "Received frame");
    this->rfHandleReceived(pData, dataLength);
  });
}

void ZehnderRF::dump_config(void) {
  ESP_LOGCONFIG(TAG, "Zehnder Fan config:");
  ESP_LOGCONFIG(TAG, "  Polling interval   %u", this->interval_);
  ESP_LOGCONFIG(TAG, "  RF channel         %u (%.3f MHz)", this->channel_,
                2.0f * (422.4f + (this->channel_ / 10.0f)));
  ESP_LOGCONFIG(TAG, "  Boost speed        %u, falls back to %u when the pre-boost speed is unknown",
                this->boost_speed_, this->revert_speed_);
  ESP_LOGCONFIG(TAG, "  Boost duration     %u minutes", this->timer_minutes_);
  for (const auto &preset : this->presets_) {
    ESP_LOGCONFIG(TAG, "  Mode               '%s' -> speed %u", preset.name, preset.speed);
  }
  ESP_LOGCONFIG(TAG, "  Fan networkId      0x%08X", this->config_.fan_networkId);
  ESP_LOGCONFIG(TAG, "  Fan my device type 0x%02X", this->config_.fan_my_device_type);
  ESP_LOGCONFIG(TAG, "  Fan my device id   0x%02X", this->config_.fan_my_device_id);
  ESP_LOGCONFIG(TAG, "  Fan main_unit type 0x%02X", this->config_.fan_main_unit_type);
  ESP_LOGCONFIG(TAG, "  Fan main unit id   0x%02X", this->config_.fan_main_unit_id);
}

const char *ZehnderRF::typeToString(uint8_t type) {
  switch (type) {
    case FAN_TYPE_BROADCAST:
      return "Broadcast";
    case FAN_TYPE_MAIN_UNIT:
      return "Main";
    case FAN_TYPE_REMOTE_CONTROL:
      return "Remote";
    case FAN_TYPE_CO2_SENSOR:
      return "CO2";
    default:
      return "?";
  }
}

const char *ZehnderRF::commandToString(uint8_t command) {
  switch (command) {
    case FAN_FRAME_SETVOLTAGE:
      return "SETVOLTAGE";
    case FAN_FRAME_SETSPEED:
      return "SETSPEED";
    case FAN_FRAME_SETTIMER:
      return "SETTIMER";
    case FAN_NETWORK_JOIN_REQUEST:
      return "JOIN_REQUEST";
    case FAN_FRAME_SETSPEED_REPLY:
      return "SETSPEED_REPLY";
    case FAN_NETWORK_JOIN_OPEN:
      return "JOIN_OPEN";
    case FAN_TYPE_FAN_SETTINGS:
      return "FAN_SETTINGS";
    case FAN_FRAME_0B:
      return "FRAME_0B";
    case FAN_NETWORK_JOIN_ACK:
      return "JOIN_ACK";
    case FAN_TYPE_QUERY_NETWORK:
      return "QUERY_NETWORK";
    case FAN_TYPE_QUERY_DEVICE:
      return "QUERY_DEVICE";
    case FAN_FRAME_SETVOLTAGE_REPLY:
      return "SETVOLTAGE_REPLY";
    default:
      return "?";
  }
}

void ZehnderRF::logReceivedFrame(const uint8_t *const pData, const uint8_t dataLength) {
  if (dataLength < FAN_FRAMESIZE) {
    ESP_LOGW(TAG, "RX FRAME too short: %u bytes", dataLength);
    return;
  }

  const RfFrame *const pFrame = (const RfFrame *) pData;

  const bool forUs = (pFrame->rx_type == this->config_.fan_my_device_type) &&
                     (pFrame->rx_id == this->config_.fan_my_device_id);

  ESP_LOGD(TAG,
           "RX FRAME  to[type=0x%02X(%s) id=0x%02X]  from[type=0x%02X(%s) id=0x%02X]  "
           "cmd=0x%02X(%s)  ttl=%u  params=%u  "
           "payload=%02X %02X %02X %02X %02X %02X %02X %02X %02X  [%s]",
           pFrame->rx_type, typeToString(pFrame->rx_type), pFrame->rx_id, pFrame->tx_type,
           typeToString(pFrame->tx_type), pFrame->tx_id, pFrame->command, commandToString(pFrame->command),
           pFrame->ttl, pFrame->parameter_count, pFrame->payload.parameters[0], pFrame->payload.parameters[1],
           pFrame->payload.parameters[2], pFrame->payload.parameters[3], pFrame->payload.parameters[4],
           pFrame->payload.parameters[5], pFrame->payload.parameters[6], pFrame->payload.parameters[7],
           pFrame->payload.parameters[8], forUs ? "for-us" : "other");

  // Raw 16-byte dump of the frame exactly as received off the air.
  ESP_LOGD(TAG, "RX RAW  %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X", pData[0],
           pData[1], pData[2], pData[3], pData[4], pData[5], pData[6], pData[7], pData[8], pData[9], pData[10],
           pData[11], pData[12], pData[13], pData[14], pData[15]);
}

void ZehnderRF::logTransmittedFrame(const uint8_t *const pData, const uint8_t dataLength) {
  if (dataLength < FAN_FRAMESIZE) {
    return;
  }

  const RfFrame *const pFrame = (const RfFrame *) pData;

  ESP_LOGD(TAG,
           "TX FRAME  to[type=0x%02X(%s) id=0x%02X]  from[type=0x%02X(%s) id=0x%02X]  "
           "cmd=0x%02X(%s)  ttl=%u  params=%u  "
           "payload=%02X %02X %02X %02X %02X %02X %02X %02X %02X",
           pFrame->rx_type, typeToString(pFrame->rx_type), pFrame->rx_id, pFrame->tx_type,
           typeToString(pFrame->tx_type), pFrame->tx_id, pFrame->command, commandToString(pFrame->command),
           pFrame->ttl, pFrame->parameter_count, pFrame->payload.parameters[0], pFrame->payload.parameters[1],
           pFrame->payload.parameters[2], pFrame->payload.parameters[3], pFrame->payload.parameters[4],
           pFrame->payload.parameters[5], pFrame->payload.parameters[6], pFrame->payload.parameters[7],
           pFrame->payload.parameters[8]);

  // Raw 16-byte dump of the frame exactly as transmitted on the air.
  ESP_LOGD(TAG, "TX RAW  %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X", pData[0],
           pData[1], pData[2], pData[3], pData[4], pData[5], pData[6], pData[7], pData[8], pData[9], pData[10],
           pData[11], pData[12], pData[13], pData[14], pData[15]);
}

void ZehnderRF::loop(void) {
  uint8_t deviceId;
  nrf905::Config rfConfig;

  // Run RF handler
  this->rfHandler();

  switch (this->state_) {
    case StateStartup:
      // Wait until started up
      if (millis() > 15000) {
        // Discovery?
        if ((this->config_.fan_networkId == 0x00000000) || (this->config_.fan_my_device_type == 0) ||
            (this->config_.fan_my_device_id == 0) || (this->config_.fan_main_unit_type == 0) ||
            (this->config_.fan_main_unit_id == 0)) {
          ESP_LOGD(TAG, "Invalid config, start paring");

          this->state_ = StateStartDiscovery;
        } else {
          ESP_LOGD(TAG, "Config data valid, start polling");

          rfConfig = this->rf_->getConfig();
          rfConfig.rx_address = this->config_.fan_networkId;
          this->rf_->updateConfig(&rfConfig);
          this->rf_->writeTxAddress(this->config_.fan_networkId);

          // Start with query
          this->queryDevice();
        }
      }
      break;

    case StateStartDiscovery:
      deviceId = this->createDeviceID();
      this->discoveryStart(deviceId);

      // For now just set TX
      break;

    case StateIdle:
      if (newSetting == true) {
        this->setSpeed(newSpeed, newTimer);
      } else {
        if ((millis() - this->lastFanQuery_) > this->interval_) {
          this->queryDevice();
        }
      }
      break;

    case StateWaitSetSpeedConfirm:
      if (this->rfState_ == RfStateIdle) {
        // When done, return to idle
        this->state_ = StateIdle;
      }

    default:
      break;
  }
}

void ZehnderRF::rfHandleReceived(const uint8_t *const pData, const uint8_t dataLength) {
  this->logReceivedFrame(pData, dataLength);

  const RfFrame *const pResponse = (RfFrame *) pData;
  RfFrame *const pTxFrame = (RfFrame *) this->_txFrame;  // frame helper
  nrf905::Config rfConfig;

  ESP_LOGD(TAG, "Current state: 0x%02X", this->state_);
  switch (this->state_) {
    case StateDiscoveryWaitForLinkRequest:
      ESP_LOGD(TAG, "DiscoverStateWaitForLinkRequest");
      switch (pResponse->command) {
        case FAN_NETWORK_JOIN_OPEN:  // Received linking request from main unit
          ESP_LOGD(TAG, "Discovery: Found unit type 0x%02X (%s) with ID 0x%02X on network 0x%08X", pResponse->tx_type,
                   pResponse->tx_type == FAN_TYPE_MAIN_UNIT ? "Main" : "?", pResponse->tx_id,
                   pResponse->payload.networkJoinOpen.networkId);

          this->rfComplete();

          (void) memset(this->_txFrame, 0, FAN_FRAMESIZE);  // Clear frame data

          // Found a main unit, so send a join request
          pTxFrame->rx_type = FAN_TYPE_MAIN_UNIT;  // Set type to main unit
          pTxFrame->rx_id = pResponse->tx_id;      // Set ID to the ID of the main unit
          pTxFrame->tx_type = this->config_.fan_my_device_type;
          pTxFrame->tx_id = this->config_.fan_my_device_id;
          pTxFrame->ttl = FAN_TTL;
          pTxFrame->command = FAN_NETWORK_JOIN_REQUEST;  // Request to connect to network
          pTxFrame->parameter_count = sizeof(RfPayloadNetworkJoinOpen);
          // Request to connect to the received network ID
          pTxFrame->payload.networkJoinRequest.networkId = pResponse->payload.networkJoinOpen.networkId;

          // Store for later
          this->config_.fan_networkId = pResponse->payload.networkJoinOpen.networkId;
          this->config_.fan_main_unit_type = pResponse->tx_type;
          this->config_.fan_main_unit_id = pResponse->tx_id;

          // Update address
          rfConfig = this->rf_->getConfig();
          rfConfig.rx_address = pResponse->payload.networkJoinOpen.networkId;
          this->rf_->updateConfig(&rfConfig, NULL);
          this->rf_->writeTxAddress(pResponse->payload.networkJoinOpen.networkId, NULL);

          // Send response frame
          this->startTransmit(this->_txFrame, FAN_TX_RETRIES, [this]() {
            ESP_LOGW(TAG, "Query Timeout");
            this->state_ = StateStartDiscovery;
          });

          this->state_ = StateDiscoveryWaitForJoinResponse;
          break;

        default:
          ESP_LOGD(TAG, "Discovery: Received unknown frame type 0x%02X from ID 0x%02X", pResponse->command,
                   pResponse->tx_id);
          break;
      }
      break;

    case StateDiscoveryWaitForJoinResponse:
      ESP_LOGD(TAG, "DiscoverStateWaitForJoinResponse");
      switch (pResponse->command) {
        case FAN_FRAME_0B:
          if ((pResponse->rx_type == this->config_.fan_my_device_type) &&
              (pResponse->rx_id == this->config_.fan_my_device_id) &&
              (pResponse->tx_type == this->config_.fan_main_unit_type) &&
              (pResponse->tx_id == this->config_.fan_main_unit_id)) {
            ESP_LOGD(TAG, "Discovery: Link successful to unit with ID 0x%02X on network 0x%08X", pResponse->tx_id,
                     this->config_.fan_networkId);

            this->rfComplete();

            (void) memset(this->_txFrame, 0, FAN_FRAMESIZE);  // Clear frame data

            pTxFrame->rx_type = FAN_TYPE_MAIN_UNIT;  // Set type to main unit
            pTxFrame->rx_id = pResponse->tx_id;      // Set ID to the ID of the main unit
            pTxFrame->tx_type = this->config_.fan_my_device_type;
            pTxFrame->tx_id = this->config_.fan_my_device_id;
            pTxFrame->ttl = FAN_TTL;
            pTxFrame->command = FAN_FRAME_0B;  // 0x0B acknowledge link successful
            pTxFrame->parameter_count = 0x00;  // No parameters

            // Send response frame
            this->startTransmit(this->_txFrame, FAN_TX_RETRIES, [this]() {
              ESP_LOGW(TAG, "Query Timeout");
              this->state_ = StateStartDiscovery;
            });

            this->state_ = StateDiscoveryJoinComplete;
          } else {
            ESP_LOGE(TAG, "Discovery: Received unknown link success from ID 0x%02X on network 0x%08X", pResponse->tx_id,
                     this->config_.fan_networkId);
          }
          break;

        default:
          ESP_LOGE(TAG, "Discovery: Received unknown frame type 0x%02X from ID 0x%02X", pResponse->command,
                   pResponse->tx_id);
          break;
      }
      break;

    case StateDiscoveryJoinComplete:
      ESP_LOGD(TAG, "StateDiscoveryJoinComplete");
      switch (pResponse->command) {
        case FAN_TYPE_QUERY_NETWORK:
          if ((pResponse->rx_type == this->config_.fan_main_unit_type) &&
              (pResponse->rx_id == this->config_.fan_main_unit_id) &&
              (pResponse->tx_type == this->config_.fan_main_unit_type) &&
              (pResponse->tx_id == this->config_.fan_main_unit_id)) {
            ESP_LOGD(TAG, "Discovery: received network join success 0x0D");

            this->rfComplete();

            ESP_LOGD(TAG, "Saving pairing config");
            this->pref_.save(&this->config_);

            this->state_ = StateIdle;
          } else {
            ESP_LOGW(TAG, "Unexpected frame join reponse from Type 0x%02X ID 0x%02X", pResponse->tx_type,
                     pResponse->tx_id);
          }
          break;

        default:
          ESP_LOGE(TAG, "Discovery: Received unknown frame type 0x%02X from ID 0x%02X on network 0x%08X",
                   pResponse->command, pResponse->tx_id, this->config_.fan_networkId);
          break;
      }
      break;

    case StateWaitQueryResponse:
      if ((pResponse->rx_type == this->config_.fan_my_device_type) &&  // If type
          (pResponse->rx_id == this->config_.fan_my_device_id)) {      // and id match, it is for us
        switch (pResponse->command) {
          case FAN_TYPE_FAN_SETTINGS:
            ESP_LOGD(TAG, "Received fan settings; speed: 0x%02X voltage: %i flags: 0x%02X",
                     pResponse->payload.fanSettings.speed, pResponse->payload.fanSettings.voltage,
                     pResponse->payload.fanSettings.flags);

            ++this->querySuccesses_;  // diagnostic: poll got a valid response

            // Self-heal: a good poll means the link is alive; reset the counter.
            this->consecutive_query_timeouts_ = 0;
            if (this->self_healing_) {
              ESP_LOGI(TAG, "Self-heal: poll link restored, leaving re-pair mode");
              this->self_healing_ = false;
              this->self_heal_discovery_attempts_ = 0;
            }

            this->rfComplete();

            this->fan_voltage_ = pResponse->payload.fanSettings.voltage;
            this->have_fan_settings_ = true;
            this->updateTimerFromDevice_(pResponse->payload.fanSettings.flags);
            this->publishFanState_(pResponse->payload.fanSettings.speed);

            this->state_ = StateIdle;
            break;

          default:
            ESP_LOGD(TAG, "Received unexpected frame; type 0x%02X from ID 0x%02X", pResponse->command,
                     pResponse->tx_id);
            break;
        }
      } else {
        ESP_LOGD(TAG, "Received frame from unknown device; type 0x%02X from ID 0x%02X type 0x%02X", pResponse->command,
                 pResponse->tx_id, pResponse->tx_type);
      }
      break;

    case StateWaitSetSpeedResponse:
      if ((pResponse->rx_type == this->config_.fan_my_device_type) &&  // If type
          (pResponse->rx_id == this->config_.fan_my_device_id)) {      // and id match, it is for us
        switch (pResponse->command) {
          case FAN_TYPE_FAN_SETTINGS:
            ESP_LOGD(TAG, "Received fan settings; speed: 0x%02X voltage: %i flags: 0x%02X",
                     pResponse->payload.fanSettings.speed, pResponse->payload.fanSettings.voltage,
                     pResponse->payload.fanSettings.flags);
            this->rfComplete();

            // Fresh reading straight after our own command: keep the timer and
            // voltage, but leave the published speed to the optimistic value
            // control()/startBoost() already sent -- some units answer with the
            // speed they had *before* applying the command.
            this->fan_voltage_ = pResponse->payload.fanSettings.voltage;
            this->have_fan_settings_ = true;
            this->updateTimerFromDevice_(pResponse->payload.fanSettings.flags);

            (void) memset(this->_txFrame, 0, FAN_FRAMESIZE);  // Clear frame data

            pTxFrame->rx_type = this->config_.fan_main_unit_type;  // Set type to main unit
            pTxFrame->rx_id = this->config_.fan_main_unit_id;      // Set ID to the ID of the main unit
            pTxFrame->tx_type = this->config_.fan_my_device_type;
            pTxFrame->tx_id = this->config_.fan_my_device_id;
            pTxFrame->ttl = FAN_TTL;
            pTxFrame->command = FAN_FRAME_SETSPEED_REPLY;  // 0x0B acknowledge link successful
            pTxFrame->parameter_count = 0x03;              // 3 parameters
            pTxFrame->payload.parameters[0] = 0x54;
            pTxFrame->payload.parameters[1] = 0x03;
            pTxFrame->payload.parameters[2] = 0x20;

            // Send response frame
            this->startTransmit(this->_txFrame, -1, NULL);

            this->state_ = StateWaitSetSpeedConfirm;
            break;

          case FAN_FRAME_SETSPEED_REPLY:
          case FAN_FRAME_SETVOLTAGE_REPLY:
            // this->rfComplete();

            // this->state_ = StateIdle;
            break;

          default:
            ESP_LOGD(TAG, "Received unexpected frame; type 0x%02X from ID 0x%02X", pResponse->command,
                     pResponse->tx_id);
            break;
        }
      } else {
        ESP_LOGD(TAG, "Received frame from unknown device; type 0x%02X from ID 0x%02X type 0x%02X", pResponse->command,
                 pResponse->tx_id, pResponse->tx_type);
      }
      break;

    case StateIdle:
      // The radio keeps receiving while idle, so unsolicited frames land here:
      // the main unit broadcasting a new fan setting after another remote (or
      // the unit's own buttons) changed the speed, plus mesh re-broadcasts of
      // those frames. Decode the ones addressed to us and push the update
      // straight to Home Assistant instead of waiting for the next poll.
      if ((pResponse->rx_type == this->config_.fan_my_device_type) &&  // If type
          (pResponse->rx_id == this->config_.fan_my_device_id)) {      // and id match, it is for us
        switch (pResponse->command) {
          case FAN_TYPE_FAN_SETTINGS: {
            const uint8_t fanSpeed = pResponse->payload.fanSettings.speed;
            ESP_LOGD(TAG, "Unsolicited fan settings; speed: 0x%02X voltage: %i flags: 0x%02X", fanSpeed,
                     pResponse->payload.fanSettings.voltage, pResponse->payload.fanSettings.flags);

            this->fan_voltage_ = pResponse->payload.fanSettings.voltage;
            this->have_fan_settings_ = true;
            this->updateTimerFromDevice_(pResponse->payload.fanSettings.flags);

            // Only publish on an actual change; mesh re-broadcasts repeat the
            // same values and would otherwise spam Home Assistant. on/off and
            // the mode both follow the speed, so the speed is the whole test.
            if (this->speed != fanSpeed) {
              this->publishFanState_(fanSpeed);
            }
            break;
          }

          default:
            ESP_LOGD(TAG, "Received unhandled frame while idle; type 0x%02X from ID 0x%02X", pResponse->command,
                     pResponse->tx_id);
            break;
        }
      } else if (pResponse->rx_type == this->config_.fan_main_unit_type) {
        // Frames addressed to the main unit are commands from *other* remotes
        // (or apps/sensors) changing the fan. We only hear these because the
        // radio now listens continuously while idle. Decode the requested speed
        // and reflect it in Home Assistant immediately, instead of waiting for
        // the next poll. (These are requests, not confirmations; the next poll
        // reconciles with the main unit's actual reported state.)
        uint8_t requestedSpeed;
        bool haveSpeed = true;
        switch (pResponse->command) {
          case FAN_FRAME_SETSPEED:  // 0x02: payload = { speed }
            requestedSpeed = pResponse->payload.setSpeed.speed;
            this->clearTimerWindow_();  // a plain speed command ends any timer
            break;
          case FAN_FRAME_SETTIMER:  // 0x03: payload = { speed, timer }
            requestedSpeed = pResponse->payload.setTimer.speed;
            // A boost started from a physical remote: start the countdown from
            // the duration that remote asked for, so the sensor tracks it too.
            this->seedTimerWindow_(pResponse->payload.setTimer.timer);
            break;
          default:
            // e.g. SETVOLTAGE (a raw percentage, not a preset) or other
            // traffic: log only and let the next poll pick up the result.
            haveSpeed = false;
            break;
        }

        if (haveSpeed) {
          ESP_LOGD(TAG, "Observed external change from type 0x%02X id 0x%02X: cmd 0x%02X speed %u", pResponse->tx_type,
                   pResponse->tx_id, pResponse->command, requestedSpeed);

          // Only publish on an actual change; mesh re-broadcasts repeat the same
          // command and would otherwise spam Home Assistant.
          if (this->speed != requestedSpeed) {
            this->publishFanState_(requestedSpeed);
          }
        }
      }
      // Any other frames were already recorded by logReceivedFrame() above.
      break;

    default:
      ESP_LOGD(TAG, "Received frame from unknown device in unknown state; type 0x%02X from ID 0x%02X type 0x%02X",
               pResponse->command, pResponse->tx_id, pResponse->tx_type);
      break;
  }
}

static uint8_t minmax(const uint8_t value, const uint8_t min, const uint8_t max) {
  if (value <= min) {
    return min;
  } else if (value >= max) {
    return max;
  } else {
    return value;
  }
}

uint8_t ZehnderRF::createDeviceID(void) {
  uint8_t random = (uint8_t) random_uint32();
  // Generate random device_id; don't use 0x00 and 0xFF

  // TODO: there's a 1 in 255 chance that the generated ID matches the ID of the main unit. Decide how to deal
  // with this (some sort of ping discovery?)

  return minmax(random, 1, 0xFE);
}

void ZehnderRF::startPairing(void) {
  ESP_LOGW(TAG, "Manual re-pairing requested -> entering discovery. Put the main unit in its pairing "
                "window now (ComfoFan S: power-cycle the unit for a ~10 min join window).");
  // Drop the (stale) identity so a failed join can't fall back to bad data, then
  // re-run the join/discovery handshake on the next loop(). The new identity is
  // persisted only once the join completes (StateDiscoveryJoinComplete).
  this->config_.fan_my_device_id = 0;
  this->config_.fan_main_unit_id = 0;
  this->state_ = StateStartDiscovery;
}

void ZehnderRF::applyYamlPin_(void) {
  if (!this->has_yaml_pairing_) {
    return;
  }
  this->config_.fan_networkId = this->yaml_network_id_;
  this->config_.fan_main_unit_type = this->yaml_main_unit_type_;
  this->config_.fan_main_unit_id = this->yaml_main_unit_id_;
  this->config_.fan_my_device_type = this->yaml_device_type_;
  this->config_.fan_my_device_id = this->yaml_device_id_;
  ESP_LOGCONFIG(TAG, "Using YAML-pinned pairing: net=0x%08X main=0x%02X/0x%02X dev=0x%02X/0x%02X",
                this->config_.fan_networkId, this->config_.fan_main_unit_type, this->config_.fan_main_unit_id,
                this->config_.fan_my_device_type, this->config_.fan_my_device_id);
}

void ZehnderRF::onQueryTimeout_(void) {
  if ((this->self_heal_threshold_ == 0) || this->self_healing_) {
    return;  // self-heal disabled, or already in a re-pair episode
  }
  ++this->consecutive_query_timeouts_;
  if (this->consecutive_query_timeouts_ < this->self_heal_threshold_) {
    return;
  }
  if (millis() < this->self_heal_cooldown_until_ms_) {
    return;  // backing off after a recent failed self-heal
  }
  ESP_LOGE(TAG,
           "Self-heal: %u consecutive poll timeouts -> RF link to the fan looks dead. Auto re-pairing "
           "(discovery). This assigns a NEW random device id and only succeeds while the main unit is in its "
           "power-up join window; if it keeps failing, power-cycle the ComfoFan S.",
           this->consecutive_query_timeouts_);
  this->self_healing_ = true;
  this->self_heal_discovery_attempts_ = 0;
  // Enter discovery. We do NOT clear the stored config here, so a failed episode
  // can cleanly restore it (resumePollingWithStoredConfig_).
  this->state_ = StateStartDiscovery;
}

void ZehnderRF::onDiscoveryTimeout_(void) {
  if (this->self_healing_) {
    ++this->self_heal_discovery_attempts_;
    if (this->self_heal_discovery_attempts_ >= SELF_HEAL_MAX_DISCOVERY_ATTEMPTS) {
      ESP_LOGW(TAG,
               "Self-heal: re-pair failed after %u attempts (main unit not in a join window). Resuming polling "
               "with the stored pairing; next self-heal attempt in %u min.",
               (unsigned) this->self_heal_discovery_attempts_, (unsigned) (SELF_HEAL_COOLDOWN_MS / 60000U));
      this->self_healing_ = false;
      this->self_heal_discovery_attempts_ = 0;
      this->consecutive_query_timeouts_ = 0;
      this->self_heal_cooldown_until_ms_ = millis() + SELF_HEAL_COOLDOWN_MS;
      this->resumePollingWithStoredConfig_();
      return;
    }
    ESP_LOGW(TAG, "Self-heal: discovery attempt %u/%u timed out (unit not in its join window yet)",
             (unsigned) this->self_heal_discovery_attempts_, (unsigned) SELF_HEAL_MAX_DISCOVERY_ATTEMPTS);
  } else {
    ESP_LOGW(TAG, "Start discovery timeout");
  }
  this->state_ = StateStartDiscovery;
}

void ZehnderRF::resumePollingWithStoredConfig_(void) {
  // Discovery mutated config_ in RAM (new device id, link rx address) but only
  // persists on a successful join, so flash still holds the last-good pairing.
  // Reload it, re-apply any YAML pin, reconfigure the radio, and resume polling.
  memset(&this->config_, 0, sizeof(Config));
  this->pref_.load(&this->config_);
  this->applyYamlPin_();

  nrf905::Config rfConfig = this->rf_->getConfig();
  rfConfig.rx_address = this->config_.fan_networkId;
  this->rf_->updateConfig(&rfConfig);
  this->rf_->writeTxAddress(this->config_.fan_networkId);

  ESP_LOGW(TAG, "Self-heal: restored stored pairing (main=0x%02X net=0x%08X), resuming polling",
           this->config_.fan_main_unit_id, this->config_.fan_networkId);
  this->state_ = StateIdle;
}

void ZehnderRF::queryDevice(void) {
  RfFrame *const pFrame = (RfFrame *) this->_txFrame;  // frame helper

  ESP_LOGD(TAG, "Query device");

  ++this->queryAttempts_;          // diagnostic: poll attempt
  this->lastFanQuery_ = millis();  // Update time

  // Clear frame data
  (void) memset(this->_txFrame, 0, FAN_FRAMESIZE);

  // Build frame
  pFrame->rx_type = this->config_.fan_main_unit_type;
  pFrame->rx_id = this->config_.fan_main_unit_id;
  pFrame->tx_type = this->config_.fan_my_device_type;
  pFrame->tx_id = this->config_.fan_my_device_id;
  pFrame->ttl = FAN_TTL;
  pFrame->command = FAN_TYPE_QUERY_DEVICE;
  pFrame->parameter_count = 0x00;  // No parameters

  this->startTransmit(this->_txFrame, FAN_TX_RETRIES, [this]() {
    ESP_LOGW(TAG, "Query Timeout");
    this->state_ = StateIdle;
    this->onQueryTimeout_();
  });

  this->state_ = StateWaitQueryResponse;
}

void ZehnderRF::setSpeed(const uint8_t paramSpeed, const uint8_t paramTimer) {
  RfFrame *const pFrame = (RfFrame *) this->_txFrame;  // frame helper
  uint8_t speed = paramSpeed;
  uint8_t timer = paramTimer;

  if (speed > this->speed_count_) {
    ESP_LOGW(TAG, "Requested speed too high (%u)", speed);
    speed = this->speed_count_;
  }

  ESP_LOGD(TAG, "Set speed: 0x%02X; Timer %u minutes", speed, timer);

  // Track the boost window from the command itself, so the countdown starts at
  // once instead of waiting for the fan to confirm on the next poll. A poll that
  // reports no timer clears it again, so a command the fan ignored self-corrects.
  if (timer != 0) {
    this->seedTimerWindow_(timer);
  } else {
    this->clearTimerWindow_();
  }

  if (this->state_ == StateIdle) {
    (void) memset(this->_txFrame, 0, FAN_FRAMESIZE);  // Clear frame data

    // Build frame
    pFrame->rx_type = this->config_.fan_main_unit_type;
    pFrame->rx_id = this->config_.fan_main_unit_id;
    pFrame->tx_type = this->config_.fan_my_device_type;
    pFrame->tx_id = this->config_.fan_my_device_id;
    pFrame->ttl = FAN_TTL;

    if (timer == 0) {
      pFrame->command = FAN_FRAME_SETSPEED;
      pFrame->parameter_count = sizeof(RfPayloadFanSetSpeed);
      pFrame->payload.setSpeed.speed = speed;
    } else {
      pFrame->command = FAN_FRAME_SETTIMER;
      pFrame->parameter_count = sizeof(RfPayloadFanSetTimer);
      pFrame->payload.setTimer.speed = speed;
      pFrame->payload.setTimer.timer = timer;
    }

    this->startTransmit(this->_txFrame, FAN_TX_RETRIES, [this]() {
      ESP_LOGW(TAG, "Set speed timeout");
      this->state_ = StateIdle;
    });

    newSetting = false;
    this->state_ = StateWaitSetSpeedResponse;
  } else {
    ESP_LOGD(TAG, "Invalid state, I'm trying later again");
    newSpeed = speed;
    newTimer = timer;
    newSetting = true;
  }
}

void ZehnderRF::set_timer_minutes(const uint8_t minutes) {
  this->timer_minutes_ = minmax(minutes, FAN_TIMER_MIN_MINUTES, FAN_TIMER_MAX_MINUTES);
  ESP_LOGD(TAG, "Boost duration set to %u minutes", this->timer_minutes_);
}

void ZehnderRF::startBoost(const uint8_t paramMinutes) {
  const uint8_t minutes = (paramMinutes == 0) ? this->timer_minutes_ : paramMinutes;

  // Remember where to come back to, but only when a boost is not already running:
  // boosting again must not overwrite the memory with the boost speed itself. And
  // only when the current speed is actually known -- before the first reply from
  // the fan it reads 0, and coming back to 0 would stop the unit rather than
  // restore it.
  const bool speed_is_known = this->have_fan_settings_ || (this->speed > 0);
  if (!this->timer_window_active_ && speed_is_known) {
    this->pre_boost_speed_ = (uint8_t) this->speed;
    this->has_pre_boost_speed_ = true;
  }

  if (this->has_pre_boost_speed_) {
    ESP_LOGI(TAG, "Boost: speed %u for %u minutes (will return to speed %u)", this->boost_speed_, minutes,
             this->pre_boost_speed_);
  } else {
    ESP_LOGI(TAG, "Boost: speed %u for %u minutes (current speed unknown, will fall back to speed %u)",
             this->boost_speed_, minutes, this->revert_speed_);
  }
  this->setSpeed(this->boost_speed_, minutes);
  // Show the boost right away; the next poll reconciles with the fan.
  this->publishFanState_(this->boost_speed_);
}

void ZehnderRF::cancelBoost(void) {
  // Back to the speed the boost started from. The configured fall-back only steps
  // in when that is unknown -- after a reboot mid-boost, say.
  const uint8_t target = this->has_pre_boost_speed_ ? this->pre_boost_speed_ : this->revert_speed_;

  ESP_LOGI(TAG, "Boost cancelled: back to speed %u (%s)", target,
           this->has_pre_boost_speed_ ? "where it was" : "configured fall-back");
  this->setSpeed(target, 0);  // clears the timer window, and the memory with it
  this->publishFanState_(target);
}

void ZehnderRF::seedTimerWindow_(const uint8_t minutes) {
  if (minutes == 0) {
    this->clearTimerWindow_();
    return;
  }
  this->timer_end_ms_ = millis() + ((uint32_t) minutes * 60000UL);
  this->timer_window_active_ = true;
}

void ZehnderRF::clearTimerWindow_(void) {
  this->timer_window_active_ = false;
  this->timer_end_ms_ = 0;
  // No boost running means nothing to come back to.
  this->has_pre_boost_speed_ = false;
}

void ZehnderRF::updateTimerFromDevice_(const uint8_t flags) {
  this->fan_flags_ = flags;

  if ((flags & FAN_SETTINGS_FLAG_TIMER) == 0) {
    // No timer running any more -- either it expired or something cancelled it.
    if (this->timer_window_active_) {
      ESP_LOGD(TAG, "Fan reports no timer running, clearing the count-down");
    }
    this->clearTimerWindow_();
    return;
  }

  // A timer is running. When we have no window yet the boost was started
  // elsewhere (a remote whose command we did not overhear, or before a reboot),
  // so fall back to the configured duration -- the best guess available, since
  // the unit never reports the time left.
  if (!this->timer_window_active_) {
    ESP_LOGD(TAG, "Fan reports a running timer we did not start, assuming %u minutes", this->timer_minutes_);
    this->seedTimerWindow_(this->timer_minutes_);
  }
}

float ZehnderRF::getTimerRemainingMinutes(void) const {
  if (!this->timer_window_active_) {
    return NAN;  // no timer running -> "unknown" rather than a misleading zero
  }

  const int32_t remaining = (int32_t) (this->timer_end_ms_ - millis());
  if (remaining <= 0) {
    return 0.0f;
  }
  return (float) ((remaining + 59999) / 60000);  // whole minutes, rounded up
}

float ZehnderRF::getFanPercentage(void) const {
  if (!this->have_fan_settings_) {
    return NAN;
  }
  // The byte is already a percentage (30 = 30% = 3.0 V on the unit's 0-10 V out).
  return (float) this->fan_voltage_;
}

void ZehnderRF::discoveryStart(const uint8_t deviceId) {
  RfFrame *const pFrame = (RfFrame *) this->_txFrame;  // frame helper
  nrf905::Config rfConfig;

  ESP_LOGD(TAG, "Start discovery with ID %u", deviceId);

  this->config_.fan_my_device_type = FAN_TYPE_REMOTE_CONTROL;
  this->config_.fan_my_device_id = deviceId;

  // Build frame
  (void) memset(this->_txFrame, 0, FAN_FRAMESIZE);  // Clear frame data

  // Set payload, available for linking
  pFrame->rx_type = 0x04;
  pFrame->rx_id = 0x00;
  pFrame->tx_type = this->config_.fan_my_device_type;
  pFrame->tx_id = this->config_.fan_my_device_id;
  pFrame->ttl = FAN_TTL;
  pFrame->command = FAN_NETWORK_JOIN_ACK;
  pFrame->parameter_count = sizeof(RfPayloadNetworkJoinAck);
  pFrame->payload.networkJoinAck.networkId = NETWORK_LINK_ID;

  // Set RX and TX address
  rfConfig = this->rf_->getConfig();
  rfConfig.rx_address = NETWORK_LINK_ID;
  this->rf_->updateConfig(&rfConfig, NULL);
  this->rf_->writeTxAddress(NETWORK_LINK_ID, NULL);

  this->startTransmit(this->_txFrame, FAN_TX_RETRIES, [this]() { this->onDiscoveryTimeout_(); });

  // Update state
  this->state_ = StateDiscoveryWaitForLinkRequest;
}

Result ZehnderRF::startTransmit(const uint8_t *const pData, const int8_t rxRetries,
                                const std::function<void(void)> callback) {
  Result result = ResultOk;
  unsigned long startTime;
  bool busy = true;

  if (this->rfState_ != RfStateIdle) {
    ESP_LOGW(TAG, "TX still ongoing");
    result = ResultBusy;
  } else {
    this->onReceiveTimeout_ = callback;
    this->retries_ = rxRetries;

    // Write data to RF
    // if (pData != NULL) {  // If frame given, load it in the nRF. Else use previous TX payload
    // ESP_LOGD(TAG, "Write payload");
    this->rf_->writeTxPayload(pData, FAN_FRAMESIZE);  // Use framesize
    this->logTransmittedFrame(pData, FAN_FRAMESIZE);
    // }

    this->rfState_ = RfStateWaitAirwayFree;
    this->airwayFreeWaitTime_ = millis();
  }

  return result;
}

void ZehnderRF::rfComplete(void) {
  this->retries_ = -1;  // Disable this->retries_
  this->rfState_ = RfStateIdle;
}

void ZehnderRF::rfHandler(void) {
  switch (this->rfState_) {
    case RfStateIdle:
      // Keep the radio actively listening while idle. Otherwise the receiver is
      // only enabled in the brief window after our own TX, so we only ever hear
      // replies to our own queries -- never unsolicited frames from other
      // devices (e.g. another remote or the main unit changing the fan).
      if (this->rf_->getMode() != nrf905::Receive) {
        ESP_LOGD(TAG, "Idle: enabling continuous receive (was mode %u)", (unsigned) this->rf_->getMode());
        this->rf_->setMode(nrf905::Receive);
      }
      break;

    case RfStateWaitAirwayFree:
      if ((millis() - this->airwayFreeWaitTime_) > 5000) {
        ESP_LOGW(TAG, "Airway too busy, giving up");
        this->rfState_ = RfStateIdle;

        if (this->onReceiveTimeout_ != NULL) {
          this->onReceiveTimeout_();
        }
      } else if (this->rf_->airwayBusy() == false) {
        ESP_LOGD(TAG, "Start TX");
        this->rf_->startTx(FAN_TX_FRAMES, nrf905::Receive);  // After transmit, wait for response

        this->rfState_ = RfStateTxBusy;
      }
      break;

    case RfStateTxBusy:
      break;

    case RfStateRxWait:
      if ((this->retries_ >= 0) && ((millis() - this->msgSendTime_) > FAN_REPLY_TIMEOUT)) {
        ESP_LOGD(TAG, "Receive timeout");

        if (this->retries_ > 0) {
          --this->retries_;
          ESP_LOGD(TAG, "No data received, retry again (left: %u)", this->retries_);

          this->rfState_ = RfStateWaitAirwayFree;
          this->airwayFreeWaitTime_ = millis();
        } else if (this->retries_ == 0) {
          // Oh oh, ran out of options

          ESP_LOGD(TAG, "No messages received, giving up now...");
          if (this->onReceiveTimeout_ != NULL) {
            this->onReceiveTimeout_();
          }

          // Back to idle
          this->rfState_ = RfStateIdle;
        }
      }
      break;

    default:
      break;
  }
}

}  // namespace zehnder
}  // namespace esphome
