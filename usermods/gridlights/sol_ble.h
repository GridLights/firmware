#pragma once

/*
 * sol_ble.h — SolSpektrum BLE Provisioning & WiFi Management Usermod
 *
 * Handles BLE-based WiFi provisioning, WiFi network switching,
 * AP management, factory reset, and boot LED indication.
 */

#ifdef USERMOD_SOL_BLE

#include "wled.h"
#include <esp_wifi.h>
#include <NimBLEDevice.h>
#include <esp_task_wdt.h>
#include <freertos/task.h>  // xTaskGetIdleTaskHandleForCPU

// USER_PRINT macros — defined in WLED-MM but not standard WLED
#ifndef USER_PRINT
  #define USER_PRINT(x)    Serial.print(x)
  #define USER_PRINTLN(x)  Serial.println(x)
  #define USER_PRINTF(...) Serial.printf(__VA_ARGS__)
#endif

// BLE_WAIT_FOR_CONNECTION: Keep BLE alive until a client connects and disconnects
// before cycling back to WiFi. Without this, BLE would restart on a timer.
#define BLE_WAIT_FOR_CONNECTION

// NimBLE deinit mode:
// true  = full heap release (reclaims ~40KB, slightly slower deinit)
// false = stop BLE but keep allocations reserved (faster + safer against WDT)
#define BLE_DEINIT_RELEASE_HEAP true

// State machine tick interval in milliseconds
#define STATE_MACHINE_TICK_MS 500

// Boot blink tuning
#define BOOT_BLINK_DELAY_MS 700
#define BOOT_BLINK_ON_MS 700   // 200ms ramp up + 500ms full-on before ramp-down
#define BOOT_BLINK_BRI 255

// Unified pulse style (boot + provisioning)
#define BLINK_TRANSITION_MS 200
#define BLINK_WHITE_R 255
#define BLINK_WHITE_G 255
#define BLINK_WHITE_B 255

// BLE UUIDs (constants)
#define BLE_SERVICE_UUID "8c8a7301-5b9b-4b1e-ae1a-2e3f6d8c9b5a"
#define BLE_CREDENTIAL_CHAR_UUID "8c8a7302-5b9b-4b1e-ae1a-2e3f6d8c9b5a"

// Global flag for WLED to check before retrying connection
// Set by BLE usermod on permanent auth failures (wrong password, SSID not found)
bool sol_ble_wifiAuthFailure = false;
// Global flag for WLED to pause reconnect logic while LAN WiFi switch test runs
bool sol_ble_wifiSwitchInProgress = false;


class SolBleUsermod : public Usermod {
public:
    static const char _name[];

    void readFromJsonState(JsonObject& root) override {
        JsonObject gl = root["gl"];
        if (gl.isNull()) return;

        bool doFactoryReset = gl["factory_reset"] | false;
        if (doFactoryReset) {
            _factoryResetRequested = true;
            USER_PRINTLN(F("[Sol BLE] WS command received: gl.factory_reset=true"));
        }

    }

    void setup() override {
        _lastToggle = millis();
        _setupTime = millis();

        // Detach core WLED button actions (toggle/random/etc.)
        // Keep direct pin read in this usermod for 3s factory reset hold.
        if (btnPin[0] >= 0) {
            buttonType[0] = BTN_TYPE_NONE;
            pinMode(btnPin[0], INPUT_PULLUP);
            USER_PRINTLN(F("[Sol BLE] Core button actions disabled for button 0"));
        }

        // Force LED output OFF on boot for test workflow
        // (override restored WLED state from previous session)
        if (bri > 0) briLast = bri;
        bri = 0;
        turnOnAtBoot = false;
        stateUpdated(CALL_MODE_INIT);
        _bootOffApplied = true;
        USER_PRINTLN(F("[Sol BLE] Boot policy: LEDs forced OFF"));

        // One-shot boot blink (after boot-off): OFF -> brief ON -> OFF
        _bootBlinkPending = true;
        _bootBlinkOn = false;
        _bootBlinkAt = millis() + BOOT_BLINK_DELAY_MS;
        USER_PRINTLN(F("[Sol BLE] Boot blink scheduled"));
        
        // Reset global WiFi guard flags on boot
        sol_ble_wifiAuthFailure = false;
        sol_ble_wifiSwitchInProgress = false;
        
        // Disable WLED's automatic AP restart - usermod handles AP lifecycle
        apBehavior = AP_BEHAVIOR_BUTTON_ONLY;
        USER_PRINTLN(F("[Sol BLE] Set apBehavior=BUTTON_ONLY (usermod controls AP)"));

        // SOL-50: Brand AP SSID as "<WLED_AP_SSID>-XXXX" (first 4 hex chars of MAC)
        // escapedMac is "AABBCCDDEEFF" — take first 4 chars for short unique suffix
        snprintf(apSSID, sizeof(apSSID), WLED_AP_SSID "-%.4s", escapedMac.c_str());
        strlcpy(apPass, WLED_AP_PASS, sizeof(apPass));
        USER_PRINTF("[Sol BLE] AP SSID: %s\n", apSSID);
        
        // Register WiFi event handler for connection events
        WiFi.onEvent([this](WiFiEvent_t event, WiFiEventInfo_t info) {
            this->handleWiFiEvent(event, info);
        });
        USER_PRINTLN(F("[Sol BLE] WiFi event handler registered"));
        
        // Prevent WLED's handleConnection() from calling initConnection() immediately.
        // initConnection() calls WiFi.disconnect(true) which would destroy our softAP
        // before our loop() has a chance to start it. Setting lastReconnectAttempt here
        // means handleConnection() won't call initConnection() until the 18s timeout,
        // by which time our loop() has already started the AP and set apActive=true.
        lastReconnectAttempt = millis();

        // Start in IDLE state - wait for WLED to attempt WiFi connection
        _state = STATE_IDLE;
        USER_PRINTLN(F("[Sol BLE] Waiting for WLED WiFi connection attempt..."));
    }

