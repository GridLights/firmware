#pragma once

/*
 * gl_ble_test.h — Simple WiFi/BLE Test Usermod
 * 
 * Clean slate for testing WiFi and BLE interactions
 */

#ifdef USERMOD_GL_BLE_TEST

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

// Comment/uncomment these lines to enable/disable features
#define ENABLE_WIFI
#define ENABLE_WIFI_SCAN
#define ENABLE_BLE
#define BLE_WAIT_FOR_CONNECTION  // Wait for BLE connect+disconnect before cycling
// #define BLE_AUTO_DISCONNECT  // Auto-disconnect using WIFI_TOGGLE_INTERVAL timeout
// NimBLE deinit mode:
// false = stop BLE but keep allocations reserved (faster + safer against WDT)
// true  = full heap release (can be slower and may increase WDT risk under heavy cycles)
#define BLE_DEINIT_RELEASE_HEAP false

// Toggle interval in milliseconds
#define WIFI_TOGGLE_INTERVAL 500

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
bool gl_ble_wifiAuthFailure = false;


class GLBLETestUsermod : public Usermod {
public:
    static const char _name[];

    void readFromJsonState(JsonObject& root) override {
        JsonObject gl = root["gl"];
        if (gl.isNull()) return;

        bool doFactoryReset = gl["factory_reset"] | false;
        if (doFactoryReset) {
            _factoryResetRequested = true;
            USER_PRINTLN(F("[BLE Test] WS command received: gl.factory_reset=true"));
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
            USER_PRINTLN(F("[BLE Test] Core button actions disabled for button 0"));
        }

        // Force LED output OFF on boot for test workflow
        // (override restored WLED state from previous session)
        if (bri > 0) briLast = bri;
        bri = 0;
        turnOnAtBoot = false;
        stateUpdated(CALL_MODE_INIT);
        _bootOffApplied = true;
        USER_PRINTLN(F("[BLE Test] Boot policy: LEDs forced OFF"));

        // One-shot boot blink (after boot-off): OFF -> brief ON -> OFF
        _bootBlinkPending = true;
        _bootBlinkOn = false;
        _bootBlinkAt = millis() + BOOT_BLINK_DELAY_MS;
        USER_PRINTLN(F("[BLE Test] Boot blink scheduled"));
        
        // Reset auth failure flag on boot
        gl_ble_wifiAuthFailure = false;
        
        // Disable WLED's automatic AP restart - usermod handles AP lifecycle
        apBehavior = AP_BEHAVIOR_BUTTON_ONLY;
        USER_PRINTLN(F("[BLE Test] Set apBehavior=BUTTON_ONLY (usermod controls AP)"));

        // SOL-50: Brand AP SSID as "SolSpektrum-XXXX" (first 4 hex chars of MAC)
        // escapedMac is "AABBCCDDEEFF" — take first 4 chars for short unique suffix
        snprintf(apSSID, sizeof(apSSID), "SolSpektrum-%.4s", escapedMac.c_str());
        strlcpy(apPass, "sol1234", sizeof(apPass));
        USER_PRINTF("[BLE Test] AP SSID: %s\n", apSSID);
        
        // Register WiFi event handler for connection events
        WiFi.onEvent([this](WiFiEvent_t event, WiFiEventInfo_t info) {
            this->handleWiFiEvent(event, info);
        });
        USER_PRINTLN(F("[BLE Test] WiFi event handler registered"));
        
        // Prevent WLED's handleConnection() from calling initConnection() immediately.
        // initConnection() calls WiFi.disconnect(true) which would destroy our softAP
        // before our loop() has a chance to start it. Setting lastReconnectAttempt here
        // means handleConnection() won't call initConnection() until the 18s timeout,
        // by which time our loop() has already started the AP and set apActive=true.
        lastReconnectAttempt = millis();

        // Start in IDLE state - wait for WLED to attempt WiFi connection
        _state = STATE_IDLE;
        USER_PRINTLN(F("[BLE Test] Waiting for WLED WiFi connection attempt..."));
    }

    void loop() override {
        // App-triggered factory reset command (WS/JSON)
        if (_factoryResetRequested) {
            _factoryResetRequested = false;
            performFactoryReset("[BLE Test] Factory reset triggered via WS command");
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
                USER_PRINTLN(F("[BLE Test] Boot blink ON (white)"));
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
                USER_PRINTLN(F("[BLE Test] Boot blink OFF"));
            }
        }

        // One-time reassert in first few seconds in case another boot path turns LEDs back on
        if (_bootOffApplied && !_bootOffReasserted && !_bootBlinkPending && millis() - _setupTime < 3000 && bri > 0) {
            briLast = bri;
            bri = 0;
            stateUpdated(CALL_MODE_INIT);
            _bootOffReasserted = true;
            USER_PRINTLN(F("[BLE Test] Reasserted OFF state after boot"));
        }

