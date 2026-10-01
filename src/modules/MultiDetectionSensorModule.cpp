#include "MultiDetectionSensorModule.h"
#include "Default.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "configuration.h"
#include "main.h"
#include <Throttle.h>
#include <cstring>

#if !MESHTASTIC_EXCLUDE_MQTT
#include "mqtt/MQTT.h"
#endif

MultiDetectionSensorModule *multiDetectionSensorModule;

#define GPIO_POLLING_INTERVAL 100
#define MIN_BROADCAST_SECS 15   // minimum seconds between repeat alerts for the same pin
#define STATE_REFRESH_SECS 300  // 5 minutes - periodic re-broadcast so a dropped
                                 // packet doesn't leave the dashboard stale forever

// ---- This list is identical on every shed node's build. ----
// Every bench-confirmed GPIO on the Heltec V4.3 is listed here under a
// generic name, EXCLUDING GPIO45 and GPIO46 - both are ESP32-S3 strapping
// pins that influence boot-time behavior (GPIO45 selects flash I/O voltage,
// GPIO46 controls ROM boot-message verbosity), so they're deliberately left
// out even though they tested as working, to avoid any risk from an
// externally wired switch holding either pin in an unexpected state exactly
// at power-on. Pins with nothing physically wired simply read a stable
// "clear" state forever - Node-RED's per-node config decides which of these
// to actually show/alert on for a given shed.
//
// EXCLUDED, confirmed directly from src/mesh/LoRaFEMInterface.cpp:
//   GPIO2  - LORA_KCT8103L_PA_CSD / LORA_GC1109_PA_EN (FEM chip enable,
//            also used at boot to detect which FEM chip is present)
//   GPIO5  - LORA_KCT8103L_PA_CTX (FEM RX mode select on V4.3)
//   GPIO7  - LORA_PA_POWER / VFEM_Ctrl (FEM supply LDO enable)
// All three are actively driven by the radio's FEM control logic on every
// board revision - confirmed by reading the actual firmware source, not
// just the schematic. Do not use these regardless of what a bench test
// alone might suggest.
//
// GPIO46 was re-added after confirming via the same source that on a real
// V4.3 board (KCT8103L, which this hardware is), fem_type resolves to
// KCT8103L_PA at boot and the GC1109-only code path that drives GPIO46
// never executes - it is genuinely untouched on this hardware.
//
// GPIO34, 38, 39, 40, 41, 42 are all GNSS-related pins (enable, standby,
// PPS, reset, TX/RX) per variant.h. No GNSS module is physically present,
// but re-verify each after setting `position.gps_mode NOT_PRESENT`, since
// that's what stops firmware from still touching them in software.
#if defined(HELTEC_V4)
static const MultiDetectionGpioConfig gpios[] = {
    {4,  "Sensor 6",  true, true},
    {6,  "Sensor 7",  true, true},
    {15, "Sensor 8",  true, true},
    {16, "Sensor 10", true, true},
    {34, "Sensor 9",  true, true},
    {38, "Sensor 5",  true, true},
    {39, "Sensor 4",  true, true},
    {40, "Sensor 3",  true, true},
    {41, "Sensor 2",  true, true},
    {42, "Sensor 1", true, true},
    //{46, "Sensor 13", true, true}, Removed GPIO46 due to inverted logic on this pin with a hardware pulldown, which is not compatible with the rest of the sensors. It will always read as "ALARM" when the sensor is clear.
    {47, "Sensor 11", true, true},
    {48, "Sensor 12", true, true},
};
#elif defined(HELTEC_V3)
// PLACEHOLDER - do not flash this as-is. NUM_GPIOS is deliberately set to 0
// in the header until V3's safe pin list is confirmed the same rigorous way
// V4's was: check variant.h/pins_arduino.h for LoRa/OLED/battery/Vext pins,
// confirm there's no FEM chip code path to worry about (V3 has none per
// platformio.ini - no HAS_LORA_FEM flag - but verify rather than assume),
// and bench-test each candidate exactly like before. V3 is NOT guaranteed
// to share V4's reserved pins - it's a different board (no FEM, external
// USB-UART bridge instead of native USB) with its own constraints.
static const MultiDetectionGpioConfig gpios[] = {
    // {gpio, "Sensor N", usePullup, activeLow},
};
#endif

static_assert(sizeof(gpios) / sizeof(gpios[0]) == NUM_GPIOS,
              "gpios[] length doesn't match NUM_GPIOS - update the #define in the header");

bool multiDetectionSensorMqttConnected()
{
#if !MESHTASTIC_EXCLUDE_MQTT
    return mqtt && mqtt->isConnectedDirectly();
#else
    return false;
#endif
}