    void loop() override {
        // App-triggered factory reset command (WS/JSON)
        if (_factoryResetRequested) {
            _factoryResetRequested = false;
            performFactoryReset("[Sol BLE] Factory reset triggered via WS command");
            return;
        }

        // One-shot boot blink sequence
        if (_bootBlinkPending) {
            if (!_bootBlinkOn && millis() >= _bootBlinkAt) {
                // Save current visual state so blink doesn't overwrite user's stored color/effect
                _savedBri = bri;
                _savedBriLast = briLast;
                for (uint8_t i = 0; i < 4; i++) {
                    _savedCol[i] = col[i];
                    _savedColSec[i] = colSec[i];
                }
                _savedEffectCurrent = effectCurrent;
                _savedEffectSpeed = effectSpeed;
                _savedEffectIntensity = effectIntensity;
                _savedEffectPalette = effectPalette;

                // Force WHITE pulse with fast transition
                applyPulseFrame(BOOT_BLINK_BRI, BLINK_WHITE_R, BLINK_WHITE_G, BLINK_WHITE_B);

                _bootBlinkOn = true;
                _bootBlinkAt = millis() + BOOT_BLINK_ON_MS;
                USER_PRINTLN(F("[Sol BLE] Boot blink ON (white)"));
            } else if (_bootBlinkOn && millis() >= _bootBlinkAt) {
                // Keep WHITE as current color/effect, then turn OFF.
                // This makes subsequent app ON commands come back as white.
                col[0] = BLINK_WHITE_R; col[1] = BLINK_WHITE_G; col[2] = BLINK_WHITE_B; col[3] = 0;
                colSec[0] = BLINK_WHITE_R; colSec[1] = BLINK_WHITE_G; colSec[2] = BLINK_WHITE_B; colSec[3] = 0;
                effectCurrent = FX_MODE_STATIC;
                effectPalette = 0;
                briLast = BOOT_BLINK_BRI;
                bri = 0;
                jsonTransitionOnce = true;
                transitionDelay = BLINK_TRANSITION_MS;
                colorUpdated(CALL_MODE_NO_NOTIFY);
                _bootBlinkPending = false;
                _bootOffReasserted = true; // final OFF state already applied
                USER_PRINTLN(F("[Sol BLE] Boot blink OFF"));
            }
        }

        // One-time reassert in first few seconds in case another boot path turns LEDs back on
        if (_bootOffApplied && !_bootOffReasserted && !_bootBlinkPending && millis() - _setupTime < 3000 && bri > 0) {
            briLast = bri;
            bri = 0;
            stateUpdated(CALL_MODE_INIT);
            _bootOffReasserted = true;
            USER_PRINTLN(F("[Sol BLE] Reasserted OFF state after boot"));
        }

        // Monitor AP connections for mutual exclusion with BLE
        uint8_t stationCount = WiFi.softAPgetStationNum();
        
        // Track when we last saw AP clients
        if (stationCount > 0) {
            _lastApClientActivity = millis();
        }
        
        // AP client connects → stop BLE (mutual exclusion)
        if (_state == STATE_BLE_WAITING && stationCount > 0 && _lastStationCount == 0) {
            USER_PRINTF("[Sol BLE] AP client connecting! Stopping BLE. Heap: %d bytes\n", ESP.getFreeHeap());
            _state = STATE_BLE_STOPPING;
        }
        
        // AP client disconnects → restart BLE if WiFi not connected
        if (stationCount == 0 && _lastStationCount > 0 && !WLED_CONNECTED && 
            (_state == STATE_IDLE || _state == STATE_BLE_STOPPING)) {
            USER_PRINTLN(F("[Sol BLE] AP client disconnected, restarting BLE..."));
            _state = STATE_BLE_STARTING;
        }
        
        _lastStationCount = stationCount;
        
        // Factory reset: hold button for 3s to clear WiFi credentials
        if (btnPin[0] >= 0) {
            if (digitalRead(btnPin[0]) == LOW) {
                if (!_buttonPressed) {
                    _buttonPressStart = millis();
                    _buttonPressed = true;
                    USER_PRINTLN(F("[Sol BLE] Button pressed, hold for 3s to factory reset..."));
                } else if (millis() - _buttonPressStart > 3000) {
                    performFactoryReset("[Sol BLE] Factory reset triggered via button hold");
                }
            } else {
                if (_buttonPressed && millis() - _buttonPressStart < 3000) {
                    USER_PRINTLN(F("[Sol BLE] Button released, reset cancelled"));
                }
                _buttonPressed = false;
            }
        }
        
        // State machine with timing
        if (millis() - _lastToggle > STATE_MACHINE_TICK_MS) {
            _lastToggle = millis();

            // Allow WiFi event handler to request provisioning transition from any state.
            // Keep this in loop() (not event callback) to avoid blocking the WiFi event task.
            if (_enterProvisioningMode && _state != STATE_ENTERING_PROVISIONING) {
                USER_PRINTLN(F("[Sol BLE] Provisioning flag set, transitioning to STATE_ENTERING_PROVISIONING"));
                _provisioningPhase = 0;
                _state = STATE_ENTERING_PROVISIONING;
            }
            
            switch (_state) {
                case STATE_BLE_STARTING: {
                    USER_PRINTLN(F("[Sol BLE] Starting BLE..."));
                    USER_PRINTF("[Sol BLE] Heap before: %d bytes\n", ESP.getFreeHeap());
                    NimBLEDevice::init(serverDescription);  // uses WLED's stored name (default: 'Sol Spektrum - Unconfigured')

                    // Create fresh server/service/characteristic on each BLE start.
                    // This avoids stale GATT handles after scan-driven deinit/reinit cycles.
                    _bleServer = NimBLEDevice::createServer();
                    _bleServer->setCallbacks(new ServerCallbacks(this));
                    _bleServer->advertiseOnDisconnect(false);

                    NimBLEService* service = _bleServer->createService(BLE_SERVICE_UUID);
                    _echoChar = service->createCharacteristic(
                        BLE_CREDENTIAL_CHAR_UUID,
                        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY
                    );
                    _echoChar->setCallbacks(new CharCallbacks(this));
                    service->start();
                    USER_PRINTLN(F("[Sol BLE] BLE server/service created"));

                    // Reset characteristic value to ready on every start
                    String readyJson = "{\"status\":\"ready\"}";
                    _echoChar->setValue((uint8_t*)readyJson.c_str(), readyJson.length());
                    
                    // Start advertising
                    NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
                    pAdvertising->addServiceUUID(BLE_SERVICE_UUID);
                    pAdvertising->start();
                    USER_PRINTF("[Sol BLE] BLE started. Heap: %d bytes\n", ESP.getFreeHeap());
#ifdef BLE_WAIT_FOR_CONNECTION
                    USER_PRINTLN(F("[Sol BLE] Waiting for BLE connection..."));
                    _bleConnected = false;
                    _bleDisconnected = false;
                    _state = STATE_BLE_WAITING;
#else
                    _state = STATE_BLE_STOPPING;
#endif
                    break;
                }
                
                case STATE_BLE_WAITING:
                    // Check for WiFi connection request
                    if (_connectRequested && _bleConnected && !_bleDisconnected) {
                        _connectRequested = false;
                        _wifiConnectStart = millis();
                        _wifiConnecting = true;
                        _authFailure = false;
                        _bootConnection = false; // This is BLE provisioning, not boot
                        _connectionAttempts = 0;
                        sol_ble_wifiAuthFailure = false; // Clear global flag for new connection attempt
                        // Note: WLED's WiFi.setSleep() is disabled via USERMOD_SOL_BLE flag
                        // to prevent pm_set_sleep_type crash during BLE coexistence
                        WiFi.disconnect();
                        delay(100);
                        WiFi.begin(clientSSID, clientPass);
                    }
                    // Check for scan request
                    else if (_scanRequested && _bleConnected && !_bleDisconnected &&
                        _bleServer != nullptr && _connHandle != BLE_HS_CONN_HANDLE_NONE) {
                        USER_PRINTLN(F("[Sol BLE] Disconnecting to perform WiFi scan..."));
                        uint16_t handle = _connHandle;
                        _connHandle = BLE_HS_CONN_HANDLE_NONE;
                        _bleServer->disconnect(handle);
                        // DON'T clear _scanRequested here - need it in STATE_BLE_STOPPING
                    }
                    // Wait for disconnection to complete cycle
                    else if (_bleDisconnected) {
                        USER_PRINTLN(F("[Sol BLE] BLE connect/disconnect cycle complete"));
                        _state = STATE_BLE_STOPPING;
                    }
                    break;
                    
                case STATE_BLE_STOPPING:
                    USER_PRINTLN(F("[Sol BLE] Stopping BLE..."));
                    USER_PRINTF("[Sol BLE] Heap before: %d bytes\n", ESP.getFreeHeap());
                    // NimBLEDevice::deinit() blocks the CPU for hundreds of ms.
                    // The TWDT monitors IDLE0 and IDLE1 by default — they can't run
                    // while we're blocked, so their WDT window expires and the ISR fires
                    // slightly AFTER deinit completes (timing artifact).
                    // Remove all three affected handles before deinit, re-add after.
                    esp_task_wdt_reset();
                    esp_task_wdt_delete(NULL);                            // loop task (if subscribed)
                    esp_task_wdt_delete(xTaskGetIdleTaskHandleForCPU(0)); // IDLE core 0
                    esp_task_wdt_delete(xTaskGetIdleTaskHandleForCPU(1)); // IDLE core 1
                    NimBLEDevice::deinit(BLE_DEINIT_RELEASE_HEAP);
                    // Re-add IDLE tasks — they must be monitored during normal operation.
                    // Do NOT re-add NULL (our task); the loop task isn't normally TWDT-subscribed
                    // and re-adding it would require feeding from every loop() call.
                    esp_task_wdt_add(xTaskGetIdleTaskHandleForCPU(0));
                    esp_task_wdt_add(xTaskGetIdleTaskHandleForCPU(1));
                    esp_task_wdt_reset();
                    // Reset connection state and clear handles.
                    // With BLE_DEINIT_RELEASE_HEAP=true we rebuild GATT on next start.
                    _bleConnected = false;
                    _bleDisconnected = false;
                    _connHandle = BLE_HS_CONN_HANDLE_NONE;
                    _bleConnectTime = 0;
                    _bleServer = nullptr;
                    _echoChar = nullptr;
                    
                    // Perform WiFi scan if requested
                    if (_scanRequested) {
                        _scanRequested = false;
                        performWifiScan(true); // true = cache results
                        _scanState = SCAN_RESULTS_READY;
                    } else {
                        _scanRequested = false;
                        _connectRequested = false; // Clear if still set
                    }
                    
                    USER_PRINTF("[Sol BLE] BLE stopped. Heap: %d bytes\n", ESP.getFreeHeap());
                    
                    // Only restart BLE if WiFi not connected AND no AP clients
                    if (WLED_CONNECTED) {
                        USER_PRINTLN(F("[Sol BLE] WiFi connected, BLE stays off"));
                        
                        // Initialize heavy services now that BLE is stopped and heap is freed
                        USER_PRINTLN(F("[Sol BLE] Initializing heavy services (UDP, E1.31, MQTT)..."));
                        if (udpPort > 0 && udpPort != ntpLocalPort) {
                            udpConnected = notifierUdp.begin(udpPort);
                            if (udpConnected && udpRgbPort != udpPort)
                                udpRgbConnected = rgbUdp.begin(udpRgbPort);
                            if (udpConnected && udpPort2 != udpPort && udpPort2 != udpRgbPort)
                                udp2Connected = notifier2Udp.begin(udpPort2);
                        }
                        if (ntpEnabled)
                            ntpConnected = ntpUdp.begin(ntpLocalPort);
                        
                        e131.begin(e131Multicast, e131Port, e131Universe, E131_MAX_UNIVERSE_COUNT);
                        ddp.begin(false, DDP_DEFAULT_PORT);
                        reconnectHue();
                        #ifndef WLED_DISABLE_MQTT
                        initMqtt();
                        #endif
                        USER_PRINTF("[Sol BLE] Heavy services started. Heap: %d bytes\n", ESP.getFreeHeap());
                        
                        _state = STATE_IDLE;
                    } else if (WiFi.softAPgetStationNum() == 0) {
                        USER_PRINTLN(F("[Sol BLE] WiFi not connected, restarting BLE..."));
                        _state = STATE_BLE_STARTING;
                    } else {
                        USER_PRINTLN(F("[Sol BLE] AP client still connected, BLE stays off"));
                        _state = STATE_IDLE;
                    }
                    break;
                    
                case STATE_ENTERING_PROVISIONING:
                    // Non-blocking provisioning entry coordinated by events
                    switch (_provisioningPhase) {
                        case 0: // Initial - trigger disconnect
                            USER_PRINTLN(F("[Sol BLE] Phase 0: Disconnecting WiFi..."));
                            _wifiStopped = false;
                            WiFi.disconnect(true);
                            _provisioningPhase = 1;
                            _provisioningPhaseStart = millis();
                            break;
                            
                        case 1: // Waiting for STA_STOP event
                            if (_wifiStopped) {
                                USER_PRINTLN(F("[Sol BLE] Phase 1: STA_STOP received, changing to AP_STA mode..."));
                                _wifiStopped = true; // Keep true to detect START
                                WiFi.mode(WIFI_AP_STA);
                                _provisioningPhase = 2;
                                _provisioningPhaseStart = millis();
                            } else if (millis() - _provisioningPhaseStart > 5000) {
                                USER_PRINTLN(F("[Sol BLE] Phase 1: Timeout waiting for STA_STOP, proceeding anyway"));
                                WiFi.mode(WIFI_AP_STA);
                                _provisioningPhase = 2;
                                _provisioningPhaseStart = millis();
                            }
                            break;
                            
                        case 2: // Waiting for STA_START event
                            if (!_wifiStopped) {
                                USER_PRINTLN(F("[Sol BLE] Phase 2: STA_START received, WiFi ready for scan"));
                                _provisioningPhase = 3;
                            } else if (millis() - _provisioningPhaseStart > 3000) {
                                USER_PRINTLN(F("[Sol BLE] Phase 2: Timeout waiting for STA_START, proceeding anyway"));
                                _provisioningPhase = 3;
                            }
                            break;
                            
                        case 3: // Ready - perform scan and start services
                            USER_PRINTLN(F("[Sol BLE] Phase 3: Starting provisioning services..."));
                            
                            // Perform WiFi scan
                            performWifiScan(true);
                            
                            // Start AP
                            USER_PRINTLN(F("[Sol BLE] Starting AP..."));
                            WiFi.softAPConfig(IPAddress(4, 3, 2, 1), IPAddress(4, 3, 2, 1), IPAddress(255, 255, 255, 0));
                            WiFi.softAP(apSSID, apPass, apChannel, apHide);
                            
                            if (!apActive) {
                                server.begin();
                                dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
                                dnsServer.start(53, "*", WiFi.softAPIP());
                                apActive = true;
                            }
                            
                            USER_PRINTF("[Sol BLE] AP started. Heap: %d bytes\n", ESP.getFreeHeap());
                            
                            // Start BLE for provisioning
                            _provisioningStarted = true;
                            _enterProvisioningMode = false;
                            _state = STATE_BLE_STARTING;
                            USER_PRINTLN(F("[Sol BLE] Provisioning mode ready, starting BLE..."));
                            break;
                    }
                    break;
                    
                case STATE_IDLE:
                    // Check immediately if WiFi is configured or if connection failed
                    if (!_provisioningStarted) {
                        // Check if WiFi credentials are configured
                        bool wifiConfigured = WLED_WIFI_CONFIGURED;
                        
                        if (!wifiConfigured) {
                            // No WiFi configured - start provisioning immediately
                            USER_PRINTLN(F("[Sol BLE] No WiFi configured - starting provisioning"));
                            
                            // Perform WiFi scan (function prints its own message)
                            performWifiScan(true);
                            USER_PRINTF("[Sol BLE] Scan complete, cached %d bytes\n", _scanResults.length());
                            
                            // Start AP
                            USER_PRINTLN(F("[Sol BLE] Starting AP..."));
                            WiFi.softAPConfig(IPAddress(4, 3, 2, 1), IPAddress(4, 3, 2, 1), IPAddress(255, 255, 255, 0));
                            WiFi.softAP(apSSID, apPass, apChannel, apHide);
                            
                            if (!apActive) {
                                server.begin();
                                dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
                                dnsServer.start(53, "*", WiFi.softAPIP());
                                apActive = true;
                            }
                            
                            USER_PRINTF("[Sol BLE] AP started. Heap: %d bytes\n", ESP.getFreeHeap());
                            
                            _provisioningStarted = true;
                            _state = STATE_BLE_STARTING;
                            USER_PRINTLN(F("[Sol BLE] Entering AP+BLE provisioning mode"));
                        }
                        // If WiFi is configured, connected() callback will set _provisioningStarted
                    }
                    break;
            }
        }

        // ---- Deferred config save (from /gl/rename) ----
        // serializeConfig() blocks ~200ms on LittleFS; calling it from the
        // AsyncTCP task trips the WDT. Handler sets this flag; we flush here.
        if (_pendingRename) {
            _pendingRename = false;
            serializeConfig();
            USER_PRINTF("[Sol BLE] Device rename saved: %s\n", serverDescription);
        }

        // ---- WiFi switch state machine (LAN reprovision test flow) ----
        if (_wifiSwitchPending && !_wifiSwitchInProgress && millis() >= _wifiSwitchStartAt) {
            _wifiSwitchPending = false;
            _wifiSwitchInProgress = true;
            sol_ble_wifiSwitchInProgress = true;
            _wifiSwitchPhase = WIFI_SWITCH_WAIT_NEW;
            _wifiSwitchPhaseStart = millis();
            _wifiSwitchNewConnectOk = false;

            setWifiSwitchStatus("connecting_new", "Connecting to new network...");
            USER_PRINTF("[WiFi Switch] Trying new SSID: %s\n", _wifiSwitchNewSSID);
            WiFi.disconnect();
            delay(80);
            WiFi.begin(_wifiSwitchNewSSID, _wifiSwitchNewPass);
        }

        if (_wifiSwitchInProgress) {
            if (_wifiSwitchPhase == WIFI_SWITCH_WAIT_NEW) {
                if (WiFi.status() == WL_CONNECTED) {
                    String activeSsid = WiFi.SSID();
                    if (activeSsid.equals(String(_wifiSwitchNewSSID))) {
                        _wifiSwitchNewConnectOk = true;
                        _wifiSwitchNewIp = WiFi.localIP();
                        _wifiSwitchNewMdns = String(cmDNS) + ".local";

                        USER_PRINTF("[WiFi Switch] New network connected: %s (%s)\n", _wifiSwitchNewSSID, _wifiSwitchNewIp.toString().c_str());
                        USER_PRINTF("[WiFi Switch] New network mDNS: %s\n", _wifiSwitchNewMdns.c_str());
                        setWifiSwitchStatus("reconnecting_old", "New network works. Reconnecting old network...");
                        _wifiSwitchPhase = WIFI_SWITCH_WAIT_OLD;
                        _wifiSwitchPhaseStart = millis();
                        WiFi.disconnect();
                        delay(80);
                        WiFi.begin(_wifiSwitchOldSSID, _wifiSwitchOldPass);
                    } else {
                        USER_PRINTF("[WiFi Switch] Connected to unexpected SSID '%s' while waiting for '%s'\n", activeSsid.c_str(), _wifiSwitchNewSSID);
                    }
                } else if (millis() - _wifiSwitchPhaseStart > WIFI_SWITCH_TIMEOUT_MS) {
                    USER_PRINTF("[WiFi Switch] New network connect timeout for SSID: %s\n", _wifiSwitchNewSSID);
                    _wifiSwitchNewConnectOk = false;
                    setWifiSwitchStatus("reconnecting_old", "Could not connect new network. Reconnecting old network...");
                    _wifiSwitchPhase = WIFI_SWITCH_WAIT_OLD;
                    _wifiSwitchPhaseStart = millis();
                    WiFi.disconnect();
                    delay(80);
                    WiFi.begin(_wifiSwitchOldSSID, _wifiSwitchOldPass);
                }
            } else if (_wifiSwitchPhase == WIFI_SWITCH_WAIT_OLD) {
                if (WiFi.status() == WL_CONNECTED) {
                    String activeSsid = WiFi.SSID();
                    if (activeSsid.equals(String(_wifiSwitchOldSSID))) {
                        _wifiSwitchOldIp = WiFi.localIP();
                        if (_wifiSwitchNewConnectOk) {
                            _wifiSwitchCommitPending = true;
                            _wifiSwitchCommitAck = false;
                            _wifiSwitchCommitDeadline = millis() + WIFI_SWITCH_COMMIT_TIMEOUT_MS;
                            setWifiSwitchStatus("success", "New network validated. Waiting app confirmation to switch permanently.");
                        } else {
                            setWifiSwitchStatus("error", "Could not connect to new network. Restored old network.", "connect_new_failed");
                        }
                        USER_PRINTF("[WiFi Switch] Old network restored: %s\n", _wifiSwitchOldSSID);
                        USER_PRINTF("[WiFi Switch] Old network mDNS: %s.local\n", cmDNS);
                        _wifiSwitchInProgress = false;
                        sol_ble_wifiSwitchInProgress = false;
                        _wifiSwitchPhase = WIFI_SWITCH_IDLE;
                    } else {
                        USER_PRINTF("[WiFi Switch] Waiting old SSID '%s', currently '%s'\n", _wifiSwitchOldSSID, activeSsid.c_str());
                    }
                } else if (millis() - _wifiSwitchPhaseStart > WIFI_SWITCH_TIMEOUT_MS) {
                    setWifiSwitchStatus("old_reconnect_failed", "Could not reconnect old network.", "old_reconnect_failed");
                    USER_PRINTLN(F("[WiFi Switch] Failed to reconnect old network"));
                    _wifiSwitchInProgress = false;
                    sol_ble_wifiSwitchInProgress = false;
                    _wifiSwitchPhase = WIFI_SWITCH_IDLE;
                }
            }
        }

        // Finalize WiFi switch (persist new creds + leave old network) after app ACK or timeout.
        if (_wifiSwitchCommitPending && (_wifiSwitchCommitAck || millis() >= _wifiSwitchCommitDeadline)) {
            if (_wifiSwitchCommitAck) {
                USER_PRINTLN(F("[WiFi Switch] Commit ACK received from app - applying new network"));
            } else {
                USER_PRINTLN(F("[WiFi Switch] Commit timeout - applying new network anyway"));
            }

            _wifiSwitchCommitPending = false;
            _wifiSwitchCommitAck = false;
            setWifiSwitchStatus("applying_new", "Applying new network credentials...");

            // Persist new WiFi credentials and switch permanently.
            strlcpy(clientSSID, _wifiSwitchNewSSID, sizeof(clientSSID));
            strlcpy(clientPass, _wifiSwitchNewPass, sizeof(clientPass));
            serializeConfig();

            WiFi.disconnect();
            delay(80);
            WiFi.begin(_wifiSwitchNewSSID, _wifiSwitchNewPass);
        }
    } // end loop()