        // Monitor AP connections for mutual exclusion with BLE
        uint8_t stationCount = WiFi.softAPgetStationNum();
        
        // Track when we last saw AP clients
        if (stationCount > 0) {
            _lastApClientActivity = millis();
        }
        
        // AP client connects → stop BLE (mutual exclusion)
        if (_state == STATE_BLE_WAITING && stationCount > 0 && _lastStationCount == 0) {
            USER_PRINTF("[BLE Test] AP client connecting! Stopping BLE. Heap: %d bytes\n", ESP.getFreeHeap());
            _state = STATE_BLE_STOPPING;
        }
        
        // AP client disconnects → restart BLE if WiFi not connected
        if (stationCount == 0 && _lastStationCount > 0 && !WLED_CONNECTED && 
            (_state == STATE_IDLE || _state == STATE_BLE_STOPPING)) {
            USER_PRINTLN(F("[BLE Test] AP client disconnected, restarting BLE..."));
            _state = STATE_BLE_STARTING;
        }
        
        _lastStationCount = stationCount;
        
        // Factory reset: hold button for 3s to clear WiFi credentials
        if (btnPin[0] >= 0) {
            if (digitalRead(btnPin[0]) == LOW) {
                if (!_buttonPressed) {
                    _buttonPressStart = millis();
                    _buttonPressed = true;
                    USER_PRINTLN(F("[BLE Test] Button pressed, hold for 3s to factory reset..."));
                } else if (millis() - _buttonPressStart > 3000) {
                    performFactoryReset("[BLE Test] Factory reset triggered via button hold");
                }
            } else {
                if (_buttonPressed && millis() - _buttonPressStart < 3000) {
                    USER_PRINTLN(F("[BLE Test] Button released, reset cancelled"));
                }
                _buttonPressed = false;
            }
        }
        