int32_t MultiDetectionSensorModule::runOnce()
{
    if (firstTime) {
        firstTime = false;

        for (uint8_t i = 0; i < NUM_GPIOS; i++) {
            pinMode(gpios[i].gpio, gpios[i].usePullup ? INPUT_PULLUP : INPUT);
            wasDetected[i] = hasDetectionEvent(i); // capture state at boot so we don't
                                                    // fire a false alarm on startup
            LOG_INFO("MultiDetectionSensor: monitoring GPIO%d as '%s'", gpios[i].gpio, gpios[i].name);
        }
        LOG_INFO("MultiDetectionSensor: init, monitoring %d GPIOs, build=%s, mqttConnected=%d",
                  NUM_GPIOS, FARM_BUILD_TAG, multiDetectionSensorMqttConnected());

        // Arrange for the first full-sync cycle below to fire at SYNC_DELAY_MS
        // after boot rather than waiting the full STATE_REFRESH_SECS - this
        // relies on unsigned wraparound arithmetic (a standard, safe pattern
        // for millis()-based timers), not on any special-cased boot logic.
        // After that first fire, lastFullSync gets reset to the real "now"
        // each time, so every subsequent cycle falls on the normal
        // STATE_REFRESH_SECS cadence.
        lastFullSync = millis() + SYNC_DELAY_MS - (STATE_REFRESH_SECS * 1000UL);

        // Note: init runs unconditionally regardless of MQTT status, so GPIO
        // monitoring starts immediately on every node - including shed nodes
        // that never have WiFi/MQTT enabled at all.
        return GPIO_POLLING_INTERVAL;
    }

    for (uint8_t i = 0; i < NUM_GPIOS; i++) {

        bool isDetected = hasDetectionEvent(i);

        //
        // State changed?
        //
        if (!Throttle::isWithinTimespanMs(lastSentToMesh[i], MIN_BROADCAST_SECS * 1000UL)) {
            if (isDetected != wasDetected[i]) {
                wasDetected[i] = isDetected;
                sendDetectionMessage(i, isDetected);
            }
        }
    }

    //
    // Full periodic refresh, recurring forever: first fires SYNC_DELAY_MS
    // after boot (per the lastFullSync trick above), then every
    // STATE_REFRESH_SECS after that.
    //
    if (!Throttle::isWithinTimespanMs(lastFullSync, STATE_REFRESH_SECS * 1000UL)) {
        doFullRefresh();
        lastFullSync = millis();
    }

    return GPIO_POLLING_INTERVAL;
}

void MultiDetectionSensorModule::doFullRefresh()
{
    // Re-sends every pin's current state followed by GPIO_SYNC_COMPLETE.
    // Called both by the periodic timer above and by an on-demand
    // REQUEST_REFRESH broadcast via handleReceived() below - this is what
    // the MQTT bridge uses to detect and clean up sensors that have gone
    // missing for several consecutive cycles in a row (currently tolerating
    // 3, to allow for dropped packets).
    for (uint8_t i = 0; i < NUM_GPIOS; i++) {
        wasDetected[i] = hasDetectionEvent(i);
        sendDetectionMessage(i, wasDetected[i]);
    }
    sendSyncComplete();
}

ProcessMessage MultiDetectionSensorModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    // Ignore our own broadcasts coming back through the local receive path
    // (Meshtastic calls handleReceived for locally-originated packets too,
    // not just ones actually heard over the air) - otherwise a node could
    // end up reacting to its own traffic. Verify the exact "is this from me"
    // accessor against your NodeDB - myNodeInfo.my_node_num / nodeDB->getNodeNum()
    // are the two most likely candidates in this codebase.
    if (mp.from == myNodeInfo.my_node_num) {
        return ProcessMessage::CONTINUE;
    }

    if (mp.decoded.payload.size == strlen(REFRESH_REQUEST_TEXT) &&
        memcmp(mp.decoded.payload.bytes, REFRESH_REQUEST_TEXT, mp.decoded.payload.size) == 0) {
        LOG_INFO("MultiDetectionSensor: refresh requested, responding immediately");
        doFullRefresh();
        lastFullSync = millis(); // don't also fire again immediately via the periodic timer
    }

    return ProcessMessage::CONTINUE;
}

void MultiDetectionSensorModule::sendSyncComplete()
{
    char message[] = "GPIO_SYNC_COMPLETE";
    meshtastic_MeshPacket *p = allocDataPacket();
    p->want_ack = false;
    p->decoded.payload.size = strlen(message);
    memcpy(p->decoded.payload.bytes, message, p->decoded.payload.size);
    service->sendToMesh(p);

    LOG_INFO("MultiDetectionSensor: GPIO sync complete, mqttConnected=%d", multiDetectionSensorMqttConnected());
}

bool MultiDetectionSensorModule::hasDetectionEvent(uint8_t index)
{
    bool raw = digitalRead(gpios[index].gpio);
    return gpios[index].activeLow ? !raw : raw;
}

void MultiDetectionSensorModule::sendDetectionMessage(uint8_t index, bool state)
{
    char message[40];
    snprintf(message, sizeof(message), "%s %s", gpios[index].name, state ? "ALARM" : "clear");

    meshtastic_MeshPacket *p = allocDataPacket();
    p->want_ack = false;
    p->decoded.payload.size = strlen(message);
    memcpy(p->decoded.payload.bytes, message, p->decoded.payload.size);

    lastSentToMesh[index] = millis();

    // Same safety check the stock module uses: refuse to send alarms over the
    // unencrypted public/default channel.
    if (!channels.isDefaultChannel(0)) {
        LOG_INFO("MultiDetectionSensor: send id=%d, msg=%.*s", p->id, p->decoded.payload.size, p->decoded.payload.bytes);
        service->sendToMesh(p);
    } else {
        LOG_ERROR("MultiDetectionSensor: message blocked - not allowed on public/default channel");
    }
}