    void connected() override {
        // Called when WiFi successfully connects (boot or runtime)
        USER_PRINTF("[Sol BLE] connected() callback - WiFi connected to: %s\n", clientSSID);
        _provisioningStarted = true; // Mark that WiFi is working, don't start provisioning
        _bootConnection = false;
        _connectionAttempts = 0;
        sol_ble_wifiAuthFailure = false; // Clear global flag on successful connection

        // Register /gl/rename endpoint once — lightweight device rename for post-provisioning.
        // Bypasses WLED's large JSON machinery; safe to call when heap is fragmented.
        if (!_renameRegistered) {
            _renameRegistered = true;
            static char _renameBody[64];
            static size_t _renameBodyLen = 0;
            server.on("/gl/rename", HTTP_POST,
                [this](AsyncWebServerRequest* request) {
                    _renameBody[_renameBodyLen] = '\0';
                    char* name = _renameBody;
                    while (*name == ' ' || *name == '\n' || *name == '\r') name++;
                    size_t nlen = strlen(name);
                    while (nlen > 0 && (name[nlen-1] == ' ' || name[nlen-1] == '\n' || name[nlen-1] == '\r'))
                        name[--nlen] = '\0';
                    if (nlen == 0 || nlen > 32) {
                        // Notify app over WS so UI can show explicit failure feedback.
                        DynamicJsonDocument doc(192);
                        JsonObject gl = doc.createNestedObject("gl");
                        gl["event"] = "rename_result";
                        gl["ok"] = false;
                        gl["error"] = "name must be 1-32 chars";
                        String wsMsg;
                        serializeJson(doc, wsMsg);
                        ws.textAll(wsMsg);
                        request->send(400, "application/json", "{\"error\":\"name must be 1-32 chars\"}");
                        return;
                    }
                    strlcpy(serverDescription, name, 33);
                    USER_PRINTF("[Sol BLE] Device renamed to: %s (save deferred)\n", serverDescription);
                    _pendingRename = true;

                    // Notify app over WS so Settings can show success confirmation.
                    DynamicJsonDocument doc(192);
                    JsonObject gl = doc.createNestedObject("gl");
                    gl["event"] = "rename_result";
                    gl["ok"] = true;
                    gl["name"] = serverDescription;
                    String wsMsg;
                    serializeJson(doc, wsMsg);
                    ws.textAll(wsMsg);

                    request->send(200, "application/json", "{\"ok\":true}");
                },
                nullptr,
                [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
                    if (index == 0) _renameBodyLen = 0;
                    size_t copy = min(len, sizeof(_renameBody) - _renameBodyLen - 1);
                    memcpy(_renameBody + _renameBodyLen, data, copy);
                    _renameBodyLen += copy;
                });
        }

        // Register WiFi scan endpoints once — used by app-side WiFi reprovision modal.
        // /gl/wifi/scan_cached: returns last cached scan result if present.
        // /gl/wifi/scan: triggers fresh scan and returns compact network JSON.
        if (!_wifiScanRegistered) {
            _wifiScanRegistered = true;

            server.on("/gl/wifi/scan_cached", HTTP_GET,
                [this](AsyncWebServerRequest* request) {
                    if (_scanResults.isEmpty()) {
                        request->send(200, "application/json", "{\"status\":\"ready\",\"networks\":[]}");
                        return;
                    }
                    request->send(200, "application/json", _scanResults);
                }
            );

            server.on("/gl/wifi/scan", HTTP_POST,
                [this](AsyncWebServerRequest* request) {
                    performWifiScan(true);
                    if (_scanResults.isEmpty()) {
                        request->send(200, "application/json", "{\"status\":\"ready\",\"networks\":[]}");
                        return;
                    }
                    request->send(200, "application/json", _scanResults);
                }
            );
        }

        // Register WiFi switch endpoints once — used by WiFi modal over LAN.
        if (!_wifiSwitchRegistered) {
            _wifiSwitchRegistered = true;
            static char _wifiSwitchBody[256];
            static size_t _wifiSwitchBodyLen = 0;

            server.on("/gl/wifi/switch_status", HTTP_GET,
                [this](AsyncWebServerRequest* request) {
                    USER_PRINTF("[WiFi Switch] /gl/wifi/switch_status -> %s\n", _wifiSwitchStatus.c_str());
                    request->send(200, "application/json", _wifiSwitchStatus);
                }
            );

            server.on("/gl/wifi/switch", HTTP_POST,
                [this](AsyncWebServerRequest* request) {
                    _wifiSwitchBody[_wifiSwitchBodyLen] = '\0';

                    DynamicJsonDocument doc(256);
                    DeserializationError err = deserializeJson(doc, _wifiSwitchBody);
                    if (err) {
                        request->send(400, "application/json", "{\"status\":\"error\",\"reason\":\"bad_json\"}");
                        return;
                    }

                    String ssid = doc["ssid"] | "";
                    String psk = doc["psk"] | "";
                    ssid.trim();
                    psk.trim();

                    if (ssid.length() == 0 || ssid.length() > 32 || psk.length() > 64) {
                        request->send(400, "application/json", "{\"status\":\"error\",\"reason\":\"invalid_credentials\"}");
                        return;
                    }

                    if (_wifiSwitchInProgress || _wifiSwitchPending) {
                        request->send(409, "application/json", "{\"status\":\"busy\"}");
                        return;
                    }

                    if (WLED_CONNECTED && ssid.equals(String(clientSSID))) {
                        setWifiSwitchStatus("already_connected", "Device is already connected to this network.");
                        request->send(200, "application/json", _wifiSwitchStatus);
                        return;
                    }

                    strlcpy(_wifiSwitchOldSSID, clientSSID, sizeof(_wifiSwitchOldSSID));
                    strlcpy(_wifiSwitchOldPass, clientPass, sizeof(_wifiSwitchOldPass));
                    _wifiSwitchOldIp = IPAddress();
                    strlcpy(_wifiSwitchNewSSID, ssid.c_str(), sizeof(_wifiSwitchNewSSID));
                    strlcpy(_wifiSwitchNewPass, psk.c_str(), sizeof(_wifiSwitchNewPass));
                    _wifiSwitchNewIp = IPAddress();
                    _wifiSwitchNewMdns = "";

                    _wifiSwitchPending = true;
                    _wifiSwitchStartAt = millis() + WIFI_SWITCH_START_DELAY_MS;
                    setWifiSwitchStatus("pending", "WiFi switch request accepted.");
                    request->send(202, "application/json", _wifiSwitchStatus);
                },
                nullptr,
                [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
                    if (index == 0) _wifiSwitchBodyLen = 0;
                    size_t copy = min(len, sizeof(_wifiSwitchBody) - _wifiSwitchBodyLen - 1);
                    memcpy(_wifiSwitchBody + _wifiSwitchBodyLen, data, copy);
                    _wifiSwitchBodyLen += copy;
                }
            );

            server.on("/gl/wifi/switch_commit", HTTP_POST,
                [this](AsyncWebServerRequest* request) {
                    if (!_wifiSwitchCommitPending) {
                        USER_PRINTLN(F("[WiFi Switch] /gl/wifi/switch_commit rejected: no pending commit"));
                        request->send(409, "application/json", "{\"status\":\"no_pending_commit\"}");
                        return;
                    }

                    _wifiSwitchCommitAck = true;
                    USER_PRINTLN(F("[WiFi Switch] /gl/wifi/switch_commit ACK received"));
                    request->send(202, "application/json", "{\"status\":\"commit_acknowledged\"}");
                }
            );
        }
    }

