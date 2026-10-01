#pragma once

#include "SinglePortModule.h"

// Bump/change this string for each distinct custom build you flash, so the boot log
// tells you definitively whether this firmware is running your custom code - not
// just a stock rebuild that happened to reuse the same base firmware version.
#define FARM_BUILD_TAG "farm-shed-v3-generic"

// This list is now IDENTICAL across every shed node's firmware (see gpios[]
// in the .cpp) - every confirmed-safe GPIO on the board is always monitored,
// under a generic name. What each "Sensor N" actually means for a given
// node lives entirely in Node-RED's per-node config, not in firmware. This
// means: no app configuration step at all, no per-shed rebuild, and adding
// a new physical sensor to an existing shed is just wiring it to an already-
// monitored pin and updating Node-RED - no reflash needed.
#define NUM_GPIOS 12

// How long after boot the first full sync fires, before settling into the
// normal STATE_REFRESH_SECS cadence (defined in the .cpp) for every sync
// after that - runs forever, not just once, so the MQTT bridge's
// consecutive-miss detection (currently 3 refreshes) has something to
// compare against on an ongoing basis, not just at startup.
#define SYNC_DELAY_MS 30000

// Text payload that triggers an immediate full refresh when received on this
// module's own port (DETECTION_SENSOR_APP) - distinct from the normal
// "<name> ALARM/clear" and "GPIO_SYNC_COMPLETE" message shapes so there's no
// collision. Broadcasting this makes EVERY node (gateway and every shed)
// refresh immediately - there's no per-node targeting, by design, to avoid
// the addressed/ACK complications we hit earlier with Remote Hardware.
#define REFRESH_REQUEST_TEXT "REQUEST_REFRESH"

struct MultiDetectionGpioConfig {
    uint8_t gpio;      // the GPIO number - not the physical header pin number
    const char *name;  // generic ("Sensor 1", etc) - real meaning lives in Node-RED
    bool usePullup;    // true = use the ESP32's internal pull-up; false = this
                       // specific pin needs an external pull-up resistor wired
    bool activeLow;    // true = "ALARM" means the GPIO reads LOW
};

class MultiDetectionSensorModule : public SinglePortModule, private concurrency::OSThread
{
  public:
    MultiDetectionSensorModule()
        : SinglePortModule("multidetection", meshtastic_PortNum_DETECTION_SENSOR_APP),
          OSThread("MultiDetectionSensor")
    {
    }

  protected:
    virtual int32_t runOnce() override;
    // Called for every incoming packet on this module's port. Verify this
    // exact signature against your own SinglePortModule.h / MeshModule.h -
    // grep "handleReceived" there to confirm before compiling.
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;

  private:
    bool firstTime = true;
    uint32_t lastFullSync = 0; // shared timer for the recurring full refresh + sync-complete cycle
    uint32_t lastSentToMesh[NUM_GPIOS] = {0};
    bool wasDetected[NUM_GPIOS] = {false};

    void sendDetectionMessage(uint8_t index, bool state);
    void sendSyncComplete();
    void doFullRefresh(); // re-sends every pin's state + sync-complete; shared by
                           // the periodic timer and the on-demand request handler
    bool hasDetectionEvent(uint8_t index);
};

extern MultiDetectionSensorModule *multiDetectionSensorModule;
