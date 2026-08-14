#ifndef __COMPONENT_ZEHNDER_H__
#define __COMPONENT_ZEHNDER_H__

#include <string>

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/spi/spi.h"
#include "esphome/components/fan/fan.h"
#include "esphome/components/nrf905/nRF905.h"

namespace esphome {
namespace zehnder {

#define FAN_FRAMESIZE 16        // Each frame consists of 16 bytes
#define FAN_TX_FRAMES 4         // Retransmit every transmitted frame 4 times
#define FAN_TX_RETRIES 10       // Retry transmission 10 times if no reply is received
#define FAN_TTL 250             // 0xFA, default time-to-live for a frame
#define FAN_REPLY_TIMEOUT 1000  // Wait 500ms for receiving a reply when doing a network scan

/* Fan device types */
enum {
  FAN_TYPE_BROADCAST = 0x00,       // Broadcast to all devices
  FAN_TYPE_MAIN_UNIT = 0x01,       // Fans
  FAN_TYPE_REMOTE_CONTROL = 0x03,  // Remote controls
  FAN_TYPE_CO2_SENSOR = 0x18       // CO2 sensors
};

/* Fan commands */
enum {
  FAN_FRAME_SETVOLTAGE = 0x01,  // Set speed (voltage / percentage)
  FAN_FRAME_SETSPEED = 0x02,    // Set speed (preset)
  FAN_FRAME_SETTIMER = 0x03,    // Set speed with timer
  FAN_NETWORK_JOIN_REQUEST = 0x04,
  FAN_FRAME_SETSPEED_REPLY = 0x05,
  FAN_NETWORK_JOIN_OPEN = 0x06,
  FAN_TYPE_FAN_SETTINGS = 0x07,  // Current settings, sent by fan in reply to 0x01, 0x02, 0x10
  FAN_FRAME_0B = 0x0B,
  FAN_NETWORK_JOIN_ACK = 0x0C,
  // FAN_NETWORK_JOIN_FINISH = 0x0D,
  FAN_TYPE_QUERY_NETWORK = 0x0D,
  FAN_TYPE_QUERY_DEVICE = 0x10,
  FAN_FRAME_SETVOLTAGE_REPLY = 0x1D
};

/* Fan speed presets */
enum {
  FAN_SPEED_AUTO = 0x00,    // Off:      0% or  0.0 volt
  FAN_SPEED_LOW = 0x01,     // Low:     30% or  3.0 volt
  FAN_SPEED_MEDIUM = 0x02,  // Medium:  50% or  5.0 volt
  FAN_SPEED_HIGH = 0x03,    // High:    90% or  9.0 volt
  FAN_SPEED_MAX = 0x04
};  // Max:    100% or 10.0 volt

#define NETWORK_LINK_ID 0xA55A5AA5
#define NETWORK_DEFAULT_ID 0xE7E7E7E7
#define FAN_JOIN_DEFAULT_TIMEOUT 10000

typedef enum { ResultOk, ResultBusy, ResultFailure } Result;

class ZehnderRF : public Component, public fan::Fan {
 public:
  ZehnderRF();

  void setup() override;

  // Setup things
  void set_rf(nrf905::nRF905 *const pRf) { rf_ = pRf; }

  void set_update_interval(const uint32_t interval) { interval_ = interval; }

  // Optionally pin the paired identity from YAML so it survives a flash erase /
  // fresh install with no dependency on a stored discovery. When set, these
  // override whatever is loaded from flash at setup(). Use the values captured
  // by a successful re-pair (start_pairing); see the README.
  void set_paired_config(uint32_t networkId, uint8_t mainType, uint8_t mainId, uint8_t deviceType,
                         uint8_t deviceId) {
    this->yaml_network_id_ = networkId;
    this->yaml_main_unit_type_ = mainType;
    this->yaml_main_unit_id_ = mainId;
    this->yaml_device_type_ = deviceType;
    this->yaml_device_id_ = deviceId;
    this->has_yaml_pairing_ = true;
  }

  // Self-healing: after this many consecutive poll timeouts the link is treated as
  // dead and the device auto re-pairs (discovery). 0 disables. Default 10.
  void set_self_heal_threshold(uint32_t n) { this->self_heal_threshold_ = n; }

  void dump_config() override;

  fan::FanTraits get_traits() override;
  int get_speed_count() { return this->speed_count_; }

  void loop() override;

  void control(const fan::FanCall &call) override;

  float get_setup_priority() const override { return setup_priority::DATA; }

  void setSpeed(const uint8_t speed, const uint8_t timer = 0);

  // Force a fresh (re-)pairing: discard the current identity and re-enter the
  // join/discovery handshake. Use after putting the main unit into its pairing
  // window (on a ComfoFan S: power-cycle the unit → ~10 min join window). The
  // unit then registers this device's id so it will answer our queries/commands.
  void startPairing(void);

  // Debug/diagnostic: poll (query) link reliability.
  uint32_t getQueryAttemptCount(void) const { return this->queryAttempts_; }
  uint32_t getQuerySuccessCount(void) const { return this->querySuccesses_; }
  float getQuerySuccessPercent(void) const {
    return (this->queryAttempts_ == 0) ? 0.0f : (100.0f * (float) this->querySuccesses_ / (float) this->queryAttempts_);
  }