    uint16_t getId() override { return USERMOD_ID_UNSPECIFIED; }

private:
    // Forward declarations for inner classes
    class ServerCallbacks;
    class CharCallbacks;

    // State machine enums
    enum State {
        STATE_IDLE,
        STATE_BLE_STARTING,
        STATE_BLE_WAITING,
        STATE_BLE_STOPPING,
        STATE_ENTERING_PROVISIONING
    };
    
    enum ScanState {
        SCAN_IDLE,
        SCAN_RESULTS_READY
    };

    enum WifiSwitchPhase {
        WIFI_SWITCH_IDLE,
        WIFI_SWITCH_WAIT_NEW,
        WIFI_SWITCH_WAIT_OLD
    };
    
    // Member variables
    uint32_t _lastToggle = 0;
    uint32_t _setupTime = 0;
    bool _provisioningStarted = false;
    State _state;
    ScanState _scanState = SCAN_IDLE;
    NimBLEServer* _bleServer = nullptr;
    NimBLECharacteristic* _echoChar = nullptr;
    bool _bleConnected = false;
    bool _bleDisconnected = false;
    uint32_t _bleConnectTime = 0;
    uint16_t _connHandle = BLE_HS_CONN_HANDLE_NONE;
    bool _scanRequested = false;
    bool _connectRequested = false;
    bool _wifiConnecting = false;
    uint32_t _wifiConnectStart = 0;
    bool _authFailure = false;
    bool _bootConnection = false; // Track if this is boot-time connection vs BLE provisioning
    uint8_t _connectionAttempts = 0; // Count connection attempts to prevent infinite retries
    bool _wifiStopped = false; // Track STA_STOP event for provisioning transition
    bool _enterProvisioningMode = false; // Flag to enter provisioning (set by event handler)
    uint32_t _provisioningPhaseStart = 0; // Timing for provisioning phases
    uint8_t _provisioningPhase = 0; // 0=initial, 1=waiting_for_stop, 2=waiting_for_start, 3=ready
    bool _buttonPressed = false;
    uint32_t _buttonPressStart = 0;
    uint8_t _lastStationCount = 0;
    uint32_t _lastApClientActivity = 0;
    String _scanResults = "";
    bool _factoryResetRequested = false;
    bool _bootOffApplied = false;
    bool _bootOffReasserted = false;
    bool _bootBlinkPending = false;
    bool _bootBlinkOn = false;
    uint32_t _bootBlinkAt = 0;
    byte _savedBri = 0;
    byte _savedBriLast = 0;
    byte _savedCol[4] = {0, 0, 0, 0};
    byte _savedColSec[4] = {0, 0, 0, 0};
    byte _savedEffectCurrent = 0;
    byte _savedEffectSpeed = 0;
    byte _savedEffectIntensity = 0;
    byte _savedEffectPalette = 0;
    // Deferred rename — set by /gl/rename handler, flushed in loop() on main task
    bool _renameRegistered = false;
    bool _wifiScanRegistered = false;
    bool _wifiSwitchRegistered = false;
    volatile bool _pendingRename = false;