        // State machine with timing
        if (millis() - _lastToggle > WIFI_TOGGLE_INTERVAL) {
            _lastToggle = millis();
            
            switch (_state) {
                case STATE_BLE_STARTING: {
                    USER_PRINTLN(F("[BLE Test] Starting BLE..."));
                    USER_PRINTF("[BLE Test] Heap before: %d bytes\n", ESP.getFreeHeap());
                    NimBLEDevice::init(serverDescription);  // uses WLED's stored name (default: 'Sol Spektrum - Unconfigured')
                    _bleServer = NimBLEDevice::createServer();
                    _bleServer->setCallbacks(new ServerCallbacks(this));
                    _bleServer->advertiseOnDisconnect(false);
                    
                    // Create service and characteristic
                    NimBLEService* service = _bleServer->createService(BLE_SERVICE_UUID);
                    _echoChar = service->createCharacteristic(
                        BLE_CREDENTIAL_CHAR_UUID,
                        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY
                    );
                    _echoChar->setCallbacks(new CharCallbacks(this));
                    String readyJson = "{\"status\":\"ready\"}";
                    _echoChar->setValue((uint8_t*)readyJson.c_str(), readyJson.length());
                    service->start();
                    
                    // Add service UUID to advertising
                    NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
                    pAdvertising->addServiceUUID(BLE_SERVICE_UUID);
                    pAdvertising->start();
                    USER_PRINTF("[BLE Test] BLE started. Heap: %d bytes\n", ESP.getFreeHeap());
#ifdef BLE_WAIT_FOR_CONNECTION
                    USER_PRINTLN(F("[BLE Test] Waiting for BLE connection..."));
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
                        gl_ble_wifiAuthFailure = false; // Clear global flag for new connection attempt
                        // Note: WLED's WiFi.setSleep() is disabled via USERMOD_GL_BLE_TEST flag
                        // to prevent pm_set_sleep_type crash during BLE coexistence
                        WiFi.disconnect();
                        delay(100);
                        WiFi.begin(clientSSID, clientPass);
                    }
                    // Check for scan request
                    else if (_scanRequested && _bleConnected && !_bleDisconnected &&
                        _bleServer != nullptr && _connHandle != BLE_HS_CONN_HANDLE_NONE) {
                        USER_PRINTLN(F("[BLE Test] Disconnecting to perform WiFi scan..."));
                        uint16_t handle = _connHandle;
                        _connHandle = BLE_HS_CONN_HANDLE_NONE;
                        _bleServer->disconnect(handle);
                        // DON'T clear _scanRequested here - need it in STATE_BLE_STOPPING
                    }
                    // Wait for disconnection to complete cycle
                    else if (_bleDisconnected) {
                        USER_PRINTLN(F("[BLE Test] BLE connect/disconnect cycle complete"));
                        _state = STATE_BLE_STOPPING;
                    }
#ifdef BLE_AUTO_DISCONNECT
                    // Auto-disconnect after toggle interval
                    else if (_bleConnected && !_bleDisconnected && 
                             _bleServer != nullptr &&
                             _connHandle != BLE_HS_CONN_HANDLE_NONE &&
                             millis() - _bleConnectTime > WIFI_TOGGLE_INTERVAL) {
                        USER_PRINTF("[BLE Test] Auto-disconnecting after %dms timeout\n", WIFI_TOGGLE_INTERVAL);
                        uint16_t handle = _connHandle;
                        _connHandle = BLE_HS_CONN_HANDLE_NONE;  // prevent repeated terminate attempts
                        bool ok = _bleServer->disconnect(handle);
                        if (!ok) {
                            USER_PRINTLN(F("[BLE Test] Disconnect request failed or already disconnected"));
                        }
                    }
#endif
                    break;
                    
                case STATE_BLE_STOPPING:
                    USER_PRINTLN(F("[BLE Test] Stopping BLE..."));
                    USER_PRINTF("[BLE Test] Heap before: %d bytes\n", ESP.getFreeHeap());
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
                    // Reset all BLE state
                    _bleServer = nullptr;
                    _echoChar = nullptr;
                    _bleConnected = false;
                    _bleDisconnected = false;
                    _connHandle = BLE_HS_CONN_HANDLE_NONE;
                    _bleConnectTime = 0;
                    
                    // Perform WiFi scan if requested
                    if (_scanRequested) {
                        _scanRequested = false;
                        performWifiScan(true); // true = cache results
                        _scanState = SCAN_RESULTS_READY;
                    } else {
                        _scanRequested = false;
                        _connectRequested = false; // Clear if still set
                    }
                    
                    USER_PRINTF("[BLE Test] BLE stopped. Heap: %d bytes\n", ESP.getFreeHeap());
                    
                    // Only restart BLE if WiFi not connected AND no AP clients
                    if (WLED_CONNECTED) {
                        USER_PRINTLN(F("[BLE Test] WiFi connected, BLE stays off"));
                        
                        // Initialize heavy services now that BLE is stopped and heap is freed
                        USER_PRINTLN(F("[BLE Test] Initializing heavy services (UDP, E1.31, MQTT)..."));
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
                        USER_PRINTF("[BLE Test] Heavy services started. Heap: %d bytes\n", ESP.getFreeHeap());
                        
                        _state = STATE_IDLE;
                    } else if (WiFi.softAPgetStationNum() == 0) {
                        USER_PRINTLN(F("[BLE Test] WiFi not connected, restarting BLE..."));
                        _state = STATE_BLE_STARTING;
                    } else {
                        USER_PRINTLN(F("[BLE Test] AP client still connected, BLE stays off"));
                        _state = STATE_IDLE;
                    }
                    break;
                    
                case STATE_ENTERING_PROVISIONING:
                    // Non-blocking provisioning entry coordinated by events
                    switch (_provisioningPhase) {
                        case 0: // Initial - trigger disconnect
                            USER_PRINTLN(F("[BLE Test] Phase 0: Disconnecting WiFi..."));
                            _wifiStopped = false;
                            WiFi.disconnect(true);
                            _provisioningPhase = 1;
                            _provisioningPhaseStart = millis();
                            break;
                            
                        case 1: // Waiting for STA_STOP event
                            if (_wifiStopped) {
                                USER_PRINTLN(F("[BLE Test] Phase 1: STA_STOP received, changing to AP_STA mode..."));
                                _wifiStopped = true; // Keep true to detect START
                                WiFi.mode(WIFI_AP_STA);
                                _provisioningPhase = 2;
                                _provisioningPhaseStart = millis();
                            } else if (millis() - _provisioningPhaseStart > 5000) {
                                USER_PRINTLN(F("[BLE Test] Phase 1: Timeout waiting for STA_STOP, proceeding anyway"));
                                WiFi.mode(WIFI_AP_STA);
                                _provisioningPhase = 2;
                                _provisioningPhaseStart = millis();
                            }
                            break;
                            
                        case 2: // Waiting for STA_START event
                            if (!_wifiStopped) {
                                USER_PRINTLN(F("[BLE Test] Phase 2: STA_START received, WiFi ready for scan"));
                                _provisioningPhase = 3;
                            } else if (millis() - _provisioningPhaseStart > 3000) {
                                USER_PRINTLN(F("[BLE Test] Phase 2: Timeout waiting for STA_START, proceeding anyway"));
                                _provisioningPhase = 3;
                            }
                            break;
                            
                        case 3: // Ready - perform scan and start services
                            USER_PRINTLN(F("[BLE Test] Phase 3: Starting provisioning services..."));
                            
                            // Perform WiFi scan
                            performWifiScan(true);
                            
                            // Start AP
                            USER_PRINTLN(F("[BLE Test] Starting AP..."));
                            WiFi.softAPConfig(IPAddress(4, 3, 2, 1), IPAddress(4, 3, 2, 1), IPAddress(255, 255, 255, 0));
                            WiFi.softAP(apSSID, apPass, apChannel, apHide);
                            
                            if (!apActive) {
                                server.begin();
                                dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
                                dnsServer.start(53, "*", WiFi.softAPIP());
                                apActive = true;
                            }
                            
                            USER_PRINTF("[BLE Test] AP started. Heap: %d bytes\n", ESP.getFreeHeap());
                            
                            // Start BLE for provisioning
                            _provisioningStarted = true;
                            _enterProvisioningMode = false;
                            _state = STATE_BLE_STARTING;
                            USER_PRINTLN(F("[BLE Test] Provisioning mode ready, starting BLE..."));
                            break;
                    }
                    break;
                    
                case STATE_IDLE:
                    // Check for provisioning entry flag from event handler
                    if (_enterProvisioningMode) {
                        USER_PRINTLN(F("[BLE Test] Provisioning flag set, transitioning to STATE_ENTERING_PROVISIONING"));
                        _state = STATE_ENTERING_PROVISIONING;
                    }
                    // Check immediately if WiFi is configured or if connection failed
                    else if (!_provisioningStarted) {
                        // Check if WiFi credentials are configured
                        bool wifiConfigured = WLED_WIFI_CONFIGURED;
                        
                        if (!wifiConfigured) {
                            // No WiFi configured - start provisioning immediately
                            USER_PRINTLN(F("[BLE Test] No WiFi configured - starting provisioning"));
                            
                            // Perform WiFi scan (function prints its own message)
                            performWifiScan(true);
                            USER_PRINTF("[BLE Test] Scan complete, cached %d bytes\n", _scanResults.length());
                            
                            // Start AP
                            USER_PRINTLN(F("[BLE Test] Starting AP..."));
                            WiFi.softAPConfig(IPAddress(4, 3, 2, 1), IPAddress(4, 3, 2, 1), IPAddress(255, 255, 255, 0));
                            WiFi.softAP(apSSID, apPass, apChannel, apHide);
                            
                            if (!apActive) {
                                server.begin();
                                dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
                                dnsServer.start(53, "*", WiFi.softAPIP());
                                apActive = true;
                            }
                            
                            USER_PRINTF("[BLE Test] AP started. Heap: %d bytes\n", ESP.getFreeHeap());
                            
                            _provisioningStarted = true;
                            _state = STATE_BLE_STARTING;
                            USER_PRINTLN(F("[BLE Test] Entering AP+BLE provisioning mode"));
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
            USER_PRINTF("[BLE] Device rename saved: %s\n", serverDescription);
        }
    } // end loop()

    void connected() override {
        // Called when WiFi successfully connects (boot or runtime)
        USER_PRINTF("[BLE Test] connected() callback - WiFi connected to: %s\n", clientSSID);
        _provisioningStarted = true; // Mark that WiFi is working, don't start provisioning
        _bootConnection = false;
        _connectionAttempts = 0;
        gl_ble_wifiAuthFailure = false; // Clear global flag on successful connection

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
                        request->send(400, "application/json", "{\"error\":\"name must be 1-32 chars\"}");
                        return;
                    }
                    strlcpy(serverDescription, name, 33);
                    USER_PRINTF("[BLE] Device renamed to: %s (save deferred)\n", serverDescription);
                    _pendingRename = true;
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
    volatile bool _pendingRename = false;

    void applyPulseFrame(byte targetBri, byte r, byte g, byte b) {
        col[0] = r; col[1] = g; col[2] = b; col[3] = 0;
        colSec[0] = r; colSec[1] = g; colSec[2] = b; colSec[3] = 0;
        effectCurrent = FX_MODE_STATIC;
        bri = targetBri;
        jsonTransitionOnce = true;
        transitionDelay = BLINK_TRANSITION_MS;
        colorUpdated(CALL_MODE_NO_NOTIFY);
    }

    void performFactoryReset(const char* sourceMsg) {
        USER_PRINTLN(sourceMsg);
        USER_PRINTLN(F("[BLE Test] Clearing WiFi credentials and resetting device name..."));

        // Match existing button-reset behavior exactly
        strcpy(clientSSID, "Your_Network");
        memset(clientPass, 0, 65);
        // Reset device name to default so BLE advertises "Sol Spektrum - Unconfigured" again
        strlcpy(serverDescription, "Sol Spektrum - Unconfigured", 33);
        // SOL-50: Re-apply branded AP credentials so cfg.json doesn't keep stale values
        snprintf(apSSID, sizeof(apSSID), "SolSpektrum-%.4s", escapedMac.c_str());
        strlcpy(apPass, "sol1234", sizeof(apPass));
        serializeConfig();
        WiFi.disconnect(true, true);

        delay(500);
        USER_PRINTLN(F("[BLE Test] Rebooting..."));
        ESP.restart();
    }

    // WiFi event handler - responds immediately to connection events
    // Handles BOTH boot-time connection attempts AND BLE provisioning attempts
    void handleWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
        switch (event) {
            case SYSTEM_EVENT_STA_START:
                USER_PRINTLN(F("[BLE Test] WiFi event: STA_START"));
                _wifiStopped = false;
                break;
                
            case SYSTEM_EVENT_STA_STOP:
                USER_PRINTLN(F("[BLE Test] WiFi event: STA_STOP"));
                _wifiStopped = true;
                break;
                
            case SYSTEM_EVENT_STA_CONNECTED:
                USER_PRINTF("[BLE Test] WiFi event: STA_CONNECTED to SSID: %s\n", clientSSID);
                _connectionAttempts = 0; // Reset attempt counter on successful connect
                // Wait for IP assignment before declaring success
                break;
                
            case SYSTEM_EVENT_STA_GOT_IP:
                USER_PRINTLN(F("[BLE Test] WiFi event: GOT_IP - Connection successful!"));
                USER_PRINTF("[BLE Test] IP address: %s\n", WiFi.localIP().toString().c_str());
                
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
                    USER_PRINTLN(F("[BLE Test] ===== NOTIFICATION SENT TO APP ====="));
                    USER_PRINTF("[BLE Test] %s\n", response.c_str());
                    USER_PRINTLN(F("[BLE Test] ====================================="));
                }
                
                // Stop BLE to free heap (BLE provisioning path)
                if (_state == STATE_BLE_WAITING) {
                    USER_PRINTLN(F("[BLE Test] Stopping BLE to free heap after successful connection"));
                    _state = STATE_BLE_STOPPING;
                } else if (_state != STATE_BLE_STOPPING) {
                    // Normal boot path: BLE was never started, start heavy services now
                    USER_PRINTLN(F("[BLE Test] Normal boot WiFi connect - starting heavy services"));
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
                    USER_PRINTF("[BLE Test] Heavy services started. Heap: %d bytes\n", ESP.getFreeHeap());
                }
                break;
                
            case SYSTEM_EVENT_STA_DISCONNECTED: {
                wifi_err_reason_t reason = (wifi_err_reason_t)info.disconnected.reason;
                
                USER_PRINTLN(F("[BLE Test] ========================================"));
                USER_PRINTF("[BLE Test] WiFi DISCONNECTED event (reason code: %d)\n", reason);
                
                // Increment attempt counter
                _connectionAttempts++;
                USER_PRINTF("[BLE Test] Connection attempt #%d\n", _connectionAttempts);
                
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
                            USER_PRINTLN(F("[BLE Test] Ignoring ASSOC_LEAVE during connection setup"));
                            USER_PRINTLN(F("[BLE Test] ========================================"));
                            return; // Ignore - wait for real connection result
                        }
                        // Otherwise treat as graceful disconnect
                        reasonDesc = "Graceful disconnect (user-initiated)";
                        USER_PRINTF("[BLE Test] %s\n", reasonDesc);
                        USER_PRINTLN(F("[BLE Test] ========================================"));
                        return; // Don't treat as error
                    default:
                        reasonDesc = "Connection failed";
                        break;
                }
                
                USER_PRINTF("[BLE Test] Failure reason: %s\n", reasonDesc);
                
                // STOP RECONNECTION ATTEMPTS IMMEDIATELY
                USER_PRINTLN(F("[BLE Test] Calling WiFi.disconnect(true) to stop reconnection..."));
                WiFi.disconnect(true);
                WiFi.mode(WIFI_AP);
                
                if (isPermanentFailure) {
                    USER_PRINTLN(F("[BLE Test] PERMANENT FAILURE DETECTED - will not retry!"));
                    _authFailure = true;
                    _wifiConnecting = false;
                    
                    // Set global flag to block WLED's reconnection attempts
                    gl_ble_wifiAuthFailure = true;
                    USER_PRINTLN(F("[BLE Test] Set gl_ble_wifiAuthFailure=true to block WLED reconnection"));
                    
                    // Enter provisioning mode if:
                    // 1. Not in BLE provisioning state (must be boot/WLED retry)
                    // 2. Multiple failures from BLE provisioning (3+ attempts)
                    bool isBootFailure = (_state != STATE_BLE_WAITING);
                    bool tooManyAttempts = (_connectionAttempts >= 3);
                    
                    if (isBootFailure || tooManyAttempts) {
                        if (isBootFailure) {
                            USER_PRINTLN(F("[BLE Test] Boot connection failed - entering provisioning mode"));
                        } else {
                            USER_PRINTF("[BLE Test] Too many attempts (%d) - entering provisioning mode\n", _connectionAttempts);
                        }
                        USER_PRINTLN(F("[BLE Test] Could not connect to WiFi - entering provisioning mode"));
                        USER_PRINTF("[BLE Test] Keeping credentials: SSID='%s' (user can retry via BLE)\n", clientSSID);
                        
                        // Log current WiFi state
                        wifi_mode_t mode = WiFi.getMode();
                        USER_PRINTF("[BLE Test] Current WiFi mode: %d (0=OFF, 1=STA, 2=AP, 3=AP_STA)\n", mode);
                        USER_PRINTF("[BLE Test] WiFi status: %d\n", WiFi.status());
                        
                        // Disconnect and wait for STA_STOP event
                        USER_PRINTLN(F("[BLE Test] Calling WiFi.disconnect(true) and waiting for STA_STOP event..."));
                        _wifiStopped = false;
                        WiFi.disconnect(true);
                        
                        // Wait up to 60s for STA_STOP event
                        unsigned long waitStart = millis();
                        while (!_wifiStopped && (millis() - waitStart < 60000)) {
                            delay(100);
                        }
                        
                        if (_wifiStopped) {
                            USER_PRINTLN(F("[BLE Test] STA stopped cleanly via event"));
                        } else {
                            USER_PRINTLN(F("[BLE Test] WARNING: Timeout waiting for STA_STOP after 60s"));
                        }
                        
                        // Set WiFi to AP+STA mode for scanning and wait for STA_START
                        USER_PRINTLN(F("[BLE Test] Setting WiFi mode to AP_STA..."));
                        _wifiStopped = true; // Reset flag to detect STA_START
                        WiFi.mode(WIFI_AP_STA);
                        
                        // Wait up to 3s for STA_START event
                        waitStart = millis();
                        while (_wifiStopped && (millis() - waitStart < 3000)) {
                            delay(10);
                        }
                        
                        if (!_wifiStopped) {
                            USER_PRINTLN(F("[BLE Test] STA started cleanly via event"));
                        } else {
                            USER_PRINTLN(F("[BLE Test] WARNING: Timeout waiting for STA_START after 3s"));
                        }
                        
                        mode = WiFi.getMode();
                        USER_PRINTF("[BLE Test] WiFi mode after change: %d\n", mode);
                        USER_PRINTF("[BLE Test] WiFi status: %d\n", WiFi.status());
                        
                        // Perform WiFi scan
                        USER_PRINTLN(F("[BLE Test] WiFi ready, starting scan..."));
                        performWifiScan(true);
                        
                        // Start AP
                        USER_PRINTLN(F("[BLE Test] Starting AP..."));
                        WiFi.softAPConfig(IPAddress(4, 3, 2, 1), IPAddress(4, 3, 2, 1), IPAddress(255, 255, 255, 0));
                        WiFi.softAP(apSSID, apPass, apChannel, apHide);
                        
                        if (!apActive) {
                            server.begin();
                            dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
                            dnsServer.start(53, "*", WiFi.softAPIP());
                            apActive = true;
                        }
                        
                        USER_PRINTF("[BLE Test] AP started. Heap: %d bytes\n", ESP.getFreeHeap());
                        
                        // Start BLE for provisioning
                        _provisioningStarted = true;
                        _bootConnection = false;
                        _state = STATE_BLE_STARTING;
                        USER_PRINTLN(F("[BLE Test] Transitioning to STATE_BLE_STARTING for provisioning"));
                    }
                    
                    // Send error notification if BLE provisioning active
                    if (_echoChar && _bleConnected) {
                        String response = "{\"status\":\"error\",\"reason\":\"" + String(reasonStr) + "\"}";
                        _echoChar->setValue((uint8_t*)response.c_str(), response.length());
                        _echoChar->notify();
                        USER_PRINTLN(F("[BLE Test] ===== NOTIFICATION SENT TO APP ====="));
                        USER_PRINTF("[BLE Test] %s\n", response.c_str());
                        USER_PRINTLN(F("[BLE Test] ====================================="));
                    }
                } else {
                    USER_PRINTLN(F("[BLE Test] Transient failure - will retry if configured"));
                }
                
                USER_PRINTLN(F("[BLE Test] ========================================"));
                break;
            }
                