  // Live paired identity (from flash/discovery or a YAML pin), for diagnostic
  // sensors so the current pairing is visible in HA without a log capture.
  uint32_t getNetworkId(void) const { return this->config_.fan_networkId; }
  uint8_t getMainUnitType(void) const { return this->config_.fan_main_unit_type; }
  uint8_t getMainUnitId(void) const { return this->config_.fan_main_unit_id; }
  uint8_t getMyDeviceType(void) const { return this->config_.fan_my_device_type; }
  uint8_t getMyDeviceId(void) const { return this->config_.fan_my_device_id; }
  uint32_t getSelfHealThreshold(void) const { return this->self_heal_threshold_; }

 protected:
  void queryDevice(void);

  uint8_t createDeviceID(void);
  void discoveryStart(const uint8_t deviceId);

  Result startTransmit(const uint8_t *const pData, const int8_t rxRetries = -1,
                       const std::function<void(void)> callback = NULL);
  void rfComplete(void);
  void rfHandler(void);
  void rfHandleReceived(const uint8_t *const pData, const uint8_t dataLength);

  typedef enum {
    StateStartup,
    StateStartDiscovery,
    StateDiscoveryWaitForLinkRequest,
    StateDiscoveryWaitForJoinResponse,
    StateDiscoveryJoinComplete,

    StateIdle,
    StateWaitQueryResponse,
    StateWaitSetSpeedResponse,
    StateWaitSetSpeedConfirm,

    StateNrOf  // Keep last
  } State;
  State state_{StateStartup};
  int speed_count_{};

  nrf905::nRF905 *rf_;
  uint32_t interval_;

  uint8_t _txFrame[FAN_FRAMESIZE];

  ESPPreferenceObject pref_;

  typedef struct {
    uint32_t fan_networkId;      // Fan (Zehnder/BUVA) network ID
    uint8_t fan_my_device_type;  // Fan (Zehnder/BUVA) device type
    uint8_t fan_my_device_id;    // Fan (Zehnder/BUVA) device ID
    uint8_t fan_main_unit_type;  // Fan (Zehnder/BUVA) main unit type
    uint8_t fan_main_unit_id;    // Fan (Zehnder/BUVA) main unit ID
  } Config;
  Config config_;

  // YAML-pinned identity (see set_paired_config). Applied over the flash-loaded
  // config in setup() when has_yaml_pairing_ is true.
  bool has_yaml_pairing_{false};
  uint32_t yaml_network_id_{0};
  uint8_t yaml_main_unit_type_{0};
  uint8_t yaml_main_unit_id_{0};
  uint8_t yaml_device_type_{0};
  uint8_t yaml_device_id_{0};
  void applyYamlPin_();  // overlay the YAML-pinned identity onto config_

  // --- Self-healing re-discovery -----------------------------------------
  // After self_heal_threshold_ consecutive poll timeouts (a clearly-dead link),
  // auto re-pair. Bounded to SELF_HEAL_MAX_DISCOVERY_ATTEMPTS tries per episode,
  // then back off for SELF_HEAL_COOLDOWN_MS so it catches the unit's next join
  // window without flooding the air. Any successful poll resets the counter.
  uint32_t self_heal_threshold_{10};            // 0 = disabled
  uint32_t consecutive_query_timeouts_{0};
  bool self_healing_{false};                    // in a self-heal re-pair episode now
  uint8_t self_heal_discovery_attempts_{0};
  uint32_t self_heal_cooldown_until_ms_{0};
  static const uint8_t SELF_HEAL_MAX_DISCOVERY_ATTEMPTS = 5;
  static const uint32_t SELF_HEAL_COOLDOWN_MS = 1800000;  // 30 min
  void onQueryTimeout_();                        // poll gave up: count + maybe self-heal
  void onDiscoveryTimeout_();                    // discovery try failed: retry or abort
  void resumePollingWithStoredConfig_();         // restore last-good pairing after a failed self-heal

  uint32_t lastFanQuery_{0};
  std::function<void(void)> onReceiveTimeout_ = NULL;

  uint32_t msgSendTime_{0};
  uint32_t airwayFreeWaitTime_{0};
  int8_t retries_{-1};
  uint8_t nextDeviceID{1};

  // Diagnostic counters (see getters above).
  uint32_t queryAttempts_{0};
  uint32_t querySuccesses_{0};

  uint8_t newSpeed{0};
  uint8_t newTimer{0};
  bool newSetting{false};

  typedef enum {
    RfStateIdle,            // Idle state
    RfStateWaitAirwayFree,  // wait for airway free
    RfStateTxBusy,          //
    RfStateRxWait,
  } RfState;
  RfState rfState_{RfStateIdle};

  // Decode + raw-dump every RF frame to the log (RX FRAME/RX RAW, TX FRAME/TX RAW).
  static const char *typeToString(uint8_t type);
  static const char *commandToString(uint8_t command);
  void logReceivedFrame(const uint8_t *const pData, const uint8_t dataLength);
  void logTransmittedFrame(const uint8_t *const pData, const uint8_t dataLength);
};

}  // namespace zehnder
}  // namespace esphome

#endif /* __COMPONENT_ZEHNDER_H__ */