    // WiFi switch over LAN (test new creds, then reconnect old network)
    bool _wifiSwitchPending = false;
    bool _wifiSwitchInProgress = false;
    bool _wifiSwitchNewConnectOk = false;
    bool _wifiSwitchCommitPending = false;
    bool _wifiSwitchCommitAck = false;
    WifiSwitchPhase _wifiSwitchPhase = WIFI_SWITCH_IDLE;
    uint32_t _wifiSwitchStartAt = 0;
    uint32_t _wifiSwitchPhaseStart = 0;
    uint32_t _wifiSwitchCommitDeadline = 0;
    char _wifiSwitchOldSSID[33] = {0};
    char _wifiSwitchOldPass[65] = {0};
    IPAddress _wifiSwitchOldIp = IPAddress();
    char _wifiSwitchNewSSID[33] = {0};
    char _wifiSwitchNewPass[65] = {0};
    IPAddress _wifiSwitchNewIp = IPAddress();
    String _wifiSwitchNewMdns = "";
    String _wifiSwitchStatus = "{\"status\":\"idle\"}";
    static const uint32_t WIFI_SWITCH_START_DELAY_MS = 250;
    static const uint32_t WIFI_SWITCH_TIMEOUT_MS = 12000;
    static const uint32_t WIFI_SWITCH_COMMIT_TIMEOUT_MS = 20000;

    void applyPulseFrame(byte targetBri, byte r, byte g, byte b) {
        col[0] = r; col[1] = g; col[2] = b; col[3] = 0;
        colSec[0] = r; colSec[1] = g; colSec[2] = b; colSec[3] = 0;
        effectCurrent = FX_MODE_STATIC;
        bri = targetBri;
        jsonTransitionOnce = true;
        transitionDelay = BLINK_TRANSITION_MS;
        colorUpdated(CALL_MODE_NO_NOTIFY);
    }