            default:
                break;
        }
    }
    
    // WiFi scan with filtering and caching
    void performWifiScan(bool cacheResults) {
        USER_PRINTLN(F("[BLE Test] Scanning for networks..."));
        USER_PRINTF("[BLE Test] Heap before scan: %d bytes\n", ESP.getFreeHeap());
        
        int16_t n = WiFi.scanNetworks(false, true);
        
        USER_PRINTF("[BLE Test] Scan complete: %d networks (heap: %d bytes)\n", n, ESP.getFreeHeap());
        
        // Build JSON results with RSSI filtering
        if (n == WIFI_SCAN_FAILED || n < 0) {
            USER_PRINTF("[BLE Test] Scan FAILED: %d\n", n);
            if (cacheResults) {
                _scanResults = "{\"status\":\"error\",\"code\":" + String(n) + "}";
            }
        } else if (n == 0) {
            USER_PRINTLN(F("[BLE Test] No networks found"));
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
            
            USER_PRINTF("[BLE Test] Found %d strong networks (>%d dBm)\n", validCount, MIN_RSSI);
            
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
                        USER_PRINTF("[BLE Test] Size limit reached at %d networks\n", finalCount);
                        break;
                    }
                    
                    _scanResults += entry;
                    finalCount++;
                    USER_PRINTF("  %d: %s (RSSI: %d)\n", finalCount, networks[i].ssid.c_str(), networks[i].rssi);
                }
                
                _scanResults += "]}";
                USER_PRINTF("[BLE Test] Results stored: %d networks, %d bytes\n", finalCount, _scanResults.length());
            }
            
            WiFi.scanDelete();
        }
    }
    
    // BLE Server callbacks
    class ServerCallbacks : public NimBLEServerCallbacks {
    public:
        ServerCallbacks(GLBLETestUsermod* parent) : _parent(parent) {}
        
        void onConnect(NimBLEServer* server) override {
            USER_PRINTLN(F("[BLE Test] Client connected!"));
            _parent->_bleConnected = true;
            _parent->_bleDisconnected = false;
            _parent->_bleConnectTime = millis();
            // Get connection handle from server
            if (server->getConnectedCount() > 0) {
                _parent->_connHandle = server->getPeerInfo(0).getConnHandle();
            }
            
            // Check if we have scan results to send (reconnect after scan)
            if (_parent->_scanState == SCAN_RESULTS_READY && !_parent->_scanResults.isEmpty()) {
                USER_PRINTLN(F("[BLE Test] Sending stored scan results..."));
                _parent->_echoChar->setValue((uint8_t*)_parent->_scanResults.c_str(), _parent->_scanResults.length());
                
                // Reset state but keep cached results (allow re-read, user can re-scan for fresh)
                _parent->_scanState = SCAN_IDLE;
                
                USER_PRINTLN(F("[BLE Test] Scan results sent, ready for next command"));
            } else if (!_parent->_scanResults.isEmpty()) {
                // Have cached results - will send on subscription
                USER_PRINTLN(F("[BLE Test] Cached scan results available, waiting for subscription..."));
            } else {
                // Normal connection - send ready message
                String readyJson = "{\"status\":\"ready\",\"heap\":" + String(ESP.getFreeHeap()) + "}";
                _parent->_echoChar->setValue((uint8_t*)readyJson.c_str(), readyJson.length());
                
                USER_PRINTLN(F("[BLE Test] Ready for commands"));
            }
            
            // Stop AP (mutual exclusion)
            if (WiFi.softAPgetStationNum() == 0) {
                USER_PRINTLN(F("[BLE Test] Stopping AP (BLE client active)"));
                WiFi.softAPdisconnect(true);
                ::dnsServer.stop();
                apActive = false;
            }
        }
        
        void onDisconnect(NimBLEServer* server) override {
            USER_PRINTLN(F("[BLE Test] Client disconnected!"));
            _parent->_bleConnected = false;
            _parent->_bleDisconnected = true;
            _parent->_connHandle = BLE_HS_CONN_HANDLE_NONE;
            
            // Resume AP if WiFi not connected (mutual exclusion)
            if (!WLED_CONNECTED) {
                USER_PRINTLN(F("[BLE Test] Resuming AP (BLE client gone, WiFi not connected)"));
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
        GLBLETestUsermod* _parent;
    };
    
    // Characteristic callbacks
    class CharCallbacks : public NimBLECharacteristicCallbacks {
    public:
        CharCallbacks(GLBLETestUsermod* parent) : _parent(parent) {}
        
        void onWrite(NimBLECharacteristic* pCharacteristic) override {
            std::string value = pCharacteristic->getValue();
            String cmd = String(value.c_str());
            cmd.trim();
            
            USER_PRINTF("[BLE Test] Received: '%s'\n", cmd.c_str());
            
            if (cmd.equalsIgnoreCase("scan")) {
                USER_PRINTLN(F("[BLE Test] Scan command received"));
                // Send scanning response
                String scanJson = "{\"status\":\"scanning\"}";
                pCharacteristic->setValue((uint8_t*)scanJson.c_str(), scanJson.length());
                pCharacteristic->notify();
                USER_PRINTLN(F("[BLE Test] ===== NOTIFICATION SENT TO APP ====="));
                USER_PRINTF("[BLE Test] %s\n", scanJson.c_str());
                USER_PRINTLN(F("[BLE Test] ====================================="));
                
                // Mark for disconnect and scan
                _parent->_scanRequested = true;
            } else if (cmd.startsWith("{")) {
                // Parse JSON for WiFi credentials
                USER_PRINTF("[BLE Test] Parsing JSON: %s\n", cmd.c_str());
                
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
                        
                        USER_PRINTF("[BLE Test] WiFi Credentials - SSID: %s, PSK: %s\n", ssid.c_str(), psk.c_str());
                        
                        // Save credentials exactly like WLED does
                        memset(clientSSID, 0, 33);
                        memset(clientPass, 0, 65);
                        strlcpy(clientSSID, ssid.c_str(), 33);
                        strlcpy(clientPass, psk.c_str(), 65);
                        
                        // Trigger config save and reconnect (WLED uses these flags)
                        doSerializeConfig = true;                        
                        USER_PRINTLN(F("[BLE Test] Credentials saved, reconnect scheduled"));
                        
                        // Don't send connecting status - let WiFi events notify success/failure
                        // String response = "{\"status\":\"connecting\"}";
                        // pCharacteristic->setValue((uint8_t*)response.c_str(), response.length());
                        // pCharacteristic->notify();
                        
                        // Set flag for main loop to handle (DON'T do blocking WiFi ops in callback!)
                        _parent->_connectRequested = true;
                    } else {
                        USER_PRINTLN(F("[BLE Test] Failed to parse credentials"));
                        String response = "{\"status\":\"error\",\"message\":\"Invalid format\"}";
                        pCharacteristic->setValue((uint8_t*)response.c_str(), response.length());
                        pCharacteristic->notify();
                        USER_PRINTLN(F("[BLE Test] ===== NOTIFICATION SENT TO APP ====="));
                        USER_PRINTF("[BLE Test] %s\n", response.c_str());
                        USER_PRINTLN(F("[BLE Test] ====================================="));
                    }
                } else {
                    // Echo response - notify
                    USER_PRINTF("[BLE Test] Echo: %s\n", cmd.c_str());
                    String response = "{\"echo\":\"" + cmd + "\"}";
                    pCharacteristic->setValue((uint8_t*)response.c_str(), response.length());
                    pCharacteristic->notify();
                    USER_PRINTLN(F("[BLE Test] ===== NOTIFICATION SENT TO APP ====="));
                    USER_PRINTF("[BLE Test] %s\n", response.c_str());
                    USER_PRINTLN(F("[BLE Test] ====================================="));
                }
            } else {
                // Echo response - notify
                USER_PRINTF("[BLE Test] Echo: %s\n", cmd.c_str());
                String response = "{\"echo\":\"" + cmd + "\"}";
                pCharacteristic->setValue((uint8_t*)response.c_str(), response.length());
                pCharacteristic->notify();
                USER_PRINTLN(F("[BLE Test] ===== NOTIFICATION SENT TO APP ====="));
                USER_PRINTF("[BLE Test] %s\n", response.c_str());
                USER_PRINTLN(F("[BLE Test] ====================================="));
            }
        }
        
        void onRead(NimBLECharacteristic* pCharacteristic) override {
            USER_PRINTF("[BLE Test] Client read: '%s'\n", pCharacteristic->getValue().c_str());
        }
        
        void onSubscribe(NimBLECharacteristic* pCharacteristic, ble_gap_conn_desc* desc, uint16_t subValue) override {
            USER_PRINTF("[BLE Test] Client subscribed (value: %d)\n", subValue);
            
            // If subscribed and we have cached results, send them immediately
            if (subValue > 0 && !_parent->_scanResults.isEmpty() && _parent->_scanState == SCAN_IDLE) {
                USER_PRINTLN(F("[BLE Test] Sending cached scan results on subscription..."));
                pCharacteristic->setValue((uint8_t*)_parent->_scanResults.c_str(), _parent->_scanResults.length());
                pCharacteristic->notify();
                USER_PRINTLN(F("[BLE Test] ===== NOTIFICATION SENT TO APP ====="));
                USER_PRINTF("[BLE Test] %s\n", _parent->_scanResults.c_str());
                USER_PRINTLN(F("[BLE Test] ====================================="));
            }
        }
        
    private:
        GLBLETestUsermod* _parent;
    };
};

const char GLBLETestUsermod::_name[] PROGMEM = "BLE_Test";

#endif // USERMOD_GL_BLE_TEST
