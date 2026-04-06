#pragma once

/*
 * gl_heap_monitor.h — Detailed ESP32 Heap Monitoring Usermod
 * 
 * Purpose: Track and report detailed heap usage for debugging memory issues
 * 
 * Reports every 10 seconds:
 *   - Free heap (total)
 *   - Largest free block (contiguous)
 *   - Min free heap (low water mark since boot)
 *   - Heap by capability (8bit, 32bit, DMA, exec)
 *   - Task stack usage (high water marks)
 */

#ifdef USERMOD_GL_HEAP_MONITOR

#include "wled.h"
#include <esp_heap_caps.h>
#include <esp_system.h>

class GLHeapMonitorUsermod : public Usermod {
public:
    static const char _name[];

    void setup() override {
        _lastReport = millis();
        USER_PRINTLN(F("[Heap Monitor] Initialized"));
        printDetailedHeapInfo(); // Print initial state
    }

    void loop() override {
        // Report every 10 seconds
        if (millis() - _lastReport > 10000) {
            printDetailedHeapInfo();
            _lastReport = millis();
        }
    }

    uint16_t getId() override { return USERMOD_ID_UNSPECIFIED; }

private:
    uint32_t _lastReport = 0;

    void printDetailedHeapInfo() {
        USER_PRINTLN(F("\n========== HEAP REPORT =========="));
        
        // Basic heap info
        uint32_t freeHeap = ESP.getFreeHeap();
        uint32_t minFreeHeap = ESP.getMinFreeHeap();
        uint32_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        
        USER_PRINTF("Free Heap:       %6d bytes\n", freeHeap);
        USER_PRINTF("Largest Block:   %6d bytes (contiguous)\n", largestBlock);
        USER_PRINTF("Min Free Ever:   %6d bytes (low water mark)\n", minFreeHeap);
        USER_PRINTF("Fragmentation:   %6d bytes lost to fragments\n", freeHeap - largestBlock);
        
        // Heap by capability
        USER_PRINTLN(F("\n--- Heap by Capability ---"));
        USER_PRINTF("8-bit capable:   %6d bytes\n", heap_caps_get_free_size(MALLOC_CAP_8BIT));
        USER_PRINTF("32-bit capable:  %6d bytes\n", heap_caps_get_free_size(MALLOC_CAP_32BIT));
        USER_PRINTF("DMA capable:     %6d bytes\n", heap_caps_get_free_size(MALLOC_CAP_DMA));
        USER_PRINTF("Exec capable:    %6d bytes\n", heap_caps_get_free_size(MALLOC_CAP_EXEC));
        
        // Internal vs external (SPIRAM if available)
        USER_PRINTLN(F("\n--- Memory Location ---"));
        USER_PRINTF("Internal RAM:    %6d bytes free\n", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        #ifdef BOARD_HAS_PSRAM
        USER_PRINTF("External PSRAM:  %6d bytes free\n", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        #else
        USER_PRINTLN("External PSRAM:  Not available");
        #endif
        
        // Multi-heap summary
        USER_PRINTLN(F("\n--- Multi-Heap Info ---"));
        multi_heap_info_t info;
        heap_caps_get_info(&info, MALLOC_CAP_8BIT);
        USER_PRINTF("Total free:      %6d bytes\n", info.total_free_bytes);
        USER_PRINTF("Total allocated: %6d bytes\n", info.total_allocated_bytes);
        USER_PRINTF("Largest block:   %6d bytes\n", info.largest_free_block);
        USER_PRINTF("Min free ever:   %6d bytes\n", info.minimum_free_bytes);
        USER_PRINTF("Allocated blks:  %6d\n", info.allocated_blocks);
        USER_PRINTF("Free blocks:     %6d\n", info.free_blocks);
        USER_PRINTF("Total blocks:    %6d\n", info.total_blocks);
        
        // Task stack high water marks (if available)
        #ifdef ARDUINO_ARCH_ESP32
        USER_PRINTLN(F("\n--- Task Stack Usage ---"));
        TaskHandle_t loopTask = xTaskGetHandle("loopTask");
        if (loopTask) {
            UBaseType_t loopStack = uxTaskGetStackHighWaterMark(loopTask);
            USER_PRINTF("Loop task stack: %6d bytes remaining\n", loopStack * 4); // Stack is in words (4 bytes)
        }
        
        TaskHandle_t asyncTcp = xTaskGetHandle("async_tcp");
        if (asyncTcp) {
            UBaseType_t asyncStack = uxTaskGetStackHighWaterMark(asyncTcp);
            USER_PRINTF("Async TCP stack: %6d bytes remaining\n", asyncStack * 4);
        }
        #endif
        
        USER_PRINTLN(F("=================================\n"));
    }
};

const char GLHeapMonitorUsermod::_name[] PROGMEM = "Heap_Monitor";

#endif // USERMOD_GL_HEAP_MONITOR