    void setWifiSwitchStatus(const char* status, const char* message, const char* reason = nullptr) {
        DynamicJsonDocument doc(384);
        doc["status"] = status;
        if (message) doc["message"] = message;
        if (reason) doc["reason"] = reason;
        if (strcmp(status, "success") == 0) {
            doc["new_ssid"] = _wifiSwitchNewSSID;
            doc["new_ip"] = _wifiSwitchNewIp.toString();
            doc["new_mdns"] = _wifiSwitchNewMdns;
            doc["old_ssid"] = _wifiSwitchOldSSID;
            doc["old_ip"] = _wifiSwitchOldIp.toString();
            doc["commit_required"] = _wifiSwitchCommitPending;
        }

        String payload;
        serializeJson(doc, payload);
        _wifiSwitchStatus = payload;
        USER_PRINTF("[WiFi Switch] Status update: %s\n", _wifiSwitchStatus.c_str());

        // Also broadcast over WS for any active app listener.
        DynamicJsonDocument ev(448);
        JsonObject gl = ev.createNestedObject("gl");
        gl["event"] = "wifi_switch";
        gl["status"] = status;
        if (message) gl["message"] = message;
        if (reason) gl["reason"] = reason;
        if (strcmp(status, "success") == 0) {
            gl["new_ssid"] = _wifiSwitchNewSSID;
            gl["new_ip"] = _wifiSwitchNewIp.toString();
            gl["new_mdns"] = _wifiSwitchNewMdns;
            gl["old_ssid"] = _wifiSwitchOldSSID;
            gl["old_ip"] = _wifiSwitchOldIp.toString();
            gl["commit_required"] = _wifiSwitchCommitPending;
        }
        String wsPayload;
        serializeJson(ev, wsPayload);
        ws.textAll(wsPayload);
    }

    void performFactoryReset(const char* sourceMsg) {
        USER_PRINTLN(sourceMsg);
        USER_PRINTLN(F("[Sol BLE] Clearing WiFi credentials and resetting device name..."));

        // Match existing button-reset behavior exactly
        strcpy(clientSSID, "Your_Network");
        memset(clientPass, 0, 65);
        // Reset device name to default so BLE advertises "Sol Spektrum - Unconfigured" again
        strlcpy(serverDescription, "Sol Spektrum - Unconfigured", 33);
        // SOL-50: Re-apply branded AP credentials so cfg.json doesn't keep stale values
        snprintf(apSSID, sizeof(apSSID), WLED_AP_SSID "-%.4s", escapedMac.c_str());
        strlcpy(apPass, WLED_AP_PASS, sizeof(apPass));
        serializeConfig();
        WiFi.disconnect(true, true);

        delay(500);
        USER_PRINTLN(F("[Sol BLE] Rebooting..."));
        ESP.restart();
    }

