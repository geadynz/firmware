#pragma once

#include "SinglePortModule.h"

// Bump/change this string for each distinct custom build you flash, so the boot log
// tells you definitively whether this firmware is running your custom code - not
// just a stock rebuild that happened to reuse the same base firmware version.
#define FARM_BUILD_TAG "farm-shed-v3-generic"

// This list is IDENTICAL across every shed of the SAME board type (see
// gpios[] in the .cpp, which selects the right list per board automatically
// via the same HELTEC_V3/HELTEC_V4 macros platformio.ini already defines -
// no manual swapping between environments). What each "Sensor N" actually
// means for a given node lives entirely in Node-RED's per-node config, not
// in firmware. Adding a new physical sensor to an existing shed is just
// wiring it to an already-monitored pin and updating Node-RED - no reflash.
//
// NUM_GPIOS differs by board since V3 and V4 have different reserved pins
// (V4 has an onboard FEM chip and native USB, V3 has neither) - confirm the
// exact macro name via `grep -rn "D HELTEC_V3" variants/` before trusting
// this compiles the branch you expect.
#if defined(HELTEC_V4)
#define NUM_GPIOS 13
#elif defined(HELTEC_V3)
#define NUM_GPIOS 0 // TODO: set once V3's safe pin list is confirmed via the
                    // same rigorous check we did for V4 - do not guess this
#else
#error "MultiDetectionSensorModule: unrecognized board - no confirmed-safe GPIO list exists for it yet"
#endif

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
