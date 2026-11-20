// ===========================================================================
// l_ble.cpp — Definitions for BLE globals (one translation unit only)
// ===========================================================================

#include "l_ble.h"

static BLEServer* bleServer = nullptr;
static BLECharacteristic* bleTxChar = nullptr;
static SemaphoreHandle_t bleMutex = nullptr;
static volatile bool bleClientConnected = false;
static String bleRxBuffer;