    // WiFi event handler - responds immediately to connection events
    // Handles BOTH boot-time connection attempts AND BLE provisioning attempts
    void handleWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
        switch (event) {
            case SYSTEM_EVENT_STA_START:
                USER_PRINTLN(F("[Sol BLE] WiFi event: STA_START"));
                _wifiStopped = false;
                break;
                
            case SYSTEM_EVENT_STA_STOP:
                USER_PRINTLN(F("[Sol BLE] WiFi event: STA_STOP"));
                _wifiStopped = true;
                break;
                
            case SYSTEM_EVENT_STA_CONNECTED:
                USER_PRINTF("[Sol BLE] WiFi event: STA_CONNECTED to SSID: %s\n", WiFi.SSID().c_str());
                _connectionAttempts = 0; // Reset attempt counter on successful connect
                // Wait for IP assignment before declaring success
                break;
                
            case SYSTEM_EVENT_STA_GOT_IP:
                USER_PRINTLN(F("[Sol BLE] WiFi event: GOT_IP - Connection successful!"));
                USER_PRINTF("[Sol BLE] IP address: %s\n", WiFi.localIP().toString().c_str());
                USER_PRINTF("[Sol BLE] mDNS: %s.local\n", cmDNS);

                if (_wifiSwitchInProgress) {
                    USER_PRINTLN(F("[Sol BLE] WiFi switch in progress: skipping normal GOT_IP flow"));
                    break;
                }
                
                _wifiConnecting = false;
                _authFailure = false;
                _bootConnection = false;
                _connectionAttempts = 0;
                
                // Send success notification if BLE provisioning active
                if (_echoChar && _bleConnected) {
                    String hostname = String(cmDNS);
                    String response = "{\"status\":\"connected\",\"ip\":\"" + WiFi.localIP().toString() + 
                                      "\",\"mdns\":\"" + hostname + ".local\"}";
                    _echoChar->setValue((uint8_t*)response.c_str(), response.length());
                    _echoChar->notify();
                    USER_PRINTLN(F("[Sol BLE] ===== NOTIFICATION SENT TO APP ====="));
                    USER_PRINTF("[Sol BLE] %s\n", response.c_str());
                    USER_PRINTLN(F("[Sol BLE] ====================================="));
                }
                
                // Stop BLE to free heap (BLE provisioning path)
                if (_state == STATE_BLE_WAITING) {
                    USER_PRINTLN(F("[Sol BLE] Stopping BLE to free heap after successful connection"));
                    _state = STATE_BLE_STOPPING;
                } else if (_state != STATE_BLE_STOPPING) {
                    // Normal boot path: BLE was never started, start heavy services now
                    USER_PRINTLN(F("[Sol BLE] Normal boot WiFi connect - starting heavy services"));
                    if (udpPort > 0 && udpPort != ntpLocalPort) {
                        udpConnected = notifierUdp.begin(udpPort);
                        if (udpConnected && udpRgbPort != udpPort)
                            udpRgbConnected = rgbUdp.begin(udpRgbPort);
                        if (udpConnected && udpPort2 != udpPort && udpPort2 != udpRgbPort)
                            udp2Connected = notifier2Udp.begin(udpPort2);
                    }
                    if (ntpEnabled)
                        ntpConnected = ntpUdp.begin(ntpLocalPort);
                    e131.begin(e131Multicast, e131Port, e131Universe, E131_MAX_UNIVERSE_COUNT);
                    ddp.begin(false, DDP_DEFAULT_PORT);
                    reconnectHue();
                    #ifndef WLED_DISABLE_MQTT
                    initMqtt();
                    #endif
                    USER_PRINTF("[Sol BLE] Heavy services started. Heap: %d bytes\n", ESP.getFreeHeap());
                }
                break;
                
            case SYSTEM_EVENT_STA_DISCONNECTED: {
                wifi_err_reason_t reason = (wifi_err_reason_t)info.disconnected.reason;

                if (_wifiSwitchInProgress) {
                    USER_PRINTF("[Sol BLE] WiFi switch in progress: ignoring disconnect reason %d\n", reason);
                    break;
                }
                
                USER_PRINTLN(F("[Sol BLE] ========================================"));
                USER_PRINTF("[Sol BLE] WiFi DISCONNECTED event (reason code: %d)\n", reason);
                
                // Increment attempt counter
                _connectionAttempts++;
                USER_PRINTF("[Sol BLE] Connection attempt #%d\n", _connectionAttempts);
                
                // Determine failure type and reason string
                bool isPermanentFailure = false;
                const char* reasonStr = "timeout";
                const char* reasonDesc = "Unknown failure";
                
                switch (reason) {
                    case WIFI_REASON_AUTH_FAIL:
                        reasonStr = "wrong_password";
                        reasonDesc = "Authentication failed - WRONG PASSWORD";
                        isPermanentFailure = true;
                        break;
                    case WIFI_REASON_AUTH_EXPIRE:
                        reasonStr = "wrong_password";
                        reasonDesc = "Authentication expired";
                        isPermanentFailure = true;
                        break;
                    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
                        reasonStr = "wrong_password";
                        reasonDesc = "4-way handshake timeout - WRONG PASSWORD";
                        isPermanentFailure = true;
                        break;
                    case WIFI_REASON_HANDSHAKE_TIMEOUT:
                        reasonStr = "wrong_password";
                        reasonDesc = "Handshake timeout - likely WRONG PASSWORD";
                        isPermanentFailure = true;
                        break;
                    case WIFI_REASON_NO_AP_FOUND:
                        reasonStr = "not_found";
                        reasonDesc = "Network NOT FOUND (SSID not in range)";
                        isPermanentFailure = true;
                        break;
                    case WIFI_REASON_BEACON_TIMEOUT:
                        reasonStr = "not_found";
                        reasonDesc = "Beacon timeout - network disappeared";
                        isPermanentFailure = true;
                        break;
                    case WIFI_REASON_ASSOC_LEAVE:
                        // ASSOC_LEAVE during connection is just WiFi.begin() cleanup, not an error
                        if (_wifiConnecting && (millis() - _wifiConnectStart < 3000)) {
                            USER_PRINTLN(F("[Sol BLE] Ignoring ASSOC_LEAVE during connection setup"));
                            USER_PRINTLN(F("[Sol BLE] ========================================"));
                            return; // Ignore - wait for real connection result
                        }
                        // Otherwise treat as graceful disconnect
                        reasonDesc = "Graceful disconnect (user-initiated)";
                        USER_PRINTF("[Sol BLE] %s\n", reasonDesc);
                        USER_PRINTLN(F("[Sol BLE] ========================================"));
                        return; // Don't treat as error
                    default:
                        reasonDesc = "Connection failed";
                        break;
                }
                
                USER_PRINTF("[Sol BLE] Failure reason: %s\n", reasonDesc);
                
                // STOP RECONNECTION ATTEMPTS IMMEDIATELY
                USER_PRINTLN(F("[Sol BLE] Calling WiFi.disconnect(true) to stop reconnection..."));
                WiFi.disconnect(true);
                
                if (isPermanentFailure) {
                    USER_PRINTLN(F("[Sol BLE] PERMANENT FAILURE DETECTED - will not retry!"));
                    _authFailure = true;
                    _wifiConnecting = false;
                    
                    // Set global flag to block WLED's reconnection attempts
                    sol_ble_wifiAuthFailure = true;
                    USER_PRINTLN(F("[Sol BLE] Set sol_ble_wifiAuthFailure=true to block WLED reconnection"));
                    
                    // Enter provisioning mode if:
                    // 1. Not in BLE provisioning state (must be boot/WLED retry)
                    // 2. Multiple failures from BLE provisioning (3+ attempts)
                    bool isBootFailure = (_state != STATE_BLE_WAITING);
                    bool tooManyAttempts = (_connectionAttempts >= 3);
                    
                    if (isBootFailure || tooManyAttempts) {
                        if (isBootFailure) {
                            USER_PRINTLN(F("[Sol BLE] Boot connection failed - entering provisioning mode"));
                        } else {
                            USER_PRINTF("[Sol BLE] Too many attempts (%d) - entering provisioning mode\n", _connectionAttempts);
                        }
                        USER_PRINTLN(F("[Sol BLE] Could not connect to WiFi - entering provisioning mode"));
                        USER_PRINTF("[Sol BLE] Keeping credentials: SSID='%s' (user can retry via BLE)\n", clientSSID);

                        // Defer full provisioning transition to loop() state machine.
                        // Doing this inside event callback blocks WiFi event processing and
                        // causes STA_STOP/STA_START timeouts and scan failures (-1/-2).
                        _enterProvisioningMode = true;
                    }
                    
                    // Send error notification if BLE provisioning active
                    if (_echoChar && _bleConnected) {
                        String response = "{\"status\":\"error\",\"reason\":\"" + String(reasonStr) + "\"}";
                        _echoChar->setValue((uint8_t*)response.c_str(), response.length());
                        _echoChar->notify();
                        USER_PRINTLN(F("[Sol BLE] ===== NOTIFICATION SENT TO APP ====="));
                        USER_PRINTF("[Sol BLE] %s\n", response.c_str());
                        USER_PRINTLN(F("[Sol BLE] ====================================="));
                    }
                } else {
                    USER_PRINTLN(F("[Sol BLE] Transient failure - will retry if configured"));
                }
                
                USER_PRINTLN(F("[Sol BLE] ========================================"));
                break;
            }
                
            default:
                break;
        }
    }
    
    // WiFi scan with filtering and caching
    void performWifiScan(bool cacheResults) {
        USER_PRINTLN(F("[Sol BLE] Scanning for networks..."));
        USER_PRINTF("[Sol BLE] Heap before scan: %d bytes\n", ESP.getFreeHeap());

        // Ensure STA is enabled for scanning. Some failure paths force AP-only mode.
        wifi_mode_t mode = WiFi.getMode();
        if (mode != WIFI_STA && mode != WIFI_AP_STA) {
            USER_PRINTF("[Sol BLE] WiFi mode %d not scan-capable, switching to AP_STA...\n", mode);
            WiFi.mode(WIFI_AP_STA);
            delay(150);
        }
        
        int16_t n = WiFi.scanNetworks(false, true);

        // Recover from transient scan state errors (-1 running / -2 not triggered on some stacks).
        if (n < 0) {
            USER_PRINTF("[Sol BLE] Initial scan returned %d, retrying once...\n", n);
            WiFi.scanDelete();
            delay(120);
            n = WiFi.scanNetworks(false, true);
        }
        
        USER_PRINTF("[Sol BLE] Scan complete: %d networks (heap: %d bytes)\n", n, ESP.getFreeHeap());
        
        // Build JSON results with RSSI filtering
        if (n == WIFI_SCAN_FAILED || n < 0) {
            USER_PRINTF("[Sol BLE] Scan FAILED: %d\n", n);
            if (cacheResults) {
                _scanResults = "{\"status\":\"error\",\"code\":" + String(n) + "}";
            }
        } else if (n == 0) {
            USER_PRINTLN(F("[Sol BLE] No networks found"));
            if (cacheResults) {
                _scanResults = "{\"status\":\"success\",\"networks\":[]}";
            }
        } else {
            // Filter and sort by RSSI
            const int MAX_NETWORKS = 8; // Compact JSON should fit 6-8 networks in 253 bytes
            const int MIN_RSSI = -85; // Show networks stronger than -85 dBm
            
            struct NetworkInfo {
                String ssid;
                int rssi;
            };
            NetworkInfo networks[MAX_NETWORKS];
            int validCount = 0;
            
            // Collect strong networks (skip empty SSIDs)
            for (int i = 0; i < n && validCount < MAX_NETWORKS; i++) {
                String ssid = WiFi.SSID(i);
                int rssi = WiFi.RSSI(i);
                if (rssi > MIN_RSSI && ssid.length() > 0) {
                    networks[validCount].ssid = ssid;
                    networks[validCount].rssi = rssi;
                    validCount++;
                }
            }
            
            USER_PRINTF("[Sol BLE] Found %d strong networks (>%d dBm)\n", validCount, MIN_RSSI);
            
            if (cacheResults) {
                const int MAX_BLE_SIZE = 240; // NimBLE notify limit is 253 bytes, leave margin
                
                _scanResults = "{\"status\":\"success\",\"networks\":[";
                int finalCount = 0;
                
                for (int i = 0; i < validCount; i++) {
                    String entry = "";
                    if (i > 0) entry += ",";
                    entry += "{\"s\":\"" + networks[i].ssid + "\",";
                    entry += "\"r\":" + String(networks[i].rssi) + "}";
                    
                    // Check if adding this entry would exceed limit
                    if (_scanResults.length() + entry.length() + 2 > MAX_BLE_SIZE) { // +2 for ]}
                        USER_PRINTF("[Sol BLE] Size limit reached at %d networks\n", finalCount);
                        break;
                    }
                    
                    _scanResults += entry;
                    finalCount++;
                    USER_PRINTF("  %d: %s (RSSI: %d)\n", finalCount, networks[i].ssid.c_str(), networks[i].rssi);
                }
                
                _scanResults += "]}";
                USER_PRINTF("[Sol BLE] Results stored: %d networks, %d bytes\n", finalCount, _scanResults.length());
            }
            
            WiFi.scanDelete();
        }
    }
    
    // BLE Server callbacks
    class ServerCallbacks : public NimBLEServerCallbacks {
    public:
        ServerCallbacks(SolBleUsermod* parent) : _parent(parent) {}
        
        void onConnect(NimBLEServer* server) override {
            USER_PRINTLN(F("[Sol BLE] Client connected!"));
            _parent->_bleConnected = true;
            _parent->_bleDisconnected = false;
            _parent->_bleConnectTime = millis();
            // Get connection handle from server
            if (server->getConnectedCount() > 0) {
                _parent->_connHandle = server->getPeerInfo(0).getConnHandle();
            }
            
            // Check if we have scan results to send (reconnect after scan)
            if (_parent->_scanState == SCAN_RESULTS_READY && !_parent->_scanResults.isEmpty()) {
                USER_PRINTLN(F("[Sol BLE] Sending stored scan results..."));
                _parent->_echoChar->setValue((uint8_t*)_parent->_scanResults.c_str(), _parent->_scanResults.length());
                
                // Reset state but keep cached results (allow re-read, user can re-scan for fresh)
                _parent->_scanState = SCAN_IDLE;
                
                USER_PRINTLN(F("[Sol BLE] Scan results sent, ready for next command"));
            } else if (!_parent->_scanResults.isEmpty()) {
                // Have cached results - will send on subscription
                USER_PRINTLN(F("[Sol BLE] Cached scan results available, waiting for subscription..."));
            } else {
                // Normal connection - send ready message
                String readyJson = "{\"status\":\"ready\",\"heap\":" + String(ESP.getFreeHeap()) + "}";
                _parent->_echoChar->setValue((uint8_t*)readyJson.c_str(), readyJson.length());
                
                USER_PRINTLN(F("[Sol BLE] Ready for commands"));
            }
            
            // Stop AP (mutual exclusion)
            if (WiFi.softAPgetStationNum() == 0) {
                USER_PRINTLN(F("[Sol BLE] Stopping AP (BLE client active)"));
                WiFi.softAPdisconnect(true);
                ::dnsServer.stop();
                apActive = false;
            }
        }
        
        void onDisconnect(NimBLEServer* server) override {
            USER_PRINTLN(F("[Sol BLE] Client disconnected!"));
            _parent->_bleConnected = false;
            _parent->_bleDisconnected = true;
            _parent->_connHandle = BLE_HS_CONN_HANDLE_NONE;
            
            // Resume AP if WiFi not connected (mutual exclusion)
            if (!WLED_CONNECTED) {
                USER_PRINTLN(F("[Sol BLE] Resuming AP (BLE client gone, WiFi not connected)"));
                WiFi.softAPConfig(IPAddress(4, 3, 2, 1), IPAddress(4, 3, 2, 1), IPAddress(255, 255, 255, 0));
                WiFi.softAP(apSSID, apPass, apChannel, apHide);
                
                // Always restart web server + DNS when resuming AP
                ::server.begin();
                ::dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
                ::dnsServer.start(53, "*", WiFi.softAPIP());
                apActive = true;
            }
        }
        
    private:
        SolBleUsermod* _parent;
    };
    
    // Characteristic callbacks
    class CharCallbacks : public NimBLECharacteristicCallbacks {
    public:
        CharCallbacks(SolBleUsermod* parent) : _parent(parent) {}
        
        void onWrite(NimBLECharacteristic* pCharacteristic) override {
            std::string value = pCharacteristic->getValue();
            String cmd = String(value.c_str());
            cmd.trim();
            
            USER_PRINTF("[Sol BLE] Received: '%s'\n", cmd.c_str());
            
            if (cmd.equalsIgnoreCase("scan")) {
                USER_PRINTLN(F("[Sol BLE] Scan command received"));
                // Send scanning response
                String scanJson = "{\"status\":\"scanning\"}";
                pCharacteristic->setValue((uint8_t*)scanJson.c_str(), scanJson.length());
                pCharacteristic->notify();
                USER_PRINTLN(F("[Sol BLE] ===== NOTIFICATION SENT TO APP ====="));
                USER_PRINTF("[Sol BLE] %s\n", scanJson.c_str());
                USER_PRINTLN(F("[Sol BLE] ====================================="));
                
                // Mark for disconnect and scan
                _parent->_scanRequested = true;
            } else if (cmd.startsWith("{")) {
                // Parse JSON for WiFi credentials
                USER_PRINTF("[Sol BLE] Parsing JSON: %s\n", cmd.c_str());
                
                // Simple JSON parsing for {"ssid":"...","psk":"..."}
                int ssidStart = cmd.indexOf("\"ssid\":\"");
                int pskStart = cmd.indexOf("\"psk\":\"");
                
                if (ssidStart > 0 && pskStart > 0) {
                    ssidStart += 8; // Skip "ssid":"
                    int ssidEnd = cmd.indexOf("\"", ssidStart);
                    
                    pskStart += 7; // Skip "psk":"
                    int pskEnd = cmd.indexOf("\"", pskStart);
                    
                    if (ssidEnd > ssidStart && pskEnd > pskStart) {
                        String ssid = cmd.substring(ssidStart, ssidEnd);
                        String psk = cmd.substring(pskStart, pskEnd);
                        
                        USER_PRINTF("[Sol BLE] WiFi Credentials - SSID: %s, PSK: %s\n", ssid.c_str(), psk.c_str());
                        
                        // Save credentials exactly like WLED does
                        memset(clientSSID, 0, 33);
                        memset(clientPass, 0, 65);
                        strlcpy(clientSSID, ssid.c_str(), 33);
                        strlcpy(clientPass, psk.c_str(), 65);
                        
                        // Trigger config save and reconnect (WLED uses these flags)
                        doSerializeConfig = true;                        
                        USER_PRINTLN(F("[Sol BLE] Credentials saved, reconnect scheduled"));
                        
                        // Don't send connecting status - let WiFi events notify success/failure
                        // String response = "{\"status\":\"connecting\"}";
                        // pCharacteristic->setValue((uint8_t*)response.c_str(), response.length());
                        // pCharacteristic->notify();
                        
                        // Set flag for main loop to handle (DON'T do blocking WiFi ops in callback!)
                        _parent->_connectRequested = true;
                    } else {
                        USER_PRINTLN(F("[Sol BLE] Failed to parse credentials"));
                        String response = "{\"status\":\"error\",\"message\":\"Invalid format\"}";
                        pCharacteristic->setValue((uint8_t*)response.c_str(), response.length());
                        pCharacteristic->notify();
                        USER_PRINTLN(F("[Sol BLE] ===== NOTIFICATION SENT TO APP ====="));
                        USER_PRINTF("[Sol BLE] %s\n", response.c_str());
                        USER_PRINTLN(F("[Sol BLE] ====================================="));
                    }
                } else {
                    // Echo response - notify
                    USER_PRINTF("[Sol BLE] Echo: %s\n", cmd.c_str());
                    String response = "{\"echo\":\"" + cmd + "\"}";
                    pCharacteristic->setValue((uint8_t*)response.c_str(), response.length());
                    pCharacteristic->notify();
                    USER_PRINTLN(F("[Sol BLE] ===== NOTIFICATION SENT TO APP ====="));
                    USER_PRINTF("[Sol BLE] %s\n", response.c_str());
                    USER_PRINTLN(F("[Sol BLE] ====================================="));
                }
            } else {
                // Echo response - notify
                USER_PRINTF("[Sol BLE] Echo: %s\n", cmd.c_str());
                String response = "{\"echo\":\"" + cmd + "\"}";
                pCharacteristic->setValue((uint8_t*)response.c_str(), response.length());
                pCharacteristic->notify();
                USER_PRINTLN(F("[Sol BLE] ===== NOTIFICATION SENT TO APP ====="));
                USER_PRINTF("[Sol BLE] %s\n", response.c_str());
                USER_PRINTLN(F("[Sol BLE] ====================================="));
            }
        }
        
        void onRead(NimBLECharacteristic* pCharacteristic) override {
            USER_PRINTF("[Sol BLE] Client read: '%s'\n", pCharacteristic->getValue().c_str());
        }
        
        void onSubscribe(NimBLECharacteristic* pCharacteristic, ble_gap_conn_desc* desc, uint16_t subValue) override {
            USER_PRINTF("[Sol BLE] Client subscribed (value: %d)\n", subValue);
            
            // If subscribed and we have cached results, send them immediately
            if (subValue > 0 && !_parent->_scanResults.isEmpty() && _parent->_scanState == SCAN_IDLE) {
                USER_PRINTLN(F("[Sol BLE] Sending cached scan results on subscription..."));
                pCharacteristic->setValue((uint8_t*)_parent->_scanResults.c_str(), _parent->_scanResults.length());
                pCharacteristic->notify();
                USER_PRINTLN(F("[Sol BLE] ===== NOTIFICATION SENT TO APP ====="));
                USER_PRINTF("[Sol BLE] %s\n", _parent->_scanResults.c_str());
                USER_PRINTLN(F("[Sol BLE] ====================================="));
            }
        }
        
    private:
        SolBleUsermod* _parent;
    };
};

const char SolBleUsermod::_name[] PROGMEM = "Sol_BLE";

#endif // USERMOD_SOL_BLE
