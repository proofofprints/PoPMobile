/**
 * ============================================================================
 * KASDeck - Kaspa Lottery Miner & Dashboard
 * ============================================================================
 *
 * Copyright (c) 2026 Proof of Prints
 * All rights reserved.
 *
 * Website: https://proofofprints.com
 * Support: support@proofofprints.com
 *
 * ============================================================================
 * LICENSE & DISCLAIMER
 * ============================================================================
 *
 * This software is provided "AS IS" without warranty of any kind, express or
 * implied, including but not limited to the warranties of merchantability,
 * fitness for a particular purpose, and noninfringement.
 *
 * In no event shall Proof of Prints or its contributors be liable for any
 * claim, damages, or other liability, whether in an action of contract, tort,
 * or otherwise, arising from, out of, or in connection with the software or
 * the use or other dealings in the software.
 *
 * CRYPTOCURRENCY MINING DISCLAIMER:
 * This device performs "lottery-style" mining with extremely low hashrate
 * (~1.15 KH/s). The probability of finding a block is astronomically low.
 * This is intended for educational and entertainment purposes only.
 * Do NOT expect any financial returns from mining with this device.
 *
 * USE AT YOUR OWN RISK:
 * - Mining cryptocurrency may be subject to local regulations
 * - Ensure you understand your local laws before mining
 * - This device connects to third-party services (pools, APIs)
 * - Always use secure network connections
 * - Keep your wallet addresses and passwords secure
 *
 * ============================================================================
 * HARDWARE REQUIREMENTS
 * ============================================================================
 * - Elecrow CrowPanel 7.0" ESP32-S3 (HMI, 800x480)
 * - MicroSD card for web interface files
 * - WiFi network connection
 *
 * ============================================================================
 * VERSION HISTORY
 * ============================================================================
 * v1.0.6 - Corrected system log to show day/time
 *        - Authentication required immediately after reboot and refresh. 
 * ============================================================================
 */

#include <Wire.h>
#include <TAMC_GT911.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <KeccakCore.h>
#include <Update.h>
#include <time.h>
#include "esp_task_wdt.h"  // For disabling watchdog during firmware update

/*********************************************************************
  KASDeck by Proof of Prints
*********************************************************************/

// ==================== SIZE OPTIMIZATION ====================
// Disable verbose Serial output in production builds to save ~400KB flash
#define SERIAL_DEBUG 0  // Set to 1 to enable debug output, 0 for production

// Feature flags - All enabled with Huge APP partition (3MB available)
#define ENABLE_NETWORK_HASHRATE_CHART 1  // Network hashrate chart on main UI
#define ENABLE_PRICE_HISTORY 1           // Price history chart
#define ENABLE_MINING_STATS_SCREEN 1     // Mining stats screen
// REMOVED: Crypto news feature disabled to save firmware size
// #define ENABLE_CRYPTO_NEWS 0          // Crypto news RSS feeds (REMOVED)
#define ENABLE_WIZARD 1                  // First-time setup wizard

#if SERIAL_DEBUG
  #define DEBUG_PRINT(x) Serial.print(x)
  #define DEBUG_PRINTLN(x) Serial.println(x)
  #define DEBUG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
  #define DEBUG_PRINT(x)
  #define DEBUG_PRINTLN(x)
  #define DEBUG_PRINTF(...)
#endif
// ==========================================================

#define LV_CONF_INCLUDE_SIMPLE
#include "lv_conf.h"
#include <lvgl.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>

#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>

// =================================================================
// STRUCT DEFINITIONS - Must be before function definitions
// =================================================================

// SD Card OTA firmware header structure (used by validation functions)
struct __attribute__((packed)) FirmwareHeader {
  uint32_t magic;           // Magic bytes: "KASK" (0x4B41534B)
  char version[16];         // Firmware version string
  char hardwareVersion[8];  // Hardware compatibility version
  uint32_t firmwareSize;    // Size of actual firmware (excluding header)
  uint32_t crc32;           // CRC32 of firmware data
  char buildDate[24];       // Build timestamp
  uint8_t reserved[4];      // Reserved for future use (4 bytes to make total 64)
};

#define FIRMWARE_HEADER_SIZE 64  // Exact header size (4+16+8+4+4+24+4=64)

// =================================================================
// SD CARD LOGO LOADING
// =================================================================
// Logos loaded from SD card to save 1.6MB flash space
// Instead of embedding in firmware, logos are stored as .bin files on SD card

// Global logo image descriptors (loaded from SD card at boot)
static lv_img_dsc_t kaspa_logo_sd;
static lv_img_dsc_t pop_logo_sd;
static uint8_t* kaspa_logo_data = nullptr;
static uint8_t* pop_logo_data = nullptr;
static bool logos_loaded = false;

// Load logo from SD card into memory
bool loadLogoFromSD(const char* filename, lv_img_dsc_t* img_dsc, uint8_t** data_ptr, uint16_t width, uint16_t height, lv_img_cf_t color_format) {
  if (!SD.exists(filename)) {
    Serial.printf("Warning: Logo file %s not found on SD card\n", filename);
    return false;
  }

  File file = SD.open(filename, FILE_READ);
  if (!file) {
    Serial.printf("Error: Failed to open %s\n", filename);
    return false;
  }

  size_t file_size = file.size();
  *data_ptr = (uint8_t*)malloc(file_size);
  if (*data_ptr == nullptr) {
    Serial.printf("Error: Failed to allocate %d bytes for %s\n", file_size, filename);
    file.close();
    return false;
  }

  size_t bytes_read = file.read(*data_ptr, file_size);
  file.close();

  if (bytes_read != file_size) {
    Serial.printf("Error: Read %d bytes but expected %d from %s\n", bytes_read, file_size, filename);
    free(*data_ptr);
    *data_ptr = nullptr;
    return false;
  }

  // Setup LVGL image descriptor
  img_dsc->header.cf = color_format;
  img_dsc->header.always_zero = 0;
  img_dsc->header.reserved = 0;
  img_dsc->header.w = width;
  img_dsc->header.h = height;
  img_dsc->data_size = file_size;
  img_dsc->data = *data_ptr;

  Serial.printf("Loaded logo %s: %dx%d, %d bytes\n", filename, width, height, file_size);
  return true;
}

// Initialize logos from SD card (call once at startup after SD.begin())
void initLogosFromSD() {
  if (logos_loaded) return;

  // Load KASDeck logo (400x126, TRUE_COLOR_ALPHA)
  bool kaspa_ok = loadLogoFromSD("/kaspa_logo.bin", &kaspa_logo_sd, &kaspa_logo_data,
                                  400, 126, LV_IMG_CF_TRUE_COLOR_ALPHA);

  // Load Proof of Prints logo (200x200, TRUE_COLOR_ALPHA)
  bool pop_ok = loadLogoFromSD("/pop_logo.bin", &pop_logo_sd, &pop_logo_data,
                                200, 200, LV_IMG_CF_TRUE_COLOR_ALPHA);

  logos_loaded = (kaspa_ok && pop_ok);

  if (logos_loaded) {
    Serial.println("All logos loaded from SD card successfully");
  } else {
    Serial.println("Warning: Some logos failed to load from SD card");
  }
}

// --- MINING CONSTANTS & GLOBALS ---
uint8_t currentHeaderHash[32] = {0};  // Initialize to zeros
uint8_t currentTarget[32] = {0};       // Initialize to zeros
char currentJobId[65] = "";  // Changed from String to char array to eliminate heap allocation
bool hasJob = false;
volatile uint32_t hashes_done = 0;
unsigned long last_hash_check = 0;
TaskHandle_t MinerTask = NULL;  // Initialize to NULL - task created only when mining enabled
double currentDifficulty = 1.0;  // Track current mining difficulty

// Mutex for protecting shared mining state between cores
SemaphoreHandle_t miningStateMutex = NULL;

// Forward declaration for mining task
void miningLoopTask(void *pvParameters);

// Start the mining task (creates task if not running)
void startMiningTask() {
  if (MinerTask == NULL) {
    Serial.println("Creating mining task on Core 0...");
    xTaskCreatePinnedToCore(miningLoopTask, "Miner", 48000, NULL, 1, &MinerTask, 0);
    Serial.println("✓ Mining task started (48KB stack on Core 0)");
  }
}

// Stop the mining task (deletes task if running)
void stopMiningTask() {
  if (MinerTask != NULL) {
    Serial.println("Stopping mining task...");
    vTaskDelete(MinerTask);
    MinerTask = NULL;
    delay(50);  // Let task cleanup complete
    Serial.println("✓ Mining task stopped (48KB freed)");
  }
}

// Check if mining task is running
bool isMiningTaskRunning() {
  return (MinerTask != NULL);
}

// Forward declarations - defined later in file
void runKeccak(const uint8_t* input, size_t len, uint8_t* output);
void playBeep(int frequency, int duration);
void showSaveConfirmation();
void showErrorNotification(const char* message);
void showSuccessNotification(const char* message);
void updateMinerDisplay();
void show_wifi_config_menu();
void show_main_menu();
void show_settings_menu();
void show_miner_config_menu();
void show_system_config_menu();
void show_price_info_screen();
void show_mining_stats_screen();
void show_about_screen();
void populateWiFiList();
void checkWiFiScanComplete();
void scanWiFiNetworks();
void connectToPool();
void fetchHistoricalPriceData();
void fetch_data();
void loadHashrateHistory();
void fetchNetworkHashrate();
void updateHashrateChart();
void saveHashrateHistory();
void show_wizard_welcome();
void show_wizard_wireless_intro();
void show_wizard_mining_setup();
void show_wifi_config_wizard();

// Callback function declarations (all functions ending in _cb)
void miner_config_btn_cb(lv_event_t * e);
void settings_btn_event_cb(lv_event_t * e);
void wifi_config_btn_cb(lv_event_t * e);
void system_config_btn_cb(lv_event_t * e);
void price_info_btn_cb(lv_event_t * e);
void close_main_menu_cb(lv_event_t * e);
void close_miner_config_cb(lv_event_t * e);
void save_miner_config_cb(lv_event_t * e);
void close_settings_event_cb(lv_event_t * e);
void restart_event_cb(lv_event_t * e);
void restart_confirm_cb(lv_event_t * e);
void wifi_reset_event_cb(lv_event_t * e);
void wifi_reset_confirm_cb(lv_event_t * e);
void factory_reset_confirm_cb(lv_event_t * e);
void close_wifi_config_cb(lv_event_t * e);
void wifi_connect_btn_cb(lv_event_t * e);
void wifi_network_selected_cb(lv_event_t * e);
void wifi_password_clicked_cb(lv_event_t * e);
void wifi_password_value_changed_cb(lv_event_t * e);
void keyboard_event_cb(lv_event_t * e);
void password_toggle_cb(lv_event_t * e);
void wallet_textarea_clicked_cb(lv_event_t * e);
void back_to_main_cb(lv_event_t * e);
void web_config_btn_cb(lv_event_t * e);

// ==================== KHEAVYHASH IMPLEMENTATION ====================
// CORRECTED VERSION - Matches kaspa-miner CUDA reference implementation

// Initialization vectors from Kaspa protocol (powP and heavyP)
// These are used by the Keccak hash function
const uint8_t powP[200] = {
    0x3d, 0xd8, 0xf6, 0xa1, 0x0d, 0xff, 0x3c, 0x11,
    0x3c, 0x7e, 0x02, 0xb7, 0x55, 0x88, 0xbf, 0x29,
    0xd2, 0x44, 0xfb, 0x0e, 0x72, 0x2e, 0x5f, 0x1e,
    0xa0, 0x69, 0x98, 0xf5, 0xa3, 0xa4, 0xa5, 0x1b,
    0x65, 0x2d, 0x5e, 0x87, 0xca, 0xaf, 0x2f, 0x7b,
    0x46, 0xe2, 0xdc, 0x29, 0xd6, 0x61, 0xef, 0x4a,
    0x10, 0x5b, 0x41, 0xad, 0x1e, 0x98, 0x3a, 0x18,
    0x9c, 0xc2, 0x9b, 0x78, 0x0c, 0xf6, 0x6b, 0x77,
    0x40, 0x31, 0x66, 0x88, 0x33, 0xf1, 0xeb, 0xf8,
    0xf0, 0x5f, 0x28, 0x43, 0x3c, 0x1c, 0x65, 0x2e,
    0x0a, 0x4a, 0xf1, 0x40, 0x05, 0x07, 0x96, 0x0f,
    0x52, 0x91, 0x29, 0x5b, 0x87, 0x67, 0xe3, 0x44,
    0x15, 0x37, 0xb1, 0x25, 0xa4, 0xf1, 0x70, 0xec,
    0x89, 0xda, 0xe9, 0x82, 0x8f, 0x5d, 0xc8, 0xe6,
    0x23, 0xb2, 0xb4, 0x85, 0x1f, 0x60, 0x1a, 0xb2,
    0x46, 0x6a, 0xa3, 0x64, 0x90, 0x54, 0x85, 0x34,
    0x1a, 0x85, 0x2f, 0x7a, 0x1c, 0xdd, 0x06, 0x0f,
    0x42, 0xb1, 0x3b, 0x56, 0x1d, 0x02, 0xa2, 0xc1,
    0xe4, 0x68, 0x16, 0x45, 0xe4, 0xe5, 0x1d, 0xba,
    0x8d, 0x5f, 0x09, 0x05, 0x41, 0x57, 0x02, 0xd1,
    0x4a, 0xcf, 0xce, 0x9b, 0x84, 0x4e, 0xca, 0x89,
    0xdb, 0x2e, 0x74, 0xa8, 0x27, 0x94, 0xb0, 0x48,
    0x72, 0x52, 0x8b, 0xe7, 0x9c, 0xce, 0xfc, 0xb1,
    0xbc, 0xa5, 0xaf, 0x82, 0xcf, 0x29, 0x11, 0x5d,
    0x83, 0x43, 0x82, 0x6f, 0x78, 0x7c, 0xb9, 0x02
};

const uint8_t heavyP[200] = {
    0x09, 0x85, 0x24, 0xb2, 0x52, 0x4c, 0xd7, 0x3a,
    0x16, 0x42, 0x9f, 0x2f, 0x0e, 0x9b, 0x62, 0x79,
    0xee, 0xf8, 0xc7, 0x16, 0x48, 0xff, 0x14, 0x7a,
    0x98, 0x64, 0x05, 0x80, 0x4c, 0x5f, 0xa7, 0x11,
    0xda, 0xce, 0xee, 0x44, 0xdf, 0xe0, 0x20, 0xe7,
    0x69, 0x40, 0xf3, 0x14, 0x2e, 0xd8, 0xc7, 0x72,
    0xba, 0x35, 0x89, 0x93, 0x2a, 0xff, 0x00, 0xc1,
    0x62, 0xc4, 0x0f, 0x25, 0x40, 0x90, 0x21, 0x5e,
    0x48, 0x6a, 0xcf, 0x0d, 0xa6, 0xf9, 0x39, 0x80,
    0x0c, 0x3d, 0x2a, 0x79, 0x9f, 0xaa, 0xbc, 0xa0,
    0x26, 0xa2, 0xa9, 0xd0, 0x5d, 0xc0, 0x31, 0xf4,
    0x3f, 0x8c, 0xc1, 0x54, 0xc3, 0x4c, 0x1f, 0xd3,
    0x3d, 0xcc, 0x69, 0xa7, 0x01, 0x7d, 0x6b, 0x6c,
    0xe4, 0x93, 0x24, 0x56, 0xd3, 0x5b, 0xc6, 0x2e,
    0x44, 0xb0, 0xcd, 0x99, 0x3a, 0x4b, 0xf7, 0x4e,
    0xb0, 0xf2, 0x34, 0x54, 0x83, 0x86, 0x4c, 0x77,
    0x16, 0x94, 0xbc, 0x36, 0xb0, 0x61, 0xe9, 0x07,
    0x07, 0xcc, 0x65, 0x77, 0xb1, 0x1d, 0x8f, 0x7e,
    0x39, 0x6d, 0xc4, 0xba, 0x80, 0xdb, 0x8f, 0xea,
    0x58, 0xca, 0x34, 0x7b, 0xd3, 0xf2, 0x92, 0xb9,
    0x57, 0xb9, 0x81, 0x84, 0x04, 0xc5, 0x76, 0xc7,
    0x2e, 0xc2, 0x12, 0x51, 0x67, 0x9f, 0xc3, 0x47,
    0x0a, 0x0c, 0x29, 0xb5, 0x9d, 0x39, 0xbb, 0x92,
    0x15, 0xc6, 0x9f, 0x2f, 0x31, 0xe0, 0x9a, 0x54,
    0x35, 0xda, 0xb9, 0x10, 0x7d, 0x32, 0x19, 0x16
};

// XoShiRo256** PRNG for matrix generation
// CORRECTED: This is the ** variant, not the ++ variant!
class XoShiRo256StarStar {
private:
    uint64_t s[4];
    
    static inline uint64_t rotl(uint64_t x, int k) {
        return (x << k) | (x >> (64 - k));
    }
    
public:
    XoShiRo256StarStar(const uint8_t* hash) {
        for (int i = 0; i < 4; i++) {
            s[i] = 0;
            for (int j = 0; j < 8; j++) {
                s[i] |= ((uint64_t)hash[i * 8 + j]) << (j * 8);
            }
        }
    }
    
    uint64_t next() {
        // CORRECTED: (s[1] * 5 rotated by 7) * 9
        // This is the StarStar (**) variant, NOT the PlusPlus (++) variant!
        const uint64_t result = rotl(s[1] * 5, 7) * 9;
        
        const uint64_t t = s[1] << 17;
        
        s[2] ^= s[0];
        s[3] ^= s[1];
        s[1] ^= s[2];
        s[0] ^= s[3];
        s[2] ^= t;
        s[3] = rotl(s[3], 45);
        
        return result;
    }
};

struct HeavyMatrix {
    uint16_t data[64][64];
    
    void generate(const uint8_t* hash) {
        XoShiRo256StarStar rng(hash);
        for (int i = 0; i < 64; i++) {
            for (int j = 0; j < 64; j += 16) {
                uint64_t val = rng.next();
                for (int shift = 0; shift < 16; shift++) {
                    data[i][j + shift] = (val >> (4 * shift)) & 0x0F;
                }
            }
        }
    }
};

struct KHeavyHashState {
    HeavyMatrix matrix;
    uint8_t lastHash1[32];
    bool matrixValid;
    
    KHeavyHashState() : matrixValid(false) {
        memset(lastHash1, 0, 32);
    }
    
    void compute(const uint8_t* input, uint8_t* output) {
        uint8_t hash1[32];
        uint8_t vec[64];
        uint8_t product[32];

        // Verify input pointer is valid
        if (input == nullptr) {
            Serial.println("ERROR: null input to compute()");
            return;
        }

        // Step 1: First Keccak
        // CORRECTED: Should be 80 bytes (72-byte header + 8-byte nonce) - matches working version
        // Buffer is 80 bytes, zero-padded, so reading 80 is safe
        runKeccak(input, 80, hash1);
        
        // Step 2: Generate matrix if needed (cache optimization)
        if (!matrixValid || memcmp(hash1, lastHash1, 32) != 0) {
            matrix.generate(hash1);
            memcpy(lastHash1, hash1, 32);
            matrixValid = true;
        }
        
        // Step 3: Split hash into 64 nibbles (4-bit values)
        for (int i = 0; i < 32; i++) {
            vec[2 * i] = hash1[i] >> 4;          // Upper nibble
            vec[2 * i + 1] = hash1[i] & 0x0F;    // Lower nibble
        }
        
        // Step 4: Matrix-vector multiplication
        // CORRECTED: Different shift values and masking for sum1 and sum2!
        for (int i = 0; i < 32; i++) {
            uint16_t sum1 = 0;
            uint16_t sum2 = 0;
            
            // Multiply two matrix rows by vector
            for (int j = 0; j < 64; j++) {
                sum1 += matrix.data[2 * i][j] * vec[j];
                sum2 += matrix.data[2 * i + 1][j] * vec[j];
            }
            
            // CORRECTED: Different shifts and masks for sum1 vs sum2
            // sum1 uses shift 6 and keeps upper nibble (0xF0)
            // sum2 uses shift 10 and keeps lower nibble (0x0F)
            uint8_t upper = (sum1 >> 6) & 0xF0;   // Shift 6, mask upper nibble
            uint8_t lower = (sum2 >> 10) & 0x0F;  // Shift 10, mask lower nibble
            product[i] = upper | lower;
        }
        
        // Step 5: XOR with original hash
        for (int i = 0; i < 32; i++) {
            product[i] ^= hash1[i];
        }
        
        // Step 6: Final Keccak
        runKeccak(product, 32, output);
    }
};

KHeavyHashState kheavyState;

// Memory guard canaries to detect corruption
#define CANARY_VALUE 0xDEADBEEF
static uint32_t canary_before = CANARY_VALUE;
static KeccakCore miningKeccak;  // Single instance for mining (Core 1 only)
static uint32_t canary_after = CANARY_VALUE;

// ==================== END KHEAVYHASH ====================

// ==================== COLOR PALETTE ====================
// Professional triadic color scheme following 60-30-10 rule
// 60% Neutral (blacks/deep navy), 30% Kaspa Green (brand/active), 10% Accent (orange/purple)

// Primary Brand Color (30% - Use for active states, primary CTAs, key highlights)
#define COLOR_KASPA_GREEN    0x49EACB  // Vibrant turquoise - the brand identity

// Accent Colors (10% - Use sparingly for emphasis)
#define COLOR_VIVID_ORANGE   0xFF6B35  // High-contrast CTA buttons, warnings, highlights
#define COLOR_DEEP_PURPLE    0x6B4C9A  // Depth/sophistication, secondary accents, chart dots

// Neutral Colors (60% - Backgrounds and structure)
#define COLOR_PURE_BLACK     0x000000  // Main backgrounds (screen already looks grayish)
#define COLOR_DEEP_NAVY      0x0F1419  // Alternative dark background for depth
#define COLOR_SLATE_GRAY     0x4A5568  // Borders on non-interactive elements

// Text Hierarchy
#define COLOR_TEXT_PRIMARY   0xFFFFFF  // White - Primary text, important data
#define COLOR_TEXT_SECONDARY 0x888888  // Medium gray - Labels, secondary info
#define COLOR_TEXT_DIM       0x555555  // Darker gray - Less important text

// Status Colors
#define COLOR_SUCCESS_GREEN  0x00FF00  // Bright green - Success states
#define COLOR_ERROR_RED      0xFF0000  // Red - Errors, destructive actions
#define COLOR_WARNING_YELLOW 0xFFAA00  // Golden yellow - Warnings

// ==================== END COLOR PALETTE ====================

// --- DASHBOARD GLOBALS ---
float networkHashrate = 0.0;
String price_usd = "Loading...", change_24h = "";
String market_cap = "Loading...";

// Price animation
float currentAnimatedPrice = 0.0;
float previousPrice = 0.0;

// Price history for 24-hour chart (48 points = 30min intervals)
#define PRICE_HISTORY_POINTS 48
float priceHistory[PRICE_HISTORY_POINTS];
uint64_t priceTimestamps[PRICE_HISTORY_POINTS] = {0};  // Use 64-bit to avoid overflow with milliseconds
int priceIndex = 0;
bool priceHistoryInitialized = false;

WebServer server(80); // Create web server on port 80

// ==================== OTA UPDATE SYSTEM ====================
// Firmware version - UPDATE THIS WITH EACH RELEASE
#define FIRMWARE_VERSION "1.0.6"
#define FIRMWARE_NAME "KASDeck"
#define HARDWARE_VERSION "1.0"  // Hardware compatibility version
#define MIN_FIRMWARE_VERSION "1.0.0"  // Minimum version that can be upgraded from

// Firmware header magic bytes for validation
#define FIRMWARE_MAGIC 0x4B41534B  // "KASK" in hex

// GitHub configuration - UPDATE WITH YOUR DETAILS
#define GITHUB_USER "proofofprints"
#define GITHUB_REPO "kasdeck"
#define GITHUB_API_URL "https://api.github.com/repos/" GITHUB_USER "/" GITHUB_REPO "/releases/latest"

// Update check interval (7 days)
#define UPDATE_CHECK_INTERVAL 604800000UL  // 7 days in milliseconds

struct FirmwareInfo {
  String version;
  String downloadUrl;
  String changelog;
  int fileSize;
  bool updateAvailable;
};

FirmwareInfo latestFirmware;
// Note: FirmwareHeader struct is defined at top of file after includes
unsigned long lastUpdateCheck = 0;
bool updateInProgress = false;
int updateProgress = 0;
// ==================== END OTA CONFIGURATION ====================

// ==================== SECURITY HELPERS ====================
// Mask sensitive data for logging (shows first 10 and last 4 chars)
String maskWallet(const String& wallet) {
  if (wallet.length() < 20) return "***";
  return wallet.substring(0, 10) + "..." + wallet.substring(wallet.length() - 4);
}

// ==================== AUTHENTICATION SYSTEM ====================
// Simple session-based authentication for web dashboard

#define MAX_AUTH_SESSIONS 5
#define AUTH_TOKEN_LENGTH 32

// Active session tokens (stored in memory, cleared on reboot)
String activeTokens[MAX_AUTH_SESSIONS];
int activeTokenCount = 0;

// Forward declarations for auth functions
bool checkAuth();
bool isPasswordSet();
String generateToken();
void handleLogin();
void handleApiAuth();
void handleApiAuthStatus();
void handleApiAuthSetup();
void handleApiAuthLogout();
void handleSetupPassword();

// Generate a random authentication token
String generateToken() {
  const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  String token = "";
  for (int i = 0; i < AUTH_TOKEN_LENGTH; i++) {
    token += charset[random(0, sizeof(charset) - 1)];
  }
  return token;
}

// Default web UI password (same as AP password for simplicity)
#define DEFAULT_WEB_PASSWORD "kaspa123"

// Get device password from NVS (returns default if not set)
String getDevicePassword() {
  Preferences prefs;
  prefs.begin("kaspa", true);  // Read-only
  String pwd = prefs.getString("devicePassword", DEFAULT_WEB_PASSWORD);
  prefs.end();
  return pwd.length() > 0 ? pwd : DEFAULT_WEB_PASSWORD;
}

// Check if device password is set in NVS (always true now since we have default)
bool isPasswordSet() {
  return true;  // Always have a password (default or custom)
}

// Add a new session token (replaces oldest if at max)
void addSessionToken(const String& token) {
  if (activeTokenCount < MAX_AUTH_SESSIONS) {
    activeTokens[activeTokenCount++] = token;
  } else {
    // Replace oldest token (index 0), shift others down
    for (int i = 0; i < MAX_AUTH_SESSIONS - 1; i++) {
      activeTokens[i] = activeTokens[i + 1];
    }
    activeTokens[MAX_AUTH_SESSIONS - 1] = token;
  }
}

// Remove a session token
void removeSessionToken(const String& token) {
  for (int i = 0; i < activeTokenCount; i++) {
    if (activeTokens[i] == token) {
      // Shift remaining tokens down
      for (int j = i; j < activeTokenCount - 1; j++) {
        activeTokens[j] = activeTokens[j + 1];
      }
      activeTokens[--activeTokenCount] = "";
      return;
    }
  }
}

// Check if a token is valid
bool isValidToken(const String& token) {
  if (token.length() == 0) return false;
  for (int i = 0; i < activeTokenCount; i++) {
    if (activeTokens[i] == token) {
      return true;
    }
  }
  return false;
}

// Check authentication - returns true if authenticated
// Checks X-Auth-Token header or authToken cookie
bool checkAuth() {
  // Check X-Auth-Token header first
  if (server.hasHeader("X-Auth-Token")) {
    String token = server.header("X-Auth-Token");
    if (isValidToken(token)) {
      return true;
    }
  }

  // Check Cookie header for authToken
  if (server.hasHeader("Cookie")) {
    String cookies = server.header("Cookie");
    int tokenStart = cookies.indexOf("authToken=");
    if (tokenStart >= 0) {
      tokenStart += 10;  // Length of "authToken="
      int tokenEnd = cookies.indexOf(";", tokenStart);
      if (tokenEnd < 0) tokenEnd = cookies.length();
      String token = cookies.substring(tokenStart, tokenEnd);
      if (isValidToken(token)) {
        return true;
      }
    }
  }

  return false;
}

// Send 401 Unauthorized response as JSON
void sendUnauthorized() {
  server.send(401, "application/json", "{\"error\":\"Unauthorized\",\"message\":\"Authentication required\"}");
}

// Redirect to login page with optional return URL
void redirectToLogin() {
  String returnUrl = server.uri();
  if (returnUrl == "/" || returnUrl.isEmpty()) {
    server.sendHeader("Location", "/login");
  } else {
    server.sendHeader("Location", "/login?return=" + returnUrl);
  }
  server.send(302, "text/plain", "");
}

// Redirect to password setup page
void redirectToSetup() {
  server.sendHeader("Location", "/setup-password");
  server.send(302, "text/plain", "");
}

// Check auth for protected endpoint - returns true if should continue
// Returns false and sends appropriate response if not authenticated
bool requireAuth() {
  // If password not set, redirect to setup
  if (!isPasswordSet()) {
    redirectToSetup();
    return false;
  }

  // Check if authenticated
  if (!checkAuth()) {
    // For API endpoints (JSON), send 401
    // For HTML endpoints, redirect to login
    String uri = server.uri();
    if (uri.startsWith("/api/")) {
      sendUnauthorized();
    } else {
      redirectToLogin();
    }
    return false;
  }

  return true;
}

// ==================== END AUTHENTICATION SYSTEM ====================

WiFiClient stratumClient;
bool isAuthorized = false;
unsigned long lastStratumCheck = 0;

// The Max Target (Difficulty 1) for Kaspa Stratum
const uint8_t DIFF_1_TARGET[32] = {
    0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// Mining configuration
String minerWalletAddress = "";
String minerPoolUrl = "stratum+tcp://pool.proofofprints.com:5555";  // Default pool
int timezoneOffsetHours = -6;  // Central Time (UTC-6), stored in hours for easier UI

// Multiple NTP servers for fallback
const char* ntpServers[] = {
  "pool.ntp.org",
  "time.nist.gov",
  "time.google.com",
  "time.cloudflare.com"
};
const int numNtpServers = 4;
String minerPoolUser = "";
bool miningEnabled = false;
bool poolConnected = false;

// DEBUG LOGGING CONTROL
// Set to true to enable detailed SD card logging (costs ~5-10% hashrate)
// Set to false for maximum hashrate performance
const bool ENABLE_DEBUG_LOGGING = false;

// First boot wizard
bool firstBoot = true;
int wizardStep = 0;  // 0=welcome, 1=wifi setup, 2=wifi connect, 3=mining setup
lv_obj_t *wizardScreen = nullptr;

// Mining stats
uint64_t totalHashes = 0;
// Mining stats (accessed from both cores - use volatile to prevent optimization issues)
volatile uint32_t sharesSubmitted = 0;
volatile uint32_t sharesAccepted = 0;
volatile uint32_t sharesRejected = 0;
volatile uint32_t sharesStale = 0;      // Stale shares (job too old)
volatile uint32_t sharesDuplicate = 0;  // Duplicate shares
volatile uint32_t sharesInvalid = 0;    // Invalid shares (bad data)
volatile uint32_t blocksFound = 0;      // Blocks found!
volatile float currentHashrate = 0;
unsigned long miningStartTime = 0;
unsigned long totalMiningTime = 0;  // Cumulative mining time in seconds
unsigned long lastMiningStateChange = 0;  // When mining was last started/stopped
String miningStartedTimestamp = "";  // Human-readable timestamp when mining started
volatile uint32_t jobsReceived = 0;  // Track total jobs received from pool

// Mining hashrate history (local miner)
#define MINER_HASHRATE_POINTS 12  // 1 hour at 5 minute intervals (was 60 at 1 min)
float minerHashrateHistory[MINER_HASHRATE_POINTS] = {0};
unsigned long minerHashrateTimestamps[MINER_HASHRATE_POINTS] = {0};  // Track timestamps
int minerHashrateIndex = 0;
unsigned long lastMinerHashrateUpdate = 0;

// Mining event log
#define MAX_LOG_ENTRIES 20
struct MiningLogEntry {
  String timestamp;
  String event;
  String detail;
};
MiningLogEntry miningLog[MAX_LOG_ENTRIES];
int logIndex = 0;

// Forward declaration for system log function
void addLog(String message, String type);

// Timezone offset in seconds (Central Time = UTC-6, or UTC-5 during DST)
// Calculated from timezoneOffsetHours variable (configured in settings)

void addMiningLog(String event, String detail) {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    // Use actual date/time with timezone
    char timeStr[32];
    strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
    miningLog[logIndex].timestamp = String(timeStr);
  } else {
    // Fallback to uptime if NTP not synced yet
    char timeStr[32];
    unsigned long uptime = (millis() - miningStartTime) / 1000;
    sprintf(timeStr, "%02lu:%02lu:%02lu", uptime / 3600, (uptime % 3600) / 60, uptime % 60);
    miningLog[logIndex].timestamp = String(timeStr);
  }
  
  miningLog[logIndex].event = event;
  miningLog[logIndex].detail = detail;
  logIndex = (logIndex + 1) % MAX_LOG_ENTRIES;

  // Also print to serial (removed addLog to avoid heap fragmentation during mining)
  String logMessage = event;
  if (detail.length() > 0) {
    logMessage += ": " + detail;
  }
  Serial.println("[MINING] " + logMessage);
}

// Get current cumulative mining time in seconds
unsigned long getCurrentMiningTime() {
  if (!miningEnabled) {
    return totalMiningTime;  // If stopped, return cumulative total
  } else {
    // If mining, add current session time to cumulative total
    return totalMiningTime + ((millis() - lastMiningStateChange) / 1000);
  }
}


// SD Card Debug Logging - writes timestamped messages to /debug.txt
void logToSD(const char* message) {
    // Conditional logging: only write to SD if debug logging is enabled
    if (!ENABLE_DEBUG_LOGGING) return;

    File debugLog = SD.open("/debug.txt", FILE_APPEND);
    if (debugLog) {
        unsigned long ms = millis();
        debugLog.printf("[%lu] %s\n", ms, message);
        debugLog.flush();  // Critical: force write immediately
        debugLog.close();
    }
}

// Log detailed hash call state for debugging crashes
void logHashState(int callNum, const uint8_t* input, size_t len) {
    // Conditional logging: only write to SD if debug logging is enabled
    if (!ENABLE_DEBUG_LOGGING) return;

    File debugLog = SD.open("/debug.txt", FILE_APPEND);
    if (debugLog) {
        debugLog.printf("[%lu] HASH #%d: len=%d, input=%p, miningKeccak=%p\n",
                       millis(), callNum, len, input, &miningKeccak);
        debugLog.printf("  Canaries: before=0x%08X, after=0x%08X\n",
                       canary_before, canary_after);
        debugLog.printf("  First 16 bytes: ");
        for (int i = 0; i < 16 && i < len; i++) {
            debugLog.printf("%02x ", input[i]);
        }
        debugLog.println();
        debugLog.flush();
        debugLog.close();
    }
}

// --- MINING HELPER FUNCTIONS ---

void runKeccak(const uint8_t* input, size_t len, uint8_t* output) {
    // Check canaries periodically (every 500 calls, OUTSIDE critical section)
    static int check_interval = 0;
    if (++check_interval % 500 == 0) {
        if (canary_before != CANARY_VALUE || canary_after != CANARY_VALUE) {
            // DISABLED: SD logging causes crashes during mining
            // char msg[150];
            // sprintf(msg, "CANARY CORRUPTION at call %d: before=0x%08X, after=0x%08X",
            //        check_interval, canary_before, canary_after);
            // logToSD(msg);
            Serial.printf("CANARY CORRUPTION at call %d: before=0x%08X, after=0x%08X\n",
                         check_interval, canary_before, canary_after);
        }
    }

    // Critical section - NO LOGGING INSIDE (SD writes too slow, cause watchdog abort)
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    portENTER_CRITICAL(&mux);

    miningKeccak.reset();
    miningKeccak.setCapacity(512);
    miningKeccak.update(input, len);  // Original crash location
    miningKeccak.pad(0x01);
    miningKeccak.extract(output, 32);

    portEXIT_CRITICAL(&mux);
}

void hexToBytes(String hex, uint8_t* bytes) {
    for (unsigned int i = 0; i < hex.length() / 2 && i < 32; i++) {
        String byteString = hex.substring(i * 2, i * 2 + 2);
        bytes[i] = (uint8_t) strtol(byteString.c_str(), NULL, 16);
    }
}


// Returns true if hash <= target (Meaning a share was found!)
inline bool checkDifficulty(uint8_t* hash, uint8_t* target) {
    for (int i = 0; i < 32; i++) {
        if (hash[i] < target[i]) return true;
        if (hash[i] > target[i]) return false;
    }
    return true; // Exactly equal
}

void submitShare(String jobId, uint64_t nonce) {
    // Convert nonce to hex string (Little Endian as required by most Stratum pools)
    char nonceHex[17];
    sprintf(nonceHex, "%016llx", nonce);

    // Create the JSON-RPC submission message
    // {"id": 4, "method": "mining.submit", "params": ["wallet.worker", "job_id", "nonce"]}
    String msg = "{\"id\": 4, \"method\": \"mining.submit\", \"params\": [\"" +
                 minerWalletAddress + "\", \"" + jobId + "\", \"" + String(nonceHex) + "\"]}\n";

    // Log what we're sending
    Serial.println("\n>>> SUBMITTING SHARE:");
    Serial.printf("Job ID: %s\n", jobId.c_str());
    Serial.printf("Nonce:  %s\n", nonceHex);
    Serial.printf("Full message: %s", msg.c_str());
    Serial.println("Waiting for response...\n");

    stratumClient.print(msg);
    sharesSubmitted++; // Increment global counter
    addMiningLog("SHARE SUBMITTED", "Nonce: " + String(nonceHex));
}

void setTargetFromDifficulty(double difficulty) {
    // Support fractional difficulty for ESP32 mining!
    if (difficulty <= 0) difficulty = 0.0001;  // Default to very low for hobby mining
    
    currentDifficulty = difficulty;  // Save to global
    
    // For Kaspa, difficulty 1 target is: 00000000ffff0000000000000000000000000000000000000000000000000000
    // For lower difficulty (easier), we scale UP the target
    
    // Start with difficulty 1 target (max target for diff 1)
    uint8_t diff1Target[32] = {
        0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    if (difficulty >= 1.0) {
        // Difficulty 1.0 or higher - use standard target or divide down
        memcpy(currentTarget, diff1Target, 32);
        // TODO: For diff > 1, would need to divide target (make harder)
    } else {
        // Difficulty < 1.0 (fractional) - multiply target (make easier!)
        // For diff 0.0001, target should be ~10000x larger
        
        // Simple scaling for low difficulties
        // Target = diff1Target / difficulty
        // For 0.0001: multiply the significant bytes by ~10000
        
        double scaleFactor = 1.0 / difficulty;  // 0.0001 -> 10000
        
        // Start with diff1 target, then scale up the significant bytes
        memcpy(currentTarget, diff1Target, 32);
        
        // The significant part is bytes 4-5 (0xffff in diff1)
        // Scale them up for easier difficulty
        uint32_t baseValue = 0xffff;  // From diff 1
        uint64_t scaledValue = (uint64_t)(baseValue * scaleFactor);
        
        // Cap at reasonable maximum to avoid overflow
        if (scaledValue > 0xFFFFFFFFFFFF) scaledValue = 0xFFFFFFFFFFFF;
        
        // Write scaled value in big-endian (most significant first)
        currentTarget[0] = (scaledValue >> 40) & 0xFF;
        currentTarget[1] = (scaledValue >> 32) & 0xFF;
        currentTarget[2] = (scaledValue >> 24) & 0xFF;
        currentTarget[3] = (scaledValue >> 16) & 0xFF;
        currentTarget[4] = (scaledValue >> 8) & 0xFF;
        currentTarget[5] = scaledValue & 0xFF;
        // Rest stays 0x00
    }

    // Debug output
    Serial.print("Target (full): ");
    for (int i = 0; i < 32; i++) Serial.printf("%02x", currentTarget[i]);
    Serial.println();
    Serial.printf("Target (first 8): %02x%02x%02x%02x%02x%02x%02x%02x\n", 
                  currentTarget[0], currentTarget[1], currentTarget[2], currentTarget[3],
                  currentTarget[4], currentTarget[5], currentTarget[6], currentTarget[7]);
    Serial.printf("New Target set for Diff: %.2f\n", difficulty);
}

#define MAX_WIFI_NETWORKS 10
String wifiNetworks[MAX_WIFI_NETWORKS];
int wifiSignals[MAX_WIFI_NETWORKS];
int wifiCount = 0;

// WiFi scan state tracking
volatile bool wifiScanInProgress = false;
volatile bool wifiScanComplete = false;

// Price data refresh flag (set when user taps price card)
volatile bool priceRefreshRequested = false;

// API Endpoints
const char* HEROMINERS_STATS = "http://kaspa.herominers.com/api/stats";

// LVGL label objects
lv_obj_t *priceLabel = nullptr;
lv_obj_t *changeLabel = nullptr;
lv_obj_t *statusLabel = nullptr;
lv_obj_t *poolStatusLabel = nullptr;  // Pool connection status on config screen
lv_obj_t *hashrateLabel = nullptr;
lv_obj_t *networkLabel = nullptr;
lv_obj_t *bigHashrateLabel = nullptr;
lv_obj_t *loadingSpinner = nullptr; 

lv_obj_t *yAxisMinLabel = nullptr;
lv_obj_t *yAxisMidLabel = nullptr;
lv_obj_t *yAxisMaxLabel = nullptr;
lv_obj_t *xAxisStartLabel = nullptr;
lv_obj_t *xAxisEndLabel = nullptr;

//Miner configuration
lv_obj_t *minerConfigMenu = nullptr;
lv_obj_t *walletTextarea = nullptr;
lv_obj_t *poolDropdown = nullptr;
lv_obj_t *miningToggle = nullptr;
lv_obj_t *minerStatusLabel = nullptr;
lv_obj_t *minerHashrateLabel = nullptr;
lv_obj_t *minerSharesLabel = nullptr;
lv_obj_t *minerJobsLabel = nullptr;
lv_obj_t *minerSubmittedLabel = nullptr;
lv_obj_t *minerAcceptedLabel = nullptr;
lv_obj_t *minerRejectedLabel = nullptr;
lv_obj_t *minerBlocksLabel = nullptr;  // Blocks found label

//keyboard control
lv_obj_t *currentKeyboard = nullptr;
unsigned long keyboardCloseTime = 0;  // Track when keyboard closed for touch blocking

// Touch configuration
#define TOUCH_GT911_SDA 19
#define TOUCH_GT911_SCL 20
#define TOUCH_GT911_INT -1
#define TOUCH_GT911_RST -1
#define TOUCH_GT911_ROTATION ROTATION_NORMAL

TAMC_GT911 ts = TAMC_GT911(TOUCH_GT911_SDA, TOUCH_GT911_SCL, TOUCH_GT911_INT, TOUCH_GT911_RST, 800, 480);
Preferences preferences;

class LGFX : public lgfx::LGFX_Device {
  lgfx::Bus_RGB _bus_instance;
  lgfx::Panel_RGB _panel_instance;
  lgfx::Light_PWM _light_instance;

public:
  LGFX(void) {
    { auto cfg = _bus_instance.config();
      cfg.panel = &_panel_instance;
      cfg.pin_d0  = 15; cfg.pin_d1  = 7; cfg.pin_d2  = 6; cfg.pin_d3  = 5;
      cfg.pin_d4  = 4; cfg.pin_d5  = 9; cfg.pin_d6  = 46; cfg.pin_d7  = 3;
      cfg.pin_d8  = 8; cfg.pin_d9  = 16; cfg.pin_d10 = 1; cfg.pin_d11 = 14;
      cfg.pin_d12 = 21; cfg.pin_d13 = 47; cfg.pin_d14 = 48; cfg.pin_d15 = 45;
      cfg.pin_henable = 41;
      cfg.pin_vsync   = 40;
      cfg.pin_hsync   = 39;
      cfg.pin_pclk    = 0;
      cfg.freq_write = 15000000;
      cfg.hsync_polarity = 0; cfg.hsync_front_porch = 40;
      cfg.hsync_pulse_width = 48; cfg.hsync_back_porch = 40;
      cfg.vsync_polarity = 0; cfg.vsync_front_porch = 1;
      cfg.vsync_pulse_width = 31; cfg.vsync_back_porch = 13;
      cfg.pclk_active_neg = 1;
      cfg.de_idle_high = 0;
      cfg.pclk_idle_high = 0;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance); }

    { auto cfg = _panel_instance.config();
      cfg.memory_width = 800;
      cfg.memory_height = 480;
      cfg.panel_width = 800;
      cfg.panel_height = 480;
      _panel_instance.config(cfg); }

    { auto cfg = _light_instance.config();
      cfg.pin_bl = 2;
      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance); }

    setPanel(&_panel_instance);
  }
};

LGFX tft;
WiFiManager wm;

static lv_disp_draw_buf_t draw_buf;
// Small static buffer for stable rendering (original working configuration)
static lv_color_t buf[800 * 10];

//lv_obj_t *priceLabel, *changeLabel, *statusLabel;
lv_obj_t *hashrateChart;
lv_chart_series_t *hashrateSeries;
lv_obj_t *currentHashLabel, *avgHashLabel, *maxHashLabel;
lv_obj_t *settingsMenu = nullptr;

// Mining stats screen labels (for live updates)
lv_obj_t *miningStatsDeviceHashLabel = nullptr;
lv_obj_t *miningStatsNetworkHashLabel = nullptr;
lv_obj_t *miningStatsAcceptedLabel = nullptr;
lv_obj_t *miningStatsRejectedLabel = nullptr;
lv_obj_t *miningStatsBlocksLabel = nullptr;

//wifi screen configuration variables
lv_obj_t *wifiConfigMenu = nullptr;
lv_obj_t *wifiList = nullptr;
lv_obj_t *passwordTextarea = nullptr;
lv_obj_t *ssidDisplayLabel = nullptr;  // Selected SSID display
lv_obj_t *scanLabel = nullptr;  // WiFi scan status label
lv_obj_t *selectedWifiButton = nullptr;  // Currently selected WiFi button for highlighting
String selectedSSID = "";

// WiFi connection state machine
enum WiFiConnectionState {
  WIFI_IDLE,
  WIFI_CONNECTING,
  WIFI_CONNECTED,
  WIFI_FAILED
};
WiFiConnectionState wifiState = WIFI_IDLE;
lv_obj_t *connectingOverlay = nullptr;
lv_obj_t *connectingStatusLabel = nullptr;
String pendingPassword = "";
unsigned long wifiConnectStartTime = 0;

bool hashrateAPIWorking = true;
lv_obj_t *apiStatusLabel;  // Error indicator label

// Firmware update flag
bool firmwareUpdateInProgress = false;

// Menu navigation screens
lv_obj_t *mainMenu = nullptr;
lv_obj_t *priceInfoScreen = nullptr;
lv_obj_t *miningStatsScreen = nullptr;

float hashrate_val = 0;
unsigned long last_update = 0;

#define HASHRATE_POINTS 24  // 24 points = 12 hours at 30min intervals
float hashrateHistory[HASHRATE_POINTS];
unsigned long hashrateTimestamps[HASHRATE_POINTS] = {0};  // Track timestamps
int hashrateIndex = 0;

void my_disp_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = lv_area_get_width(area);
  uint32_t h = lv_area_get_height(area);
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.writePixels((lgfx::rgb565_t*)color_p, w * h);
  tft.endWrite();
  lv_disp_flush_ready(drv);
}


void my_touch_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  static bool wasTouched = false;
  static unsigned long touchStartTime = 0;
  static unsigned long touchReleaseTime = 0;  // Track when touch was released
  static int lastX = -1, lastY = -1;
  static int releaseX = -1, releaseY = -1;    // Track where finger lifted

  // Block all touch input for 400ms after keyboard closes to prevent "fall through"
  if (keyboardCloseTime > 0 && (millis() - keyboardCloseTime) < 400) {
    data->state = LV_INDEV_STATE_REL;
    wasTouched = false;
    return;
  }

  ts.read();

  if (ts.isTouched) {
    int currentX = map(ts.points[0].x, 800, 0, 0, 799);
    int currentY = map(ts.points[0].y, 480, 0, 0, 479);

    if (!wasTouched) {
      // New touch started - LOG IT for debugging
      wasTouched = true;
      touchStartTime = millis();
      touchReleaseTime = 0;
      lastX = currentX;
      lastY = currentY;

      // Store coordinates globally for keyboard event callback to check
      extern int lastTouchX, lastTouchY;
      lastTouchX = currentX;
      lastTouchY = currentY;

      data->state = LV_INDEV_STATE_PR;
      data->point.x = currentX;
      data->point.y = currentY;
    } else {
      // Touch continuing - check if it moved significantly
      int deltaX = abs(currentX - lastX);
      int deltaY = abs(currentY - lastY);

      if (deltaX > 10 || deltaY > 10) {
        // Moved - update position (drag)
        lastX = currentX;
        lastY = currentY;
        data->state = LV_INDEV_STATE_PR;
        data->point.x = currentX;
        data->point.y = currentY;
      } else {
        // Still touching same spot - maintain press state
        data->state = LV_INDEV_STATE_PR;
        data->point.x = lastX;
        data->point.y = lastY;
      }
    }
  } else {
    // Touch released
    if (wasTouched) {
      unsigned long touchDuration = millis() - touchStartTime;

      // Debounce: ignore very short touches (< 50ms) - likely noise
      if (touchDuration < 50) {
        data->state = LV_INDEV_STATE_REL;
        wasTouched = false;
        return;
      }

      // Record where and when the touch was released
      releaseX = lastX;
      releaseY = lastY;
      touchReleaseTime = millis();
      wasTouched = false;
    }
    data->state = LV_INDEV_STATE_REL;
  }
}

void close_settings_event_cb(lv_event_t * e) {
  if (e->code == LV_EVENT_CLICKED && settingsMenu) {
    lv_obj_del(settingsMenu);
    settingsMenu = nullptr;
  }
}

void system_config_btn_cb(lv_event_t *e) {
  if (e->code == LV_EVENT_CLICKED) {
    if (settingsMenu) {
      lv_obj_del(settingsMenu);
      settingsMenu = nullptr;
    }
    show_system_config_menu();
  }
}

// WiFi Reset confirmation callback
void wifi_reset_confirm_cb(lv_event_t *e) {
  if (e->code == LV_EVENT_CLICKED) {
    playBeep(600, 100);
    
    lv_color_t kaspa_green = lv_color_hex(0x49EACB);
    lv_color_t orange = lv_color_hex(0xFF9900);
    
    lv_obj_t *confirmOverlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(confirmOverlay, 800, 480);
    lv_obj_set_pos(confirmOverlay, 0, 0);
    lv_obj_set_style_bg_color(confirmOverlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(confirmOverlay, LV_OPA_90, 0);
    lv_obj_set_style_border_width(confirmOverlay, 0, 0);
    lv_obj_clear_flag(confirmOverlay, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_t *confirmBox = lv_obj_create(confirmOverlay);
    lv_obj_set_size(confirmBox, 450, 270);  // Increased height for spacing
    lv_obj_align(confirmBox, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(confirmBox, 0, 0);
    lv_obj_set_style_bg_color(confirmBox, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(confirmBox, 3, 0);
    lv_obj_set_style_border_color(confirmBox, orange, 0);
    lv_obj_clear_flag(confirmBox, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_t *titleLabel = lv_label_create(confirmBox);
    lv_label_set_text(titleLabel, "Reset WiFi?");
    lv_obj_set_style_text_font(titleLabel, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(titleLabel, orange, 0);
    lv_obj_align(titleLabel, LV_ALIGN_TOP_MID, 0, 30);  // Centered
    
    lv_obj_t *msgLabel = lv_label_create(confirmBox);
    lv_label_set_text(msgLabel, "WiFi credentials will be cleared.\nYou will need to reconfigure.\n\n\nContinue?");  // Added extra line
    lv_obj_set_style_text_font(msgLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(msgLabel, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_align(msgLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(msgLabel, LV_ALIGN_TOP_MID, 0, 75);  // Centered, more space from title
    lv_obj_set_width(msgLabel, 380);
    lv_label_set_long_mode(msgLabel, LV_LABEL_LONG_WRAP);
    
    lv_obj_t *yesBtn = lv_btn_create(confirmBox);
    lv_obj_set_size(yesBtn, 170, 45);
    lv_obj_align(yesBtn, LV_ALIGN_BOTTOM_LEFT, 40, -25);  // 40px from left edge
    lv_obj_set_style_radius(yesBtn, 0, 0);
    lv_obj_set_style_bg_color(yesBtn, orange, 0);
    lv_obj_set_style_border_width(yesBtn, 0, 0);
    // Button press effect - invert colors and scale down
    lv_obj_set_style_bg_color(yesBtn, lv_color_hex(0x000000), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(yesBtn, 2, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(yesBtn, orange, LV_STATE_PRESSED);
    lv_obj_add_event_cb(yesBtn, [](lv_event_t *e) {
      if (e->code == LV_EVENT_CLICKED) {
        playBeep(800, 100);
        preferences.begin("kaspa", false);
        preferences.putBool("wifiReset", true);
        preferences.end();
        ESP.restart();
      }
    }, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *yesLabel = lv_label_create(yesBtn);
    lv_label_set_text(yesLabel, "YES");
    lv_obj_set_style_text_font(yesLabel, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(yesLabel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_color(yesLabel, orange, LV_STATE_PRESSED);  // Inverted on press
    lv_obj_center(yesLabel);
    
    lv_obj_t *noBtn = lv_btn_create(confirmBox);
    lv_obj_set_size(noBtn, 170, 45);
    lv_obj_align(noBtn, LV_ALIGN_BOTTOM_RIGHT, -40, -25);  // 40px from right edge (gives 30px gap in middle)
    lv_obj_set_style_radius(noBtn, 0, 0);
    lv_obj_set_style_bg_color(noBtn, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(noBtn, 2, 0);
    lv_obj_set_style_border_color(noBtn, kaspa_green, 0);
    // Button press effect - scale down
    lv_obj_set_style_bg_color(noBtn, kaspa_green, LV_STATE_PRESSED);
    lv_obj_add_event_cb(noBtn, [](lv_event_t *e) {
      if (e->code == LV_EVENT_CLICKED) {
        playBeep(800, 50);
        lv_obj_t *overlay = lv_obj_get_parent(lv_obj_get_parent(lv_event_get_target(e)));
        lv_obj_del(overlay);
      }
    }, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *noLabel = lv_label_create(noBtn);
    lv_label_set_text(noLabel, "CANCEL");
    lv_obj_set_style_text_font(noLabel, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(noLabel, kaspa_green, 0);
    lv_obj_set_style_text_color(noLabel, lv_color_hex(0x000000), LV_STATE_PRESSED);  // Inverted on press
    lv_obj_center(noLabel);
  }
}

// Restart System confirmation callback
void restart_confirm_cb(lv_event_t *e) {
  if (e->code == LV_EVENT_CLICKED) {
    playBeep(600, 100);
    
    lv_color_t kaspa_green = lv_color_hex(0x49EACB);
    lv_color_t blue = lv_color_hex(0x4A90E2);
    
    lv_obj_t *confirmOverlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(confirmOverlay, 800, 480);
    lv_obj_set_pos(confirmOverlay, 0, 0);
    lv_obj_set_style_bg_color(confirmOverlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(confirmOverlay, LV_OPA_90, 0);
    lv_obj_set_style_border_width(confirmOverlay, 0, 0);
    lv_obj_clear_flag(confirmOverlay, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_t *confirmBox = lv_obj_create(confirmOverlay);
    lv_obj_set_size(confirmBox, 450, 250);  // Increased height
    lv_obj_align(confirmBox, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(confirmBox, 0, 0);
    lv_obj_set_style_bg_color(confirmBox, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(confirmBox, 3, 0);
    lv_obj_set_style_border_color(confirmBox, blue, 0);
    lv_obj_clear_flag(confirmBox, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_t *titleLabel = lv_label_create(confirmBox);
    lv_label_set_text(titleLabel, "Restart System?");
    lv_obj_set_style_text_font(titleLabel, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(titleLabel, blue, 0);
    lv_obj_align(titleLabel, LV_ALIGN_TOP_MID, 0, 30);  // Centered
    
    lv_obj_t *msgLabel = lv_label_create(confirmBox);
    lv_label_set_text(msgLabel, "The device will reboot.\n\n\nAre you sure?");  // Added extra line
    lv_obj_set_style_text_font(msgLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(msgLabel, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_align(msgLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(msgLabel, LV_ALIGN_TOP_MID, 0, 85);  // More space from title
    lv_obj_set_width(msgLabel, 380);
    lv_label_set_long_mode(msgLabel, LV_LABEL_LONG_WRAP);
    
    lv_obj_t *yesBtn = lv_btn_create(confirmBox);
    lv_obj_set_size(yesBtn, 170, 45);
    lv_obj_align(yesBtn, LV_ALIGN_BOTTOM_LEFT, 40, -25);  // 40px from left edge
    lv_obj_set_style_radius(yesBtn, 0, 0);
    lv_obj_set_style_bg_color(yesBtn, blue, 0);
    lv_obj_set_style_border_width(yesBtn, 0, 0);
    // Button press effect - scale down
    lv_obj_set_style_bg_color(yesBtn, lv_color_hex(0x000000), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(yesBtn, 2, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(yesBtn, blue, LV_STATE_PRESSED);
    lv_obj_add_event_cb(yesBtn, [](lv_event_t *e) {
      if (e->code == LV_EVENT_CLICKED) {
        playBeep(1000, 100);
        showSaveConfirmation();  // Show brief confirmation
        delay(1000);
        ESP.restart();
      }
    }, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *yesLabel = lv_label_create(yesBtn);
    lv_label_set_text(yesLabel, "YES");
    lv_obj_set_style_text_font(yesLabel, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(yesLabel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_color(yesLabel, blue, LV_STATE_PRESSED);
    lv_obj_center(yesLabel);
    
    lv_obj_t *noBtn = lv_btn_create(confirmBox);
    lv_obj_set_size(noBtn, 170, 45);
    lv_obj_align(noBtn, LV_ALIGN_BOTTOM_RIGHT, -40, -25);  // 40px from right edge (gives 30px gap)
    lv_obj_set_style_radius(noBtn, 0, 0);
    lv_obj_set_style_bg_color(noBtn, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(noBtn, 2, 0);
    lv_obj_set_style_border_color(noBtn, kaspa_green, 0);
    // Button press effect - scale down
    lv_obj_set_style_bg_color(noBtn, kaspa_green, LV_STATE_PRESSED);
    lv_obj_add_event_cb(noBtn, [](lv_event_t *e) {
      if (e->code == LV_EVENT_CLICKED) {
        playBeep(800, 50);
        lv_obj_t *overlay = lv_obj_get_parent(lv_obj_get_parent(lv_event_get_target(e)));
        lv_obj_del(overlay);
      }
    }, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *noLabel = lv_label_create(noBtn);
    lv_label_set_text(noLabel, "CANCEL");
    lv_obj_set_style_text_font(noLabel, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(noLabel, kaspa_green, 0);
    lv_obj_center(noLabel);
  }
}

// Factory Reset callback - shows confirmation dialog
void factory_reset_confirm_cb(lv_event_t *e) {
  if (e->code == LV_EVENT_CLICKED) {
    playBeep(600, 100);  // Warning beep
    
    lv_color_t kaspa_green = lv_color_hex(0x49EACB);
    lv_color_t red = lv_color_hex(0xFF0000);
    
    // Create confirmation overlay
    lv_obj_t *confirmOverlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(confirmOverlay, 800, 480);
    lv_obj_set_pos(confirmOverlay, 0, 0);
    lv_obj_set_style_bg_color(confirmOverlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(confirmOverlay, LV_OPA_90, 0);
    lv_obj_set_style_border_width(confirmOverlay, 0, 0);
    lv_obj_clear_flag(confirmOverlay, LV_OBJ_FLAG_SCROLLABLE);
    
    // Confirmation box
    lv_obj_t *confirmBox = lv_obj_create(confirmOverlay);
    lv_obj_set_size(confirmBox, 500, 300);
    lv_obj_align(confirmBox, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(confirmBox, 0, 0);
    lv_obj_set_style_bg_color(confirmBox, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(confirmBox, 3, 0);
    lv_obj_set_style_border_color(confirmBox, red, 0);
    lv_obj_clear_flag(confirmBox, LV_OBJ_FLAG_SCROLLABLE);
    
    // Warning icon - centered
    lv_obj_t *warnIcon = lv_label_create(confirmBox);
    lv_label_set_text(warnIcon, "!");
    lv_obj_set_style_text_font(warnIcon, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(warnIcon, red, 0);
    lv_obj_align(warnIcon, LV_ALIGN_TOP_MID, 0, 20);
    
    // Warning title - centered
    lv_obj_t *titleLabel = lv_label_create(confirmBox);
    lv_label_set_text(titleLabel, "Factory Reset?");
    lv_obj_set_style_text_font(titleLabel, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(titleLabel, red, 0);
    lv_obj_align(titleLabel, LV_ALIGN_TOP_MID, 0, 80);
    
    // Warning message - centered
    lv_obj_t *msgLabel = lv_label_create(confirmBox);
    lv_label_set_text(msgLabel, "This will erase ALL settings:\nWiFi credentials, Wallet address,\nPool configuration");
    lv_obj_set_style_text_font(msgLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(msgLabel, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_align(msgLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(msgLabel, LV_ALIGN_TOP_MID, 0, 120);
    lv_obj_set_width(msgLabel, 420);
    lv_label_set_long_mode(msgLabel, LV_LABEL_LONG_WRAP);
    
    // YES button (RED) - properly spaced
    lv_obj_t *yesBtn = lv_btn_create(confirmBox);
    lv_obj_set_size(yesBtn, 170, 45);
    lv_obj_align(yesBtn, LV_ALIGN_BOTTOM_LEFT, 60, -25);  // 60px from left edge for 500px dialog
    lv_obj_set_style_radius(yesBtn, 0, 0);
    lv_obj_set_style_bg_color(yesBtn, red, 0);
    lv_obj_set_style_border_width(yesBtn, 0, 0);
    // Button press effect - scale down
    lv_obj_set_style_bg_color(yesBtn, lv_color_hex(0x000000), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(yesBtn, 2, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(yesBtn, red, LV_STATE_PRESSED);
    lv_obj_add_event_cb(yesBtn, [](lv_event_t *e) {
      if (e->code == LV_EVENT_CLICKED) {
        playBeep(400, 300);  // Confirmation beep
        
        // Perform factory reset
        Serial.println("!!! FACTORY RESET INITIATED !!!");

        // Clear application preferences (wallet, pool, timezone, password, etc.)
        Preferences prefs;
        prefs.begin("kaspa", false);
        prefs.clear();  // Clears all including devicePassword - will revert to default "kaspa123"
        prefs.end();

        // Clear WiFi credentials (stored separately by WiFiManager)
        Serial.println("Clearing WiFi credentials...");
        wm.resetSettings();

        Serial.println("All settings cleared. Web UI password reset to default (kaspa123)");

        // Show resetting message
        showErrorNotification("Factory Reset Complete!\nSystem will restart...");
        delay(2000);

        ESP.restart();
      }
    }, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *yesLabel = lv_label_create(yesBtn);
    lv_label_set_text(yesLabel, "YES, RESET");
    lv_obj_set_style_text_font(yesLabel, &lv_font_montserrat_14, 0);  // Slightly smaller font
    lv_obj_set_style_text_color(yesLabel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_color(yesLabel, red, LV_STATE_PRESSED);
    lv_obj_center(yesLabel);
    
    // NO button (GREEN) - properly spaced
    lv_obj_t *noBtn = lv_btn_create(confirmBox);
    lv_obj_set_size(noBtn, 170, 45);
    lv_obj_align(noBtn, LV_ALIGN_BOTTOM_RIGHT, -60, -25);  // 60px from right edge (gives 40px gap)
    lv_obj_set_style_radius(noBtn, 0, 0);
    lv_obj_set_style_bg_color(noBtn, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(noBtn, 2, 0);
    lv_obj_set_style_border_color(noBtn, kaspa_green, 0);
    // Button press effect - scale down
    lv_obj_set_style_bg_color(noBtn, kaspa_green, LV_STATE_PRESSED);
    lv_obj_add_event_cb(noBtn, [](lv_event_t *e) {
      if (e->code == LV_EVENT_CLICKED) {
        playBeep(800, 50);  // Cancel beep
        // Close confirmation dialog
        lv_obj_t *overlay = lv_obj_get_parent(lv_obj_get_parent(lv_event_get_target(e)));
        lv_obj_del(overlay);
      }
    }, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *noLabel = lv_label_create(noBtn);
    lv_label_set_text(noLabel, "CANCEL");
    lv_obj_set_style_text_font(noLabel, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(noLabel, kaspa_green, 0);
    lv_obj_center(noLabel);
  }
}

void show_system_config_menu() {
  lv_color_t kaspa_green = lv_color_hex(0x49EACB);
  
  lv_obj_t *systemMenu = lv_obj_create(lv_scr_act());
  lv_obj_set_size(systemMenu, 800, 480);
  lv_obj_set_pos(systemMenu, 0, 0);
  lv_obj_set_style_bg_color(systemMenu, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(systemMenu, LV_OPA_90, 0);
  lv_obj_set_style_border_width(systemMenu, 0, 0);
  lv_obj_clear_flag(systemMenu, LV_OBJ_FLAG_SCROLLABLE);
  
  lv_obj_t *panel = lv_obj_create(systemMenu);
  lv_obj_set_size(panel, 550, 460);  // Increased size for network info
  lv_obj_align(panel, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_radius(panel, 12, 0);  // Rounded corners
  lv_obj_set_style_bg_color(panel, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(panel, LV_OPA_100, 0);
  lv_obj_set_style_border_width(panel, 2, 0);
  lv_obj_set_style_border_color(panel, kaspa_green, 0);
  lv_obj_set_style_shadow_width(panel, 0, 0);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = lv_label_create(panel);
  lv_label_set_text(title, "SYSTEM CONFIG");  // Removed > character
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(title, kaspa_green, 0);
  lv_obj_set_pos(title, 20, 15);
  
  // Timezone Configuration Section (moved up, network info removed)
  lv_obj_t *timezoneLabel = lv_label_create(panel);
  lv_label_set_text(timezoneLabel, "Timezone:");
  lv_obj_set_style_text_font(timezoneLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(timezoneLabel, kaspa_green, 0);
  lv_obj_set_pos(timezoneLabel, 40, 60);  // Moved way up from 155
  
  // Timezone dropdown
  lv_obj_t *timezoneDropdown = lv_dropdown_create(panel);
  lv_dropdown_set_options(timezoneDropdown, 
    "UTC-12\n"
    "UTC-11 (Hawaii)\n"
    "UTC-10 (Alaska)\n"
    "UTC-9\n"
    "UTC-8 (Pacific)\n"
    "UTC-7 (Mountain)\n"
    "UTC-6 (Central)\n"
    "UTC-5 (Eastern)\n"
    "UTC-4 (Atlantic)\n"
    "UTC-3\n"
    "UTC-2\n"
    "UTC-1\n"
    "UTC+0 (GMT)\n"
    "UTC+1 (CET)\n"
    "UTC+2\n"
    "UTC+3\n"
    "UTC+4\n"
    "UTC+5\n"
    "UTC+6\n"
    "UTC+7\n"
    "UTC+8 (China)\n"
    "UTC+9 (Japan)\n"
    "UTC+10 (Australia)\n"
    "UTC+11\n"
    "UTC+12"
  );
  
  // Set current timezone selection (convert from hours to dropdown index)
  int dropdownIndex = timezoneOffsetHours + 12;  // -12 becomes 0, 0 becomes 12, +12 becomes 24
  lv_dropdown_set_selected(timezoneDropdown, dropdownIndex);
  
  lv_obj_set_size(timezoneDropdown, 200, 35);
  lv_obj_set_pos(timezoneDropdown, 150, 55);  // Moved up from 150
  lv_obj_set_style_bg_color(timezoneDropdown, lv_color_hex(0x1A1A1A), 0);
  lv_obj_set_style_border_color(timezoneDropdown, kaspa_green, 0);
  lv_obj_set_style_text_color(timezoneDropdown, lv_color_hex(0xFFFFFF), 0);
  
  // Add callback to save timezone when changed
  lv_obj_add_event_cb(timezoneDropdown, [](lv_event_t *e) {
    if (e->code == LV_EVENT_VALUE_CHANGED) {
      lv_obj_t *dropdown = lv_event_get_target(e);
      uint16_t selected = lv_dropdown_get_selected(dropdown);
      
      // Convert dropdown index back to hours offset
      timezoneOffsetHours = (int)selected - 12;
      
      Serial.printf("Timezone changed to UTC%+d\n", timezoneOffsetHours);
      
      // Save to preferences
      preferences.begin("kaspa", false);
      preferences.putInt("timezone", timezoneOffsetHours);
      preferences.end();
      
      // Reconfigure NTP with new timezone
      int timezoneOffsetSeconds = timezoneOffsetHours * 3600;
      configTime(timezoneOffsetSeconds, 3600, ntpServers[0]);
      
      playBeep(800, 50);
    }
  }, LV_EVENT_VALUE_CHANGED, NULL);

  // Reset WiFi button (moved up)
  lv_obj_t *wifiResetBtn = lv_btn_create(panel);
  lv_obj_set_size(wifiResetBtn, 300, 50);
  lv_obj_align(wifiResetBtn, LV_ALIGN_TOP_MID, 0, 110);  // Moved up from 200
  lv_obj_set_style_radius(wifiResetBtn, 8, 0);
  lv_obj_set_style_bg_color(wifiResetBtn, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(wifiResetBtn, 2, 0);
  lv_obj_set_style_border_color(wifiResetBtn, kaspa_green, 0);
  lv_obj_set_style_shadow_width(wifiResetBtn, 0, 0);
  lv_obj_set_style_shadow_ofs_y(wifiResetBtn, 3, 0);
  // Button press effect
  lv_obj_set_style_bg_color(wifiResetBtn, kaspa_green, LV_STATE_PRESSED);
  lv_obj_add_event_cb(wifiResetBtn, wifi_reset_confirm_cb, LV_EVENT_CLICKED, NULL);
  
  lv_obj_t *wifiResetLabel = lv_label_create(wifiResetBtn);
  lv_label_set_text(wifiResetLabel, "Reset WiFi");
  lv_obj_set_style_text_font(wifiResetLabel, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(wifiResetLabel, lv_color_hex(0xFFFFFF), 0);  // White
  lv_obj_set_style_text_color(wifiResetLabel, lv_color_hex(0x000000), LV_STATE_PRESSED);
  lv_obj_center(wifiResetLabel);

  // Restart System button (moved up)
  lv_obj_t *restartBtn = lv_btn_create(panel);
  lv_obj_set_size(restartBtn, 300, 50);
  lv_obj_align(restartBtn, LV_ALIGN_TOP_MID, 0, 175);  // Moved up from 265
  lv_obj_set_style_radius(restartBtn, 8, 0);
  lv_obj_set_style_bg_color(restartBtn, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(restartBtn, 2, 0);
  lv_obj_set_style_border_color(restartBtn, kaspa_green, 0);
  lv_obj_set_style_shadow_width(restartBtn, 0, 0);
  lv_obj_set_style_shadow_ofs_y(restartBtn, 3, 0);
  // Button press effect
  lv_obj_set_style_bg_color(restartBtn, kaspa_green, LV_STATE_PRESSED);
  lv_obj_add_event_cb(restartBtn, restart_confirm_cb, LV_EVENT_CLICKED, NULL);
  
  lv_obj_t *restartLabel = lv_label_create(restartBtn);
  lv_label_set_text(restartLabel, "Restart System");
  lv_obj_set_style_text_font(restartLabel, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(restartLabel, lv_color_hex(0xFFFFFF), 0);  // White
  lv_obj_set_style_text_color(restartLabel, lv_color_hex(0x000000), LV_STATE_PRESSED);
  lv_obj_center(restartLabel);

  // Factory Reset button (RED) with confirmation (moved up)
  lv_obj_t *factoryResetBtn = lv_btn_create(panel);
  lv_obj_set_size(factoryResetBtn, 300, 50);
  lv_obj_align(factoryResetBtn, LV_ALIGN_TOP_MID, 0, 240);  // Moved up from 330
  lv_obj_set_style_radius(factoryResetBtn, 8, 0);
  lv_obj_set_style_bg_color(factoryResetBtn, lv_color_hex(0xFF0000), 0);  // RED
  lv_obj_set_style_border_width(factoryResetBtn, 0, 0);
  lv_obj_set_style_shadow_width(factoryResetBtn, 0, 0);
  // Button press effect - scale down
  lv_obj_set_style_bg_color(factoryResetBtn, lv_color_hex(0x000000), LV_STATE_PRESSED);
  lv_obj_set_style_border_width(factoryResetBtn, 2, LV_STATE_PRESSED);
  lv_obj_set_style_border_color(factoryResetBtn, lv_color_hex(0xFF0000), LV_STATE_PRESSED);
  lv_obj_add_event_cb(factoryResetBtn, factory_reset_confirm_cb, LV_EVENT_CLICKED, NULL);
  
  lv_obj_t *factoryResetLabel = lv_label_create(factoryResetBtn);
  lv_label_set_text(factoryResetLabel, "Factory Reset");
  lv_obj_set_style_text_font(factoryResetLabel, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(factoryResetLabel, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_color(factoryResetLabel, lv_color_hex(0xFF0000), LV_STATE_PRESSED);
  lv_obj_center(factoryResetLabel);

  // Back button (moved up)
  lv_obj_t *backBtn = lv_btn_create(panel);
  lv_obj_set_size(backBtn, 300, 50);
  lv_obj_align(backBtn, LV_ALIGN_TOP_MID, 0, 305);  // Moved up from 395
  lv_obj_set_style_radius(backBtn, 8, 0);
  lv_obj_set_style_bg_color(backBtn, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(backBtn, 2, 0);
  lv_obj_set_style_border_color(backBtn, kaspa_green, 0);
  lv_obj_set_style_shadow_width(backBtn, 0, 0);
  // Button press effect
  lv_obj_set_style_bg_color(backBtn, kaspa_green, LV_STATE_PRESSED);
  lv_obj_add_event_cb(backBtn, [](lv_event_t *e) {
    if (e->code == LV_EVENT_CLICKED) {
      lv_obj_t *parent = lv_obj_get_parent(lv_event_get_target(e));
      while (parent && parent != lv_scr_act()) {
        lv_obj_t *next = lv_obj_get_parent(parent);
        if (next == lv_scr_act()) {
          lv_obj_del(parent);
          show_settings_menu();
          break;
        }
        parent = next;
      }
    }
  }, LV_EVENT_CLICKED, NULL);
  
  lv_obj_t *backLabel = lv_label_create(backBtn);
  lv_label_set_text(backLabel, "Back");
  lv_obj_set_style_text_font(backLabel, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(backLabel, lv_color_hex(0xFFFFFF), 0);  // White
  lv_obj_set_style_text_color(backLabel, lv_color_hex(0x000000), LV_STATE_PRESSED);
  lv_obj_center(backLabel);
  lv_label_set_text(backLabel, "Back");
  lv_obj_set_style_text_font(backLabel, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(backLabel, kaspa_green, 0);
  lv_obj_center(backLabel);
}

void wifi_reset_event_cb(lv_event_t * e) {
  if (e->code == LV_EVENT_CLICKED) {
    preferences.begin("kaspa", false);
    preferences.putBool("wifiReset", true);
    preferences.end();
    wm.resetSettings();
    delay(500);
    ESP.restart();
  }
}

void restart_event_cb(lv_event_t * e) {
  if (e->code == LV_EVENT_CLICKED) {
    ESP.restart();
  }
}

// ========== FIRST BOOT WIZARD ==========

void show_wizard_welcome() {
  lv_color_t kaspa_green = lv_color_hex(0x49EACB);
  
  wizardScreen = lv_obj_create(lv_scr_act());
  lv_obj_set_size(wizardScreen, 800, 480);
  lv_obj_set_pos(wizardScreen, 0, 0);
  lv_obj_set_style_bg_color(wizardScreen, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_grad_color(wizardScreen, lv_color_hex(0x0a0a0a), 0);
  lv_obj_set_style_bg_grad_dir(wizardScreen, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_border_width(wizardScreen, 0, 0);
  lv_obj_clear_flag(wizardScreen, LV_OBJ_FLAG_SCROLLABLE);
  
  // Proof of Prints logo at top center - SMALLER and HIGHER
  lv_obj_t *popLogo = lv_img_create(wizardScreen);
  if (logos_loaded) {
    lv_img_set_src(popLogo, &pop_logo_sd);
  }
  lv_img_set_zoom(popLogo, 180);  // Scale to 70% of 200x200 = ~140x140
  lv_obj_align(popLogo, LV_ALIGN_TOP_MID, 0, -15);  // MOVED UP MORE
  
  // Welcome text - CENTER ALIGNED, positioned below PoP logo
  lv_obj_t *welcomeText = lv_label_create(wizardScreen);
  lv_label_set_text(welcomeText, 
    "Welcome to KASDeck, the Kaspa companion that can\n"
    "provide lottery style mining for the Kaspa network.\n\n"
    "For support email us at support@proofofprints.com\n\n"
    "Tap Get Started to begin.");
  lv_obj_set_style_text_font(welcomeText, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(welcomeText, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_align(welcomeText, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(welcomeText, 700);
  lv_label_set_long_mode(welcomeText, LV_LABEL_LONG_WRAP);
  lv_obj_align(welcomeText, LV_ALIGN_CENTER, 0, -20);  // Centered, slightly above middle
  
  // KASDeck logo below text - CENTERED and SMALLER
  lv_obj_t *kasDeckLogo = lv_img_create(wizardScreen);
  if (logos_loaded) {
    lv_img_set_src(kasDeckLogo, &kaspa_logo_sd);
  }
  lv_img_set_zoom(kasDeckLogo, 154);  // Scale to 60% (400x126 → 240x76)
  lv_obj_align(kasDeckLogo, LV_ALIGN_BOTTOM_MID, 0, -80);  // Centered, above button
  
  // Get Started button (bottom right - "Wizard Button")
  lv_obj_t *wizardBtn = lv_btn_create(wizardScreen);
  lv_obj_set_size(wizardBtn, 200, 50);
  lv_obj_align(wizardBtn, LV_ALIGN_BOTTOM_RIGHT, -20, -20);
  lv_obj_set_style_radius(wizardBtn, 0, 0);
  lv_obj_set_style_bg_color(wizardBtn, kaspa_green, 0);
  lv_obj_set_style_border_width(wizardBtn, 0, 0);
  lv_obj_add_event_cb(wizardBtn, [](lv_event_t *e) {
    if (e->code == LV_EVENT_CLICKED) {
      playBeep(800, 50);
      lv_obj_del(wizardScreen);
      wizardScreen = nullptr;
      wizardStep = 1;
      show_wizard_wireless_intro();
    }
  }, LV_EVENT_CLICKED, NULL);
  
  lv_obj_t *btnLabel = lv_label_create(wizardBtn);
  lv_label_set_text(btnLabel, "Get Started");
  lv_obj_set_style_text_font(btnLabel, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(btnLabel, lv_color_hex(0x000000), 0);
  lv_obj_center(btnLabel);
}

void show_wizard_wireless_intro() {
  lv_color_t kaspa_green = lv_color_hex(0x49EACB);
  
  wizardScreen = lv_obj_create(lv_scr_act());
  lv_obj_set_size(wizardScreen, 800, 480);
  lv_obj_set_pos(wizardScreen, 0, 0);
  lv_obj_set_style_bg_color(wizardScreen, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_grad_color(wizardScreen, lv_color_hex(0x0a0a0a), 0);
  lv_obj_set_style_bg_grad_dir(wizardScreen, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_border_width(wizardScreen, 0, 0);
  lv_obj_clear_flag(wizardScreen, LV_OBJ_FLAG_SCROLLABLE);
  
  // PoP logo at top left (stays here for rest of wizard) - SCALED DOWN
  lv_obj_t *popLogo = lv_img_create(wizardScreen);
  if (logos_loaded) {
    lv_img_set_src(popLogo, &pop_logo_sd);
  }
  lv_img_set_zoom(popLogo, 128);  // Scale to 50% (256 = 100%), ~100x100px
  lv_obj_set_pos(popLogo, -10, -20);  // Left 20px and UP 30px
  
  // Title
  lv_obj_t *titleLabel = lv_label_create(wizardScreen);
  lv_label_set_text(titleLabel, "Setup Wireless");
  lv_obj_set_style_text_font(titleLabel, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(titleLabel, kaspa_green, 0);
  lv_obj_align(titleLabel, LV_ALIGN_TOP_MID, 0, 40);
  
  // Instructions
  lv_obj_t *instructText = lv_label_create(wizardScreen);
  lv_label_set_text(instructText,
    "First we will need to be able to connect to the internet to bring\n"
    "down data from the Kaspa network.\n\n"
    "The KASDeck can connect to your wireless 2.4GHz and 5GHz\n"
    "networks.\n\n"
    "Click Setup Wireless to connect.");
  lv_obj_set_style_text_font(instructText, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(instructText, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_align(instructText, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(instructText, 700);
  lv_label_set_long_mode(instructText, LV_LABEL_LONG_WRAP);
  lv_obj_align(instructText, LV_ALIGN_CENTER, 0, -20);
  
  // Setup Wireless button (Wizard Button - bottom right)
  lv_obj_t *wizardBtn = lv_btn_create(wizardScreen);
  lv_obj_set_size(wizardBtn, 220, 50);
  lv_obj_align(wizardBtn, LV_ALIGN_BOTTOM_RIGHT, -20, -20);
  lv_obj_set_style_radius(wizardBtn, 0, 0);
  lv_obj_set_style_bg_color(wizardBtn, kaspa_green, 0);
  lv_obj_set_style_border_width(wizardBtn, 0, 0);
  lv_obj_add_event_cb(wizardBtn, [](lv_event_t *e) {
    if (e->code == LV_EVENT_CLICKED) {
      playBeep(800, 50);
      lv_obj_del(wizardScreen);
      wizardScreen = nullptr;
      wizardStep = 2;
      show_wifi_config_wizard();  // Modified WiFi config for wizard
    }
  }, LV_EVENT_CLICKED, NULL);
  
  lv_obj_t *btnLabel = lv_label_create(wizardBtn);
  lv_label_set_text(btnLabel, "Setup Wireless");
  lv_obj_set_style_text_font(btnLabel, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(btnLabel, lv_color_hex(0x000000), 0);
  lv_obj_center(btnLabel);
}

void show_wizard_mining_setup() {
  lv_color_t kaspa_green = lv_color_hex(0x49EACB);
  
  wizardScreen = lv_obj_create(lv_scr_act());
  lv_obj_set_size(wizardScreen, 800, 480);
  lv_obj_set_pos(wizardScreen, 0, 0);
  lv_obj_set_style_bg_color(wizardScreen, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_grad_color(wizardScreen, lv_color_hex(0x0a0a0a), 0);
  lv_obj_set_style_bg_grad_dir(wizardScreen, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_border_width(wizardScreen, 0, 0);
  lv_obj_clear_flag(wizardScreen, LV_OBJ_FLAG_SCROLLABLE);
  
  // PoP logo at top left - SCALED DOWN
  lv_obj_t *popLogo = lv_img_create(wizardScreen);
  if (logos_loaded) {
    lv_img_set_src(popLogo, &pop_logo_sd);
  }
  lv_img_set_zoom(popLogo, 128);  // Scale to 50% (256 = 100%), ~100x100px
  lv_obj_set_pos(popLogo, -30, -40);  // More left and up for longer text
  
  // Title
  lv_obj_t *titleLabel = lv_label_create(wizardScreen);
  lv_label_set_text(titleLabel, "Mining Setup");
  lv_obj_set_style_text_font(titleLabel, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(titleLabel, kaspa_green, 0);
  lv_obj_align(titleLabel, LV_ALIGN_TOP_MID, 0, 40);
  
  // Instructions
  lv_obj_t *instructText = lv_label_create(wizardScreen);
  lv_label_set_text(instructText,
    "The KASDeck has the capability to provide lottery style mining on the Kaspa\n"
    "network. The hashrate of this miner is only around 1.5 KH/s so don't expect to\n"
    "smash blocks left and right. With 10bps there is very little time to submit hashes.\n\n"
    "The best approach is to setup your own node and set the minimum difficulty to 0.001.\n"
    "You can try connecting to other pool providers, but they may not be setup for\n"
    "such low difficulty.\n\n"
    "If you would like to try lottery mining, go to one of the URLs listed below in your\n"
    "browser from a device on the same network:");
  lv_obj_set_style_text_font(instructText, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(instructText, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_align(instructText, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(instructText, 750);
  lv_label_set_long_mode(instructText, LV_LABEL_LONG_WRAP);
  lv_obj_align(instructText, LV_ALIGN_TOP_MID, 0, 100);

  // mDNS URL (.local address)
  String ipAddress = WiFi.localIP().toString();
  String mdnsName = "kaspa-" + WiFi.macAddress().substring(12);
  mdnsName.replace(":", "");
  mdnsName.toLowerCase();

  lv_obj_t *mdnsLabel = lv_label_create(wizardScreen);
  String mdnsUrl = "http://" + mdnsName + ".local";
  lv_label_set_text(mdnsLabel, mdnsUrl.c_str());
  lv_obj_set_style_text_color(mdnsLabel, kaspa_green, 0);
  lv_obj_set_style_text_font(mdnsLabel, &lv_font_montserrat_18, 0);
  lv_obj_align(mdnsLabel, LV_ALIGN_BOTTOM_MID, 0, -115);

  // IP Address URL
  lv_obj_t *urlLabel = lv_label_create(wizardScreen);
  String urlText = "http://" + ipAddress;
  lv_label_set_text(urlLabel, urlText.c_str());
  lv_obj_set_style_text_color(urlLabel, kaspa_green, 0);
  lv_obj_set_style_text_font(urlLabel, &lv_font_montserrat_18, 0);
  lv_obj_align(urlLabel, LV_ALIGN_BOTTOM_MID, 0, -90);

  // Password hint
  lv_obj_t *pwdHint = lv_label_create(wizardScreen);
  lv_label_set_text(pwdHint, "Web UI Password: kaspa123");
  lv_obj_set_style_text_color(pwdHint, lv_color_hex(0x888888), 0);
  lv_obj_set_style_text_font(pwdHint, &lv_font_montserrat_14, 0);
  lv_obj_align(pwdHint, LV_ALIGN_BOTTOM_MID, 0, -65);

  // Complete Setup button (Wizard Button - bottom right)
  lv_obj_t *wizardBtn = lv_btn_create(wizardScreen);
  lv_obj_set_size(wizardBtn, 220, 50);
  lv_obj_align(wizardBtn, LV_ALIGN_BOTTOM_RIGHT, -20, -20);
  lv_obj_set_style_radius(wizardBtn, 0, 0);
  lv_obj_set_style_bg_color(wizardBtn, kaspa_green, 0);
  lv_obj_set_style_border_width(wizardBtn, 0, 0);
  lv_obj_add_event_cb(wizardBtn, [](lv_event_t *e) {
    if (e->code == LV_EVENT_CLICKED) {
      playBeep(1000, 100);
      
      // Mark wizard as complete
      preferences.begin("kaspa", false);
      preferences.putBool("wizardDone", true);
      preferences.end();
      
      firstBoot = false;
      
      // Clean up wizard screen - main dashboard is already underneath
      lv_obj_del(wizardScreen);
      wizardScreen = nullptr;
      
      // Main UI was already created in setup(), just reveal it
    }
  }, LV_EVENT_CLICKED, NULL);
  
  lv_obj_t *btnLabel = lv_label_create(wizardBtn);
  lv_label_set_text(btnLabel, "Complete Setup");
  lv_obj_set_style_text_font(btnLabel, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(btnLabel, lv_color_hex(0x000000), 0);
  lv_obj_center(btnLabel);
}

// Wizard version of WiFi config - proceeds to mining setup after connection
void show_wifi_config_wizard() {
  // Use the existing WiFi config, but set a flag to know we're in wizard mode
  // The connect callback will check wizardStep and proceed accordingly
  show_wifi_config_menu();
}

void show_about_screen() {
  lv_color_t kaspa_green = lv_color_hex(0x49EACB);
  
  // Create overlay
  lv_obj_t *aboutOverlay = lv_obj_create(lv_scr_act());
  lv_obj_set_size(aboutOverlay, 800, 480);
  lv_obj_set_pos(aboutOverlay, 0, 0);
  lv_obj_set_style_bg_color(aboutOverlay, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(aboutOverlay, LV_OPA_90, 0);
  lv_obj_set_style_border_width(aboutOverlay, 0, 0);
  lv_obj_clear_flag(aboutOverlay, LV_OBJ_FLAG_SCROLLABLE);
  
  // About panel - TALLER to fit everything
  lv_obj_t *aboutPanel = lv_obj_create(aboutOverlay);
  lv_obj_set_size(aboutPanel, 580, 450);  // Wider and taller
  lv_obj_align(aboutPanel, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_radius(aboutPanel, 0, 0);
  lv_obj_set_style_bg_color(aboutPanel, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(aboutPanel, 2, 0);
  lv_obj_set_style_border_color(aboutPanel, kaspa_green, 0);
  lv_obj_set_style_shadow_width(aboutPanel, 0, 0);
  lv_obj_clear_flag(aboutPanel, LV_OBJ_FLAG_SCROLLABLE);
  
  // Title - Top
  lv_obj_t *titleLabel = lv_label_create(aboutPanel);
  lv_label_set_text(titleLabel, "KASDECK");
  lv_obj_set_style_text_font(titleLabel, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(titleLabel, kaspa_green, 0);
  lv_obj_set_pos(titleLabel, 0, 25);  // Fixed position from top
  lv_obj_align(titleLabel, LV_ALIGN_TOP_MID, 0, 25);
  
  // Firmware version - More space
  lv_obj_t *firmwareLabel = lv_label_create(aboutPanel);
  String firmwareText = "Firmware: " + String(FIRMWARE_VERSION);
  lv_label_set_text(firmwareLabel, firmwareText.c_str());
  lv_obj_set_style_text_font(firmwareLabel, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(firmwareLabel, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(firmwareLabel, LV_ALIGN_TOP_MID, 0, 70);  // +10px more space
  
  // Serial number - More space
  uint64_t chipid = ESP.getEfuseMac();
  char serialStr[32];
  sprintf(serialStr, "Serial: %04X%08X", (uint16_t)(chipid >> 32), (uint32_t)chipid);
  
  lv_obj_t *serialLabel = lv_label_create(aboutPanel);
  lv_label_set_text(serialLabel, serialStr);
  lv_obj_set_style_text_font(serialLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(serialLabel, lv_color_hex(0xCCCCCC), 0);
  lv_obj_align(serialLabel, LV_ALIGN_TOP_MID, 0, 105);  // +15px more space
  
  // Credit text - LESS space from serial, remove extra blank line
  lv_obj_t *creditLabel = lv_label_create(aboutPanel);
  lv_label_set_text(creditLabel, "Built for the Kaspa Community\nby Proof of Prints");  // No extra \n
  lv_obj_set_style_text_font(creditLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(creditLabel, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_align(creditLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(creditLabel, LV_ALIGN_TOP_MID, 0, 140);  // Closer to serial (was 135, now 140)
  
  // Proof of Prints logo - Move down 5px more for spacing from text
  lv_obj_t *popLogo = lv_img_create(aboutPanel);
  if (logos_loaded) {
    lv_img_set_src(popLogo, &pop_logo_sd);
  }
  lv_obj_align(popLogo, LV_ALIGN_CENTER, 0, 45);  // Was +40, now +45 (5px more gap from text)
  
  // Close button - More space from logo, at bottom
  lv_obj_t *closeBtn = lv_btn_create(aboutPanel);
  lv_obj_set_size(closeBtn, 200, 45);
  lv_obj_align(closeBtn, LV_ALIGN_BOTTOM_MID, 0, -20);  // 20px from bottom
  lv_obj_set_style_radius(closeBtn, 8, 0);
  lv_obj_set_style_bg_color(closeBtn, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(closeBtn, 2, 0);
  lv_obj_set_style_border_color(closeBtn, kaspa_green, 0);
  lv_obj_set_style_shadow_width(closeBtn, 0, 0);
  lv_obj_set_style_bg_color(closeBtn, kaspa_green, LV_STATE_PRESSED);
  lv_obj_add_event_cb(closeBtn, [](lv_event_t *e) {
    if (e->code == LV_EVENT_CLICKED) {
      playBeep(800, 50);
      lv_obj_t *overlay = lv_obj_get_parent(lv_obj_get_parent(lv_event_get_target(e)));
      lv_obj_del(overlay);
    }
  }, LV_EVENT_CLICKED, NULL);
  
  lv_obj_t *closeLabel = lv_label_create(closeBtn);
  lv_label_set_text(closeLabel, "Close");
  lv_obj_set_style_text_font(closeLabel, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(closeLabel, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_color(closeLabel, lv_color_hex(0x000000), LV_STATE_PRESSED);
  lv_obj_center(closeLabel);
}

void show_settings_menu() {
  if (settingsMenu) return;
  
  lv_color_t kaspa_green = lv_color_hex(0x49EACB);
  
  settingsMenu = lv_obj_create(lv_scr_act());
  lv_obj_set_size(settingsMenu, 800, 480);
  lv_obj_set_pos(settingsMenu, 0, 0);
  lv_obj_set_style_bg_color(settingsMenu, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(settingsMenu, LV_OPA_90, 0);
  lv_obj_set_style_border_width(settingsMenu, 0, 0);
  lv_obj_clear_flag(settingsMenu, LV_OBJ_FLAG_SCROLLABLE);
  
  lv_obj_t *panel = lv_obj_create(settingsMenu);
  lv_obj_set_size(panel, 500, 400);
  lv_obj_align(panel, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_radius(panel, 0, 0);  // Sharp corners
  lv_obj_set_style_bg_color(panel, lv_color_hex(0x000000), 0);  // Black bg
  lv_obj_set_style_bg_opa(panel, LV_OPA_100, 0);
  lv_obj_set_style_border_width(panel, 2, 0);
  lv_obj_set_style_border_color(panel, kaspa_green, 0);
  lv_obj_set_style_shadow_width(panel, 0, 0);  // No shadow
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
  
  lv_obj_t *title = lv_label_create(panel);
  lv_label_set_text(title, "SYSTEM MENU");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(title, kaspa_green, 0);
  lv_obj_set_pos(title, 20, 15);

  // Info button (top right corner)
  lv_obj_t *infoBtn = lv_btn_create(panel);
  lv_obj_set_size(infoBtn, 40, 40);  // Small circular-ish button
  lv_obj_align(infoBtn, LV_ALIGN_TOP_RIGHT, -15, 10);
  lv_obj_set_style_radius(infoBtn, 8, 0);
  lv_obj_set_style_bg_color(infoBtn, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(infoBtn, 2, 0);
  lv_obj_set_style_border_color(infoBtn, kaspa_green, 0);
  lv_obj_set_style_shadow_width(infoBtn, 0, 0);
  lv_obj_set_style_shadow_ofs_y(infoBtn, 3, 0);
  lv_obj_set_style_bg_color(infoBtn, kaspa_green, LV_STATE_PRESSED);
  lv_obj_add_event_cb(infoBtn, [](lv_event_t *e) {
    if (e->code == LV_EVENT_CLICKED) {
      playBeep(800, 50);
      show_about_screen();
    }
  }, LV_EVENT_CLICKED, NULL);
  
  lv_obj_t *infoLabel = lv_label_create(infoBtn);
  lv_label_set_text(infoLabel, "i");  // Info icon
  lv_obj_set_style_text_font(infoLabel, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(infoLabel, kaspa_green, 0);
  lv_obj_set_style_text_color(infoLabel, lv_color_hex(0x000000), LV_STATE_PRESSED);
  lv_obj_center(infoLabel);

  //Configure Miner  
  lv_obj_t *minerConfigBtn = lv_btn_create(panel);
  lv_obj_set_size(minerConfigBtn, 300, 50);  // Smaller, matching System Config
  lv_obj_align(minerConfigBtn, LV_ALIGN_TOP_MID, 0, 70);
  lv_obj_set_style_radius(minerConfigBtn, 8, 0);
  lv_obj_set_style_bg_color(minerConfigBtn, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(minerConfigBtn, 2, 0);
  lv_obj_set_style_border_color(minerConfigBtn, kaspa_green, 0);
  lv_obj_set_style_shadow_width(minerConfigBtn, 0, 0);
  lv_obj_set_style_shadow_ofs_y(minerConfigBtn, 3, 0);
  // Button press effect
  lv_obj_set_style_bg_color(minerConfigBtn, kaspa_green, LV_STATE_PRESSED);
  lv_obj_add_event_cb(minerConfigBtn, miner_config_btn_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *minerConfigLabel = lv_label_create(minerConfigBtn);
  lv_label_set_text(minerConfigLabel, "Lottery Miner Config");
  lv_obj_set_style_text_font(minerConfigLabel, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(minerConfigLabel, lv_color_hex(0xFFFFFF), 0);  // White
  lv_obj_set_style_text_color(minerConfigLabel, lv_color_hex(0x000000), LV_STATE_PRESSED);
  lv_obj_center(minerConfigLabel);

  // Add WiFi Config button
  lv_obj_t *wifiConfigBtn = lv_btn_create(panel);
  lv_obj_set_size(wifiConfigBtn, 300, 50);
  lv_obj_align(wifiConfigBtn, LV_ALIGN_TOP_MID, 0, 135);
  lv_obj_set_style_radius(wifiConfigBtn, 8, 0);
  lv_obj_set_style_bg_color(wifiConfigBtn, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(wifiConfigBtn, 2, 0);
  lv_obj_set_style_border_color(wifiConfigBtn, kaspa_green, 0);
  lv_obj_set_style_shadow_width(wifiConfigBtn, 0, 0);
  lv_obj_set_style_shadow_ofs_y(wifiConfigBtn, 3, 0);
  // Button press effect
  lv_obj_set_style_bg_color(wifiConfigBtn, kaspa_green, LV_STATE_PRESSED);
  lv_obj_add_event_cb(wifiConfigBtn, wifi_config_btn_cb, LV_EVENT_CLICKED, NULL);
  
  lv_obj_t *wifiConfigLabel = lv_label_create(wifiConfigBtn);
  lv_label_set_text(wifiConfigLabel, "Configure WiFi");
  lv_obj_set_style_text_font(wifiConfigLabel, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(wifiConfigLabel, lv_color_hex(0xFFFFFF), 0);  // White
  lv_obj_set_style_text_color(wifiConfigLabel, lv_color_hex(0x000000), LV_STATE_PRESSED);
  lv_obj_center(wifiConfigLabel);

  // Configure System button
  lv_obj_t *systemConfigBtn = lv_btn_create(panel);
  lv_obj_set_size(systemConfigBtn, 300, 50);
  lv_obj_align(systemConfigBtn, LV_ALIGN_TOP_MID, 0, 200);
  lv_obj_set_style_radius(systemConfigBtn, 8, 0);
  lv_obj_set_style_bg_color(systemConfigBtn, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(systemConfigBtn, 2, 0);
  lv_obj_set_style_border_color(systemConfigBtn, kaspa_green, 0);
  lv_obj_set_style_shadow_width(systemConfigBtn, 0, 0);
  lv_obj_set_style_shadow_ofs_y(systemConfigBtn, 3, 0);
  // Button press effect
  lv_obj_set_style_bg_color(systemConfigBtn, kaspa_green, LV_STATE_PRESSED);
  lv_obj_add_event_cb(systemConfigBtn, system_config_btn_cb, LV_EVENT_CLICKED, NULL);
  
  lv_obj_t *systemConfigLabel = lv_label_create(systemConfigBtn);
  lv_label_set_text(systemConfigLabel, "Configure System");
  lv_obj_set_style_text_font(systemConfigLabel, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(systemConfigLabel, lv_color_hex(0xFFFFFF), 0);  // White
  lv_obj_set_style_text_color(systemConfigLabel, lv_color_hex(0x000000), LV_STATE_PRESSED);
  lv_obj_center(systemConfigLabel);
 
  // Close button
  lv_obj_t *closeBtn = lv_btn_create(panel);
  lv_obj_set_size(closeBtn, 300, 50);
  lv_obj_align(closeBtn, LV_ALIGN_TOP_MID, 0, 265);
  lv_obj_set_style_radius(closeBtn, 8, 0);
  lv_obj_set_style_bg_color(closeBtn, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(closeBtn, 2, 0);
  lv_obj_set_style_border_color(closeBtn, kaspa_green, 0);
  lv_obj_set_style_shadow_width(closeBtn, 0, 0);
  // Button press effect
  lv_obj_set_style_bg_color(closeBtn, kaspa_green, LV_STATE_PRESSED);
  lv_obj_add_event_cb(closeBtn, close_settings_event_cb, LV_EVENT_CLICKED, NULL);
  
  lv_obj_t *closeLabel = lv_label_create(closeBtn);
  lv_label_set_text(closeLabel, "Close");
  lv_obj_set_style_text_font(closeLabel, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(closeLabel, lv_color_hex(0xFFFFFF), 0);  // White
  lv_obj_set_style_text_color(closeLabel, lv_color_hex(0x000000), LV_STATE_PRESSED);
  lv_obj_center(closeLabel);
}

void settings_btn_event_cb(lv_event_t * e) {
  Serial.println(">>> Settings button EVENT received");
  if (e->code == LV_EVENT_CLICKED) {
    Serial.println(">>> Settings button CLICKED");
    show_settings_menu();
  }
}

// ========== MENU NAVIGATION CALLBACKS ==========

void menu_btn_event_cb(lv_event_t * e);
void show_main_menu();
void close_main_menu_cb(lv_event_t *e);
void price_info_btn_cb(lv_event_t *e);
void mining_stats_btn_cb(lv_event_t *e);
void show_price_info_screen();
void show_mining_stats_screen();
void back_to_main_cb(lv_event_t *e);
void chart_touch_cb(lv_event_t *e);
void chart_release_cb(lv_event_t *e);

void menu_btn_event_cb(lv_event_t * e) {
  if (e->code == LV_EVENT_CLICKED) {
    Serial.println(">>> MENU button clicked");
    show_main_menu();
  }
}

void show_main_menu() {
  if (mainMenu) {
    lv_obj_del(mainMenu);
    mainMenu = nullptr;
  }

  lv_color_t kaspa_green = lv_color_hex(0x49EACB);
  lv_color_t text_white = lv_color_hex(0xFFFFFF);

  mainMenu = lv_obj_create(lv_scr_act());
  lv_obj_set_size(mainMenu, 800, 480);
  lv_obj_align(mainMenu, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(mainMenu, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(mainMenu, LV_OPA_TRANSP, 0);  // Start transparent
  lv_obj_set_style_border_width(mainMenu, 0, 0);
  lv_obj_clear_flag(mainMenu, LV_OBJ_FLAG_SCROLLABLE);

  // Fade-in animation for overlay
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, mainMenu);
  lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_90);
  lv_anim_set_time(&a, 200);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_style_bg_opa);
  lv_anim_start(&a);

  lv_obj_t *panel = lv_obj_create(mainMenu);
  lv_obj_set_size(panel, 500, 320);
  lv_obj_align(panel, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_radius(panel, 12, 0);  // Larger radius for panels
  lv_obj_set_style_bg_color(panel, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(panel, 2, 0);
  lv_obj_set_style_border_color(panel, kaspa_green, 0);
  lv_obj_set_style_shadow_width(panel, 20, 0);  // Larger shadow for modals
  lv_obj_set_style_shadow_opa(panel, LV_OPA_50, 0);
  lv_obj_set_style_shadow_color(panel, lv_color_hex(0x000000), 0);
  lv_obj_set_style_shadow_ofs_y(panel, 8, 0);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = lv_label_create(panel);
  lv_label_set_text(title, "MAIN MENU");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(title, kaspa_green, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);

  // Price Info button
  lv_obj_t *priceBtn = lv_btn_create(panel);
  lv_obj_set_size(priceBtn, 450, 50);
  lv_obj_align(priceBtn, LV_ALIGN_TOP_MID, 0, 60);
  lv_obj_set_style_radius(priceBtn, 8, 0);
  lv_obj_set_style_bg_color(priceBtn, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(priceBtn, 2, 0);
  lv_obj_set_style_border_color(priceBtn, kaspa_green, 0);
  lv_obj_set_style_shadow_width(priceBtn, 0, 0);  // Disable button shadows to save memory
  lv_obj_set_style_bg_color(priceBtn, kaspa_green, LV_STATE_PRESSED);
  lv_obj_add_event_cb(priceBtn, price_info_btn_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *priceLabel = lv_label_create(priceBtn);
  lv_label_set_text(priceLabel, "Kaspa Price");
  lv_obj_set_style_text_font(priceLabel, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(priceLabel, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_color(priceLabel, lv_color_hex(0x000000), LV_STATE_PRESSED);
  lv_obj_center(priceLabel);

  // Mining Stats button
  lv_obj_t *miningBtn = lv_btn_create(panel);
  lv_obj_set_size(miningBtn, 450, 50);
  lv_obj_align(miningBtn, LV_ALIGN_TOP_MID, 0, 120);
  lv_obj_set_style_radius(miningBtn, 8, 0);
  lv_obj_set_style_bg_color(miningBtn, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(miningBtn, 2, 0);
  lv_obj_set_style_border_color(miningBtn, kaspa_green, 0);
  lv_obj_set_style_shadow_width(miningBtn, 0, 0);
  lv_obj_set_style_bg_color(miningBtn, kaspa_green, LV_STATE_PRESSED);
  lv_obj_add_event_cb(miningBtn, mining_stats_btn_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *miningLabel = lv_label_create(miningBtn);
  lv_label_set_text(miningLabel, "Mining Stats");
  lv_obj_set_style_text_font(miningLabel, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(miningLabel, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_color(miningLabel, lv_color_hex(0x000000), LV_STATE_PRESSED);
  lv_obj_center(miningLabel);

  // Close button
  lv_obj_t *closeBtn = lv_btn_create(panel);
  lv_obj_set_size(closeBtn, 100, 40);
  lv_obj_align(closeBtn, LV_ALIGN_TOP_MID, 0, 250);
  lv_obj_set_style_radius(closeBtn, 8, 0);
  lv_obj_set_style_bg_color(closeBtn, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(closeBtn, 2, 0);
  lv_obj_set_style_border_color(closeBtn, kaspa_green, 0);
  lv_obj_set_style_shadow_width(closeBtn, 0, 0);
  lv_obj_set_style_bg_color(closeBtn, kaspa_green, LV_STATE_PRESSED);
  lv_obj_add_event_cb(closeBtn, close_main_menu_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *closeLabel = lv_label_create(closeBtn);
  lv_label_set_text(closeLabel, "CLOSE");
  lv_obj_set_style_text_font(closeLabel, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(closeLabel, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_color(closeLabel, lv_color_hex(0x000000), LV_STATE_PRESSED);
  lv_obj_center(closeLabel);
}

void close_main_menu_cb(lv_event_t *e) {
  if (e->code == LV_EVENT_CLICKED && mainMenu) {
    lv_obj_del(mainMenu);
    mainMenu = nullptr;
  }
}

void price_info_btn_cb(lv_event_t *e) {
  if (e->code == LV_EVENT_CLICKED) {
    if (mainMenu) {
      lv_obj_del(mainMenu);
      mainMenu = nullptr;
    }
    show_price_info_screen();
  }
}

void mining_stats_btn_cb(lv_event_t *e) {
  if (e->code == LV_EVENT_CLICKED) {
    if (mainMenu) {
      lv_obj_del(mainMenu);
      mainMenu = nullptr;
    }
    #if ENABLE_MINING_STATS_SCREEN
    show_mining_stats_screen();
    #else
    // Stats screen disabled to save space - mining stats shown on main screen
    #endif
  }
}

void back_to_main_cb(lv_event_t *e) {
  if (e->code == LV_EVENT_CLICKED) {
    // Close any open screen
    if (priceInfoScreen) {
      lv_obj_del(priceInfoScreen);
      priceInfoScreen = nullptr;
    }
    if (miningStatsScreen) {
      lv_obj_del(miningStatsScreen);
      miningStatsScreen = nullptr;
    }
  }
}

// Global variables for touch interaction
static lv_obj_t *touchPriceLabel = nullptr;
static lv_obj_t *touchTimestampLabel = nullptr;
static lv_obj_t *currentPriceChart = nullptr;
static lv_chart_series_t *currentPriceSeries = nullptr;
static int selectedChartPoint = -1;
static float chartMinPrice = 0.0;
static float chartMaxPrice = 0.0;

void show_price_info_screen() {
  if (priceInfoScreen) {
    lv_obj_del(priceInfoScreen);
    priceInfoScreen = nullptr;
  }

  lv_color_t kaspa_green = lv_color_hex(0x49EACB);
  lv_color_t text_white = lv_color_hex(0xFFFFFF);
  lv_color_t text_dim = lv_color_hex(0x888888);

  priceInfoScreen = lv_obj_create(lv_scr_act());
  lv_obj_set_size(priceInfoScreen, 800, 480);
  lv_obj_align(priceInfoScreen, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(priceInfoScreen, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(priceInfoScreen, 3, 0);
  lv_obj_set_style_border_color(priceInfoScreen, kaspa_green, 0);
  lv_obj_clear_flag(priceInfoScreen, LV_OBJ_FLAG_SCROLLABLE);

  // Slide-in animation from right
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, priceInfoScreen);
  lv_anim_set_values(&a, 800, 0);
  lv_anim_set_time(&a, 300);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_x);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);

  // BACK button (top-left, matches MENU button position)
  lv_obj_t *backBtn = lv_btn_create(priceInfoScreen);
  lv_obj_set_size(backBtn, 100, 35);
  lv_obj_align(backBtn, LV_ALIGN_TOP_LEFT, 15, 15);
  lv_obj_set_style_radius(backBtn, 8, 0);
  lv_obj_set_style_bg_color(backBtn, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(backBtn, 2, 0);
  lv_obj_set_style_border_color(backBtn, kaspa_green, 0);
  lv_obj_set_style_shadow_width(backBtn, 0, 0);
  lv_obj_set_style_bg_color(backBtn, kaspa_green, LV_STATE_PRESSED);
  lv_obj_add_event_cb(backBtn, back_to_main_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *backLabel = lv_label_create(backBtn);
  lv_label_set_text(backLabel, "BACK");
  lv_obj_set_style_text_font(backLabel, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(backLabel, lv_color_hex(0xFFFFFF), 0);  // White text
  lv_obj_set_style_text_color(backLabel, lv_color_hex(0x000000), LV_STATE_PRESSED);
  lv_obj_center(backLabel);

  // Market Cap Box (left side, below back button)
  lv_obj_t *marketCapTitle = lv_label_create(priceInfoScreen);
  lv_label_set_text(marketCapTitle, "MARKET CAP");
  lv_obj_set_style_text_font(marketCapTitle, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(marketCapTitle, kaspa_green, 0);
  lv_obj_set_width(marketCapTitle, 180);  // Same width as box below
  lv_obj_set_style_text_align(marketCapTitle, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(marketCapTitle, LV_ALIGN_TOP_LEFT, 15, 65);  // Same X as box, text centered within

  lv_obj_t *marketCapBox = lv_obj_create(priceInfoScreen);
  lv_obj_set_size(marketCapBox, 180, 50);
  lv_obj_align(marketCapBox, LV_ALIGN_TOP_LEFT, 15, 80);
  lv_obj_set_style_radius(marketCapBox, 8, 0);
  lv_obj_set_style_bg_color(marketCapBox, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(marketCapBox, 2, 0);
  lv_obj_set_style_border_color(marketCapBox, lv_color_hex(COLOR_SLATE_GRAY), 0);  // Neutral border
  lv_obj_set_style_shadow_width(marketCapBox, 0, 0);
  lv_obj_clear_flag(marketCapBox, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *marketCapLabel = lv_label_create(marketCapBox);
  lv_label_set_text(marketCapLabel, market_cap.c_str());
  lv_obj_set_style_text_font(marketCapLabel, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(marketCapLabel, text_white, 0);
  lv_obj_center(marketCapLabel);

  // "Current Price" label (aligned with top of BACK button)
  lv_obj_t *currentPriceTitle = lv_label_create(priceInfoScreen);
  lv_label_set_text(currentPriceTitle, "CURRENT PRICE");
  lv_obj_set_style_text_font(currentPriceTitle, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(currentPriceTitle, kaspa_green, 0);
  lv_obj_align(currentPriceTitle, LV_ALIGN_TOP_RIGHT, -80, 15);

  // Current Price Box (top-right corner, aligned below label)
  lv_obj_t *priceBox = lv_obj_create(priceInfoScreen);
  lv_obj_set_size(priceBox, 240, 90);
  lv_obj_align(priceBox, LV_ALIGN_TOP_RIGHT, -15, 30);
  lv_obj_set_style_radius(priceBox, 8, 0);
  lv_obj_set_style_bg_color(priceBox, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(priceBox, 2, 0);
  lv_obj_set_style_border_color(priceBox, lv_color_hex(COLOR_SLATE_GRAY), 0);  // Neutral border
  lv_obj_set_style_shadow_width(priceBox, 0, 0);
  lv_obj_clear_flag(priceBox, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *currentPriceLabel = lv_label_create(priceBox);
  lv_label_set_text(currentPriceLabel, price_usd.c_str());
  lv_obj_set_style_text_font(currentPriceLabel, &lv_font_montserrat_38, 0);
  lv_obj_set_style_text_color(currentPriceLabel, text_white, 0);
  lv_obj_align(currentPriceLabel, LV_ALIGN_CENTER, 0, -20);

  lv_obj_t *changeDisplayLabel = lv_label_create(priceBox);
  lv_label_set_text(changeDisplayLabel, change_24h.c_str());
  lv_obj_set_style_text_font(changeDisplayLabel, &lv_font_montserrat_22, 0);

  // Parse change to set color
  if (change_24h.length() > 0 && change_24h[0] == '+') {
    lv_obj_set_style_text_color(changeDisplayLabel, lv_color_hex(0x00FF00), 0);
  } else if (change_24h.length() > 0 && change_24h[0] == '-') {
    lv_obj_set_style_text_color(changeDisplayLabel, lv_color_hex(0xFF0000), 0);
  } else {
    lv_obj_set_style_text_color(changeDisplayLabel, text_dim, 0);
  }
  lv_obj_align(changeDisplayLabel, LV_ALIGN_BOTTOM_MID, 0, 10);

  // Chart Container (fills most of screen, no border)
  lv_obj_t *chartCard = lv_obj_create(priceInfoScreen);
  lv_obj_set_size(chartCard, 720, 340);
  lv_obj_set_pos(chartCard, 20, 130);
  lv_obj_set_style_radius(chartCard, 0, 0);
  lv_obj_set_style_bg_color(chartCard, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(chartCard, 0, 0);
  lv_obj_set_style_shadow_width(chartCard, 0, 0);
  lv_obj_set_style_pad_all(chartCard, 0, 0);
  lv_obj_clear_flag(chartCard, LV_OBJ_FLAG_SCROLLABLE);

  // Calculate price range dynamically
  float minPrice = 999999.0;
  float maxPrice = 0.0;
  for (int i = 0; i < PRICE_HISTORY_POINTS; i++) {
    if (priceHistory[i] > 0) {
      if (priceHistory[i] < minPrice) minPrice = priceHistory[i];
      if (priceHistory[i] > maxPrice) maxPrice = priceHistory[i];
    }
  }

  // Add 10% padding to range
  if (maxPrice > minPrice) {
    float padding = (maxPrice - minPrice) * 0.1;
    minPrice -= padding;
    maxPrice += padding;
  } else {
    // Default range if no data
    minPrice = 0.0;
    maxPrice = 0.2;
  }

  // Store for touch interaction
  chartMinPrice = minPrice;
  chartMaxPrice = maxPrice;

  // Y-axis labels (price on left side)
  char labelBuf[16];

  // Max price label
  lv_obj_t *maxLabel = lv_label_create(chartCard);
  snprintf(labelBuf, sizeof(labelBuf), "$%.4f", maxPrice);
  lv_label_set_text(maxLabel, labelBuf);
  lv_obj_set_style_text_font(maxLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(maxLabel, kaspa_green, 0);
  lv_obj_set_pos(maxLabel, 5, 15);

  // Mid price label
  lv_obj_t *midLabel = lv_label_create(chartCard);
  snprintf(labelBuf, sizeof(labelBuf), "$%.4f", (maxPrice + minPrice) / 2.0);
  lv_label_set_text(midLabel, labelBuf);
  lv_obj_set_style_text_font(midLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(midLabel, kaspa_green, 0);
  lv_obj_set_pos(midLabel, 5, 145);

  // Min price label
  lv_obj_t *minLabel = lv_label_create(chartCard);
  snprintf(labelBuf, sizeof(labelBuf), "$%.4f", minPrice);
  lv_label_set_text(minLabel, labelBuf);
  lv_obj_set_style_text_font(minLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(minLabel, kaspa_green, 0);
  lv_obj_set_pos(minLabel, 5, 280);

  // X-axis labels (time at bottom)
  lv_obj_t *time24hLabel = lv_label_create(chartCard);
  lv_label_set_text(time24hLabel, "24h ago");
  lv_obj_set_style_text_font(time24hLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(time24hLabel, kaspa_green, 0);
  lv_obj_set_pos(time24hLabel, 65, 305);

  lv_obj_t *time12hLabel = lv_label_create(chartCard);
  lv_label_set_text(time12hLabel, "12h ago");
  lv_obj_set_style_text_font(time12hLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(time12hLabel, kaspa_green, 0);
  lv_obj_set_pos(time12hLabel, 315, 305);

  lv_obj_t *timeNowLabel = lv_label_create(chartCard);
  lv_label_set_text(timeNowLabel, "Now");
  lv_obj_set_style_text_font(timeNowLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(timeNowLabel, kaspa_green, 0);
  lv_obj_set_pos(timeNowLabel, 665, 305);

  // Create chart
  lv_obj_t *priceChart = lv_chart_create(chartCard);
  lv_obj_set_size(priceChart, 640, 275);
  lv_obj_set_pos(priceChart, 65, 15);
  lv_chart_set_type(priceChart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(priceChart, PRICE_HISTORY_POINTS);
  lv_chart_set_range(priceChart, LV_CHART_AXIS_PRIMARY_Y, (int)(minPrice * 10000), (int)(maxPrice * 10000));
  lv_chart_set_update_mode(priceChart, LV_CHART_UPDATE_MODE_SHIFT);

  // Store for touch interaction
  currentPriceChart = priceChart;

  // Chart styling
  lv_obj_set_style_bg_color(priceChart, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(priceChart, LV_OPA_100, 0);
  lv_obj_set_style_border_width(priceChart, 1, 0);
  lv_obj_set_style_border_color(priceChart, kaspa_green, 0);
  lv_obj_set_style_border_opa(priceChart, LV_OPA_30, 0);

  // Grid lines
  lv_chart_set_div_line_count(priceChart, 5, 12);
  lv_obj_set_style_line_color(priceChart, kaspa_green, LV_PART_MAIN);
  lv_obj_set_style_line_opa(priceChart, LV_OPA_10, LV_PART_MAIN);

  // Determine chart color based on 24h change
  // Parse change_24h string to determine if price is up or down
  lv_color_t chartColor;
  lv_color_t dotColor;
  bool isPriceUp = (change_24h.length() > 0 && change_24h[0] == '+');

  if (isPriceUp) {
    chartColor = lv_color_hex(0x00FF00);  // Regular green for up
    dotColor = lv_color_hex(0x00CC00);    // Darker green for dots
  } else {
    chartColor = lv_color_hex(0xFF0000);  // Red for down
    dotColor = lv_color_hex(0xCC0000);    // Darker red for dots
  }

  // Add single series with color based on 24h performance
  lv_chart_series_t *priceSeries = lv_chart_add_series(priceChart, chartColor, LV_CHART_AXIS_PRIMARY_Y);
  currentPriceSeries = priceSeries;  // Store for touch interaction

  // Line styling
  lv_obj_set_style_line_width(priceChart, 3, LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(priceChart, LV_OPA_40, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(priceChart, chartColor, LV_PART_ITEMS);
  lv_obj_set_style_bg_grad_color(priceChart, lv_color_hex(0x0a0a0f), LV_PART_ITEMS);
  lv_obj_set_style_bg_grad_dir(priceChart, LV_GRAD_DIR_VER, LV_PART_ITEMS);

  // Set dot color and make them circular
  lv_obj_set_style_size(priceChart, 8, LV_PART_INDICATOR);  // Dot size
  lv_obj_set_style_bg_color(priceChart, dotColor, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(priceChart, LV_OPA_100, LV_PART_INDICATOR);
  lv_obj_set_style_radius(priceChart, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);  // Make dots circular

  lv_chart_set_series_color(priceChart, priceSeries, chartColor);

  // Populate chart with price history in chronological order
  for (int i = 0; i < PRICE_HISTORY_POINTS; i++) {
    int index = (priceIndex + i) % PRICE_HISTORY_POINTS;
    if (priceHistory[index] > 0) {
      lv_chart_set_next_value(priceChart, priceSeries, (int)(priceHistory[index] * 10000));
    } else {
      // Use LV_CHART_POINT_NONE to skip invalid points (no line will be drawn)
      lv_chart_set_next_value(priceChart, priceSeries, LV_CHART_POINT_NONE);
    }
  }

  // Add touch interaction for chart
  lv_obj_add_event_cb(priceChart, chart_touch_cb, LV_EVENT_PRESSED, NULL);
  lv_obj_add_event_cb(priceChart, chart_touch_cb, LV_EVENT_PRESSING, NULL);
  lv_obj_add_event_cb(priceChart, chart_release_cb, LV_EVENT_RELEASED, NULL);

  // Add draw event to customize selected point color
  lv_obj_add_event_cb(priceChart, chart_draw_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);

  // Create touch price label (shown above chart, more centered)
  touchPriceLabel = lv_label_create(priceInfoScreen);
  lv_label_set_text(touchPriceLabel, "Tap dot for\nprice");
  lv_obj_set_style_text_font(touchPriceLabel, &lv_font_montserrat_32, 0);  // Larger
  lv_obj_set_style_text_color(touchPriceLabel, lv_color_hex(0x00FF00), 0);  // Bright green
  lv_obj_set_style_bg_opa(touchPriceLabel, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(touchPriceLabel, 0, 0);
  lv_obj_set_style_text_align(touchPriceLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(touchPriceLabel, LV_ALIGN_TOP_MID, -30, 15);  // Shifted left to center between market cap and price cards

  // Create timestamp label below price (aligned with price)
  touchTimestampLabel = lv_label_create(priceInfoScreen);
  lv_label_set_text(touchTimestampLabel, "");
  lv_obj_set_style_text_font(touchTimestampLabel, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(touchTimestampLabel, kaspa_green, 0);
  lv_obj_set_style_bg_opa(touchTimestampLabel, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(touchTimestampLabel, 0, 0);
  lv_obj_set_style_text_align(touchTimestampLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(touchTimestampLabel, LV_ALIGN_TOP_MID, -30, 85);  // Shifted left to match price label
  lv_obj_add_flag(touchTimestampLabel, LV_OBJ_FLAG_HIDDEN);
}

// Custom draw callback to change selected point color
void chart_draw_cb(lv_event_t *e) {
  lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);

  // Check if we're drawing a chart indicator (dot)
  if (dsc->part == LV_PART_INDICATOR) {
    // Convert chronological index to actual array index
    int chronologicalIndex = dsc->id;
    int actualIndex = (priceIndex + chronologicalIndex) % PRICE_HISTORY_POINTS;

    // If this is the selected point, change its color
    if (actualIndex == selectedChartPoint) {
      dsc->rect_dsc->bg_color = lv_color_hex(COLOR_VIVID_ORANGE);  // Coral color for selected point
    }
  }
}

void chart_touch_cb(lv_event_t *e) {
  if (!currentPriceChart || !touchPriceLabel || !touchTimestampLabel) return;

  lv_indev_t *indev = lv_indev_get_act();
  if (!indev) return;

  lv_point_t point;
  lv_indev_get_point(indev, &point);

  // Get chart coordinates relative to its parent
  lv_area_t chart_area;
  lv_obj_get_coords(currentPriceChart, &chart_area);

  // Calculate relative position within chart
  int relX = point.x - chart_area.x1;
  int chartWidth = chart_area.x2 - chart_area.x1;

  if (relX < 0 || relX > chartWidth) return;

  // Map X position to data point in chronological order
  int chronologicalIndex = (relX * PRICE_HISTORY_POINTS) / chartWidth;
  if (chronologicalIndex < 0) chronologicalIndex = 0;
  if (chronologicalIndex >= PRICE_HISTORY_POINTS) chronologicalIndex = PRICE_HISTORY_POINTS - 1;

  // Convert chronological index to actual array index (accounting for circular buffer)
  int dataIndex = (priceIndex + chronologicalIndex) % PRICE_HISTORY_POINTS;

  // Get price at this index
  float price = priceHistory[dataIndex];
  if (price <= 0) return;

  // Update and show price label
  char priceBuf[32];
  snprintf(priceBuf, sizeof(priceBuf), "$%.4f", price);
  lv_label_set_text(touchPriceLabel, priceBuf);

  // Format and show timestamp with user's timezone
  uint64_t timestamp = priceTimestamps[dataIndex];
  if (timestamp > 0) {
    // Convert Unix timestamp (ms) to seconds
    time_t timeInSeconds = timestamp / 1000ULL;

    Serial.printf("DEBUG: Chart touch - dataIndex: %d, timestamp_ms: %llu, time_t: %ld\n",
                  dataIndex, (unsigned long long)timestamp, (long)timeInSeconds);

    struct tm timeinfo;

    // time() returns UTC, so use localtime_r to convert to local timezone
    localtime_r(&timeInSeconds, &timeinfo);

    Serial.printf("DEBUG: After localtime_r: %04d-%02d-%02d %02d:%02d:%02d\n",
                  timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                  timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

    // Convert to 12-hour format with AM/PM
    int hour12 = timeinfo.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;  // 0 should be 12 AM/PM
    const char* ampm = (timeinfo.tm_hour < 12) ? "AM" : "PM";

    char timeBuf[64];
    snprintf(timeBuf, sizeof(timeBuf), "%02d/%02d %d:%02d %s",
             timeinfo.tm_mon + 1, timeinfo.tm_mday,
             hour12, timeinfo.tm_min, ampm);

    Serial.printf("DEBUG: Formatted string: %s\n", timeBuf);

    lv_label_set_text(touchTimestampLabel, timeBuf);
    lv_obj_clear_flag(touchTimestampLabel, LV_OBJ_FLAG_HIDDEN);
  }

  // Track selected point for color change
  selectedChartPoint = dataIndex;

  // Trigger chart redraw to update point colors
  lv_obj_invalidate(currentPriceChart);
}

void chart_release_cb(lv_event_t *e) {
  // Price stays visible after release - do nothing
}

#if ENABLE_MINING_STATS_SCREEN
void show_mining_stats_screen() {
  if (miningStatsScreen) {
    lv_obj_del(miningStatsScreen);
    miningStatsScreen = nullptr;
  }

  // Reset label pointers
  miningStatsDeviceHashLabel = nullptr;
  miningStatsNetworkHashLabel = nullptr;
  miningStatsAcceptedLabel = nullptr;
  miningStatsRejectedLabel = nullptr;
  miningStatsBlocksLabel = nullptr;

  lv_color_t kaspa_green = lv_color_hex(0x49EACB);
  lv_color_t text_white = lv_color_hex(0xFFFFFF);
  lv_color_t text_dim = lv_color_hex(0x888888);

  miningStatsScreen = lv_obj_create(lv_scr_act());
  lv_obj_set_size(miningStatsScreen, 800, 480);
  lv_obj_align(miningStatsScreen, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(miningStatsScreen, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(miningStatsScreen, 3, 0);
  lv_obj_set_style_border_color(miningStatsScreen, kaspa_green, 0);
  lv_obj_clear_flag(miningStatsScreen, LV_OBJ_FLAG_SCROLLABLE);

  // Slide-in animation from right
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, miningStatsScreen);
  lv_anim_set_values(&a, 800, 0);
  lv_anim_set_time(&a, 300);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_x);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);

  // BACK button
  lv_obj_t *backBtn = lv_btn_create(miningStatsScreen);
  lv_obj_set_size(backBtn, 100, 35);
  lv_obj_align(backBtn, LV_ALIGN_TOP_LEFT, 15, 15);
  lv_obj_set_style_radius(backBtn, 8, 0);
  lv_obj_set_style_bg_color(backBtn, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(backBtn, 2, 0);
  lv_obj_set_style_border_color(backBtn, kaspa_green, 0);
  lv_obj_set_style_shadow_width(backBtn, 0, 0);
  lv_obj_set_style_bg_color(backBtn, kaspa_green, LV_STATE_PRESSED);
  lv_obj_add_event_cb(backBtn, back_to_main_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *backLabel = lv_label_create(backBtn);
  lv_label_set_text(backLabel, "BACK");
  lv_obj_set_style_text_font(backLabel, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(backLabel, lv_color_hex(0xFFFFFF), 0);  // White text
  lv_obj_set_style_text_color(backLabel, lv_color_hex(0x000000), LV_STATE_PRESSED);
  lv_obj_center(backLabel);

  lv_obj_t *title = lv_label_create(miningStatsScreen);
  lv_label_set_text(title, "MINING STATS");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);  // Larger title
  lv_obj_set_style_text_color(title, kaspa_green, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);  // Better centered vertically

  // Two-column layout
  int leftColX = 30;
  int rightColX = 420;
  int topRowY = 70;
  int rowHeight = 55;

  // TOP ROW - Device Hashrate and Blocks Found (LARGE & BOLD - most important)
  // Device Hashrate (left side of top row)
  lv_obj_t *deviceHashLabelTitle = lv_label_create(miningStatsScreen);
  lv_label_set_text(deviceHashLabelTitle, "Device Hashrate");
  lv_obj_set_style_text_font(deviceHashLabelTitle, &lv_font_montserrat_20, 0);  // Larger title
  lv_obj_set_style_text_color(deviceHashLabelTitle, kaspa_green, 0);  // Kaspa green instead of dim
  lv_obj_align(deviceHashLabelTitle, LV_ALIGN_TOP_LEFT, leftColX, topRowY);

  miningStatsDeviceHashLabel = lv_label_create(miningStatsScreen);
  String hrText = (currentHashrate > 1000) ?
                  String(currentHashrate / 1000.0, 2) + " KH/s" :
                  String(currentHashrate, 0) + " H/s";
  lv_label_set_text(miningStatsDeviceHashLabel, hrText.c_str());
  lv_obj_set_style_text_font(miningStatsDeviceHashLabel, &lv_font_montserrat_48, 0);  // Much larger (48 instead of 28)
  lv_obj_set_style_text_color(miningStatsDeviceHashLabel, kaspa_green, 0);
  lv_obj_align(miningStatsDeviceHashLabel, LV_ALIGN_TOP_LEFT, leftColX, topRowY + 30);

  // Blocks Found (right side of top row)
  lv_obj_t *blocksLabelTitle = lv_label_create(miningStatsScreen);
  lv_label_set_text(blocksLabelTitle, "Blocks Found");
  lv_obj_set_style_text_font(blocksLabelTitle, &lv_font_montserrat_20, 0);  // Larger title
  lv_obj_set_style_text_color(blocksLabelTitle, lv_color_hex(0xFFD700), 0);  // Gold instead of dim
  lv_obj_align(blocksLabelTitle, LV_ALIGN_TOP_LEFT, rightColX, topRowY);

  miningStatsBlocksLabel = lv_label_create(miningStatsScreen);
  lv_label_set_text_fmt(miningStatsBlocksLabel, "%u", blocksFound);
  lv_obj_set_style_text_font(miningStatsBlocksLabel, &lv_font_montserrat_48, 0);  // Much larger (48 instead of 28)
  lv_obj_set_style_text_color(miningStatsBlocksLabel, lv_color_hex(0xFFD700), 0);
  lv_obj_align(miningStatsBlocksLabel, LV_ALIGN_TOP_LEFT, rightColX, topRowY + 30);

  // LEFT COLUMN - Mining Stats
  int statsStartY = topRowY + rowHeight + 50;  // Increased spacing from 20 to 50 for more room after large fonts

  // Jobs Received
  lv_obj_t *jobsLabelTitle = lv_label_create(miningStatsScreen);
  lv_label_set_text(jobsLabelTitle, "Jobs Received");
  lv_obj_set_style_text_font(jobsLabelTitle, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(jobsLabelTitle, text_dim, 0);
  lv_obj_align(jobsLabelTitle, LV_ALIGN_TOP_LEFT, leftColX, statsStartY);

  lv_obj_t *miningStatsJobsLabel = lv_label_create(miningStatsScreen);
  lv_label_set_text_fmt(miningStatsJobsLabel, "%u", jobsReceived);
  lv_obj_set_style_text_font(miningStatsJobsLabel, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(miningStatsJobsLabel, text_white, 0);
  lv_obj_align(miningStatsJobsLabel, LV_ALIGN_TOP_LEFT, leftColX, statsStartY + 24);

  // Shares Submitted
  lv_obj_t *submittedLabelTitle = lv_label_create(miningStatsScreen);
  lv_label_set_text(submittedLabelTitle, "Shares Submitted");
  lv_obj_set_style_text_font(submittedLabelTitle, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(submittedLabelTitle, text_dim, 0);
  lv_obj_align(submittedLabelTitle, LV_ALIGN_TOP_LEFT, leftColX, statsStartY + rowHeight);

  lv_obj_t *miningStatsSubmittedLabel = lv_label_create(miningStatsScreen);
  lv_label_set_text_fmt(miningStatsSubmittedLabel, "%u", sharesSubmitted);
  lv_obj_set_style_text_font(miningStatsSubmittedLabel, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(miningStatsSubmittedLabel, text_white, 0);
  lv_obj_align(miningStatsSubmittedLabel, LV_ALIGN_TOP_LEFT, leftColX, statsStartY + rowHeight + 24);

  // Shares Accepted
  lv_obj_t *acceptedLabelTitle = lv_label_create(miningStatsScreen);
  lv_label_set_text(acceptedLabelTitle, "Shares Accepted");
  lv_obj_set_style_text_font(acceptedLabelTitle, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(acceptedLabelTitle, text_dim, 0);
  lv_obj_align(acceptedLabelTitle, LV_ALIGN_TOP_LEFT, leftColX, statsStartY + rowHeight * 2);

  miningStatsAcceptedLabel = lv_label_create(miningStatsScreen);
  lv_label_set_text_fmt(miningStatsAcceptedLabel, "%u", sharesAccepted);
  lv_obj_set_style_text_font(miningStatsAcceptedLabel, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(miningStatsAcceptedLabel, lv_color_hex(0x00FF00), 0);
  lv_obj_align(miningStatsAcceptedLabel, LV_ALIGN_TOP_LEFT, leftColX, statsStartY + rowHeight * 2 + 24);

  // Shares Rejected
  lv_obj_t *rejectedLabelTitle = lv_label_create(miningStatsScreen);
  lv_label_set_text(rejectedLabelTitle, "Shares Rejected");
  lv_obj_set_style_text_font(rejectedLabelTitle, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(rejectedLabelTitle, text_dim, 0);
  lv_obj_align(rejectedLabelTitle, LV_ALIGN_TOP_LEFT, leftColX, statsStartY + rowHeight * 3);

  miningStatsRejectedLabel = lv_label_create(miningStatsScreen);
  lv_label_set_text_fmt(miningStatsRejectedLabel, "%u", sharesRejected);
  lv_obj_set_style_text_font(miningStatsRejectedLabel, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(miningStatsRejectedLabel, lv_color_hex(0xFF0000), 0);
  lv_obj_align(miningStatsRejectedLabel, LV_ALIGN_TOP_LEFT, leftColX, statsStartY + rowHeight * 3 + 24);

  // RIGHT COLUMN - Hashrate History
  lv_obj_t *historyTitle = lv_label_create(miningStatsScreen);
  lv_label_set_text(historyTitle, "Hashrate History");
  lv_obj_set_style_text_font(historyTitle, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(historyTitle, kaspa_green, 0);
  lv_obj_align(historyTitle, LV_ALIGN_TOP_LEFT, rightColX, statsStartY);

  // Display up to 6 historical hashrate entries
  int historyStartY = statsStartY + 30;
  int historySpacing = 30;

  for (int i = 0; i < 6 && i < MINER_HASHRATE_POINTS; i++) {
    int idx = (minerHashrateIndex - 1 - i + MINER_HASHRATE_POINTS) % MINER_HASHRATE_POINTS;
    float hashrate = minerHashrateHistory[idx];
    unsigned long timestamp = minerHashrateTimestamps[idx];

    if (hashrate > 0 && timestamp > 0) {
      unsigned long minutesAgo = (millis() - timestamp) / 60000;

      lv_obj_t *historyEntry = lv_label_create(miningStatsScreen);
      String hrText = (hashrate > 1000) ?
                      String(hashrate / 1000.0, 1) + " KH/s" :
                      String(hashrate, 0) + " H/s";
      String entryText = hrText + " - " + String(minutesAgo) + " min";
      lv_label_set_text(historyEntry, entryText.c_str());
      lv_obj_set_style_text_font(historyEntry, &lv_font_montserrat_14, 0);
      lv_obj_set_style_text_color(historyEntry, text_white, 0);
      lv_obj_align(historyEntry, LV_ALIGN_TOP_LEFT, rightColX, historyStartY + i * historySpacing);
    }
  }
}
#endif // ENABLE_MINING_STATS_SCREEN

// Helper function to extract text between XML tags
String extractXMLTag(const String& xml, const String& tag, int startPos = 0) {
  String openTag = "<" + tag + ">";
  String closeTag = "</" + tag + ">";

  int start = xml.indexOf(openTag, startPos);
  if (start == -1) return "";
  start += openTag.length();

  int end = xml.indexOf(closeTag, start);
  if (end == -1) return "";

  return xml.substring(start, end);
}

// Clean problematic UTF-8 characters that ESP32 can't display
String cleanTextForDisplay(String text) {
  // Replace common problematic characters with ASCII equivalents
  text.replace("\u2019", "'");   // Right single quotation mark
  text.replace("\u2018", "'");   // Left single quotation mark
  text.replace("\u201C", "\"");  // Left double quotation mark
  text.replace("\u201D", "\"");  // Right double quotation mark
  text.replace("\u2013", "-");   // En dash
  text.replace("\u2014", "-");   // Em dash
  text.replace("\u2026", "..."); // Horizontal ellipsis
  text.replace("\u00A0", " ");   // Non-breaking space
  text.replace("\u00E9", "e");   // é
  text.replace("\u00E8", "e");   // è
  text.replace("\u00EA", "e");   // ê
  text.replace("\u00C9", "E");   // É
  text.replace("\u2022", "*");   // Bullet point

  // Remove any remaining non-ASCII characters (show as space)
  String cleaned = "";
  for (unsigned int i = 0; i < text.length(); i++) {
    char c = text.charAt(i);
    // Keep printable ASCII characters (32-126) and newlines/tabs
    if ((c >= 32 && c <= 126) || c == '\n' || c == '\r' || c == '\t') {
      cleaned += c;
    } else if (c < 0) {
      // Multi-byte UTF-8 character - skip it
      // UTF-8 continuation bytes start with 10xxxxxx (values 128-191 or -128 to -65 in signed char)
      continue;
    } else {
      cleaned += ' ';  // Replace with space
    }
  }

  return cleaned;
}

// NOTE: fetchPriceHistory() removed - we now build the chart from live data collection
// The chart populates automatically as prices are fetched every few minutes
// No API key needed - completely free!

void updateHashrateChart() {
  Serial.printf("updateHashrateChart called - networkHashrate = %.2f PH/s\n", networkHashrate);
  
  // Add new hashrate value to history
  hashrateHistory[hashrateIndex] = networkHashrate;
  hashrateTimestamps[hashrateIndex] = millis();
  hashrateIndex = (hashrateIndex + 1) % HASHRATE_POINTS;
  
  // Calculate statistics and find min/max
  float sum = 0, maxVal = 0, minVal = 999999;
  int count = 0;
  for (int i = 0; i < HASHRATE_POINTS; i++) {
    if (hashrateHistory[i] > 0) {
      sum += hashrateHistory[i];
      if (hashrateHistory[i] > maxVal) maxVal = hashrateHistory[i];
      if (hashrateHistory[i] < minVal) minVal = hashrateHistory[i];
      count++;
    }
  }
  float avgVal = (count > 0) ? sum / count : networkHashrate;
  
  // Auto-scale the chart with 10% padding
  if (count > 0) {
    float range = maxVal - minVal;
    float padding = range * 0.1;
    if (padding < 10) padding = 10;
    
    int chartMin = (int)(minVal - padding);
    int chartMax = (int)(maxVal + padding);
    
    if (chartMax - chartMin < 20) {
      int center = (chartMax + chartMin) / 2;
      chartMin = center - 10;
      chartMax = center + 10;
    }
    
    lv_chart_set_range(hashrateChart, LV_CHART_AXIS_PRIMARY_Y, chartMin, chartMax);
    Serial.printf("Chart Y-axis scaled: %d to %d PH/s\n", chartMin, chartMax);
    
    // UPDATE Y-AXIS LABELS dynamically!
    int midValue = (chartMax + chartMin) / 2;
    
    if (yAxisMaxLabel) {
      lv_label_set_text_fmt(yAxisMaxLabel, "%d", chartMax);
    }
    if (yAxisMidLabel) {
      lv_label_set_text_fmt(yAxisMidLabel, "%d", midValue);
    }
    if (yAxisMinLabel) {
      lv_label_set_text_fmt(yAxisMinLabel, "%d", chartMin);
    }
  }
  
  // Update chart with current value (skip if invalid/zero to avoid line drop)
  if (networkHashrate > 0) {
    lv_chart_set_next_value(hashrateChart, hashrateSeries, (int)networkHashrate);
  } else {
    lv_chart_set_next_value(hashrateChart, hashrateSeries, LV_CHART_POINT_NONE);
  }
  
  Serial.printf("Stats - count: %d, sum: %.2f, avg: %.2f, max: %.2f, min: %.2f\n", 
                count, sum, avgVal, maxVal, minVal);
  
  // Update big hashrate display (number with unit)
  if (bigHashrateLabel) {
    lv_label_set_text_fmt(bigHashrateLabel, "%d PH/s", (int)networkHashrate);
  }
  
  // Update stats labels
  if (avgHashLabel) {
    lv_label_set_text_fmt(avgHashLabel, "Avg: %d PH/s", (int)avgVal);
  }
  
  if (maxHashLabel) {
    lv_label_set_text_fmt(maxHashLabel, "Peak: %d PH/s", (int)maxVal);
  }


  Serial.printf("Chart updated - Current: %.1f, Avg: %.1f, Peak: %.1f PH/s\n",
                networkHashrate, avgVal, maxVal);
}

void updateMinerDisplay() {
    // Skip all updates during firmware update to prevent crashes
    if (firmwareUpdateInProgress) return;
    if (!minerStatusLabel) return;

    // Cache previous values to avoid unnecessary UI updates
    static float lastDisplayedHashrate = -1;
    static uint32_t lastJobs = 0, lastSubmitted = 0, lastAccepted = 0, lastRejected = 0, lastBlocks = 0;
    static bool lastMiningState = false;

    // TEST STEP 1: Calculate hashrate ONLY (no LVGL calls yet)
    unsigned long now = millis();
    unsigned long timeDiff = now - last_hash_check;
    if (timeDiff >= 1000 && timeDiff > 0) {
        // Atomically read and reset hashes_done
        uint32_t hashes_snapshot = hashes_done;
        hashes_done = 0;
        currentHashrate = (float)hashes_snapshot / (timeDiff / 1000.0);
        last_hash_check = now;
    }

    // TEST STEP 2: Add mining status label update
    // Track pool connection state for UI updates
    static bool lastPoolConnected = false;
    bool isPoolConnected = stratumClient.connected();

    if (miningEnabled != lastMiningState || isPoolConnected != lastPoolConnected) {
        if (!miningEnabled) {
            lv_label_set_text(minerStatusLabel, "Mining Paused");
            lv_obj_set_style_text_color(minerStatusLabel, lv_color_hex(COLOR_TEXT_SECONDARY), 0);
        } else if (!isPoolConnected) {
            lv_label_set_text(minerStatusLabel, "Pool Disconnected");
            lv_obj_set_style_text_color(minerStatusLabel, lv_color_hex(0xFF4444), 0);  // Red
        } else {
            lv_label_set_text(minerStatusLabel, "Mining Active");
            lv_obj_set_style_text_color(minerStatusLabel, lv_color_hex(0x00FF00), 0);  // Green
        }
        lastMiningState = miningEnabled;
        lastPoolConnected = isPoolConnected;
    }

    // TEST STEP 3: Add hashrate display
    if (miningEnabled) {
        // Only update hashrate if it changed significantly (>5% change or first time)
        if (lastDisplayedHashrate < 0 || abs(currentHashrate - lastDisplayedHashrate) > (lastDisplayedHashrate * 0.05)) {
            // Use static buffer to avoid dangling pointer bug
            static char hrText[32];
            if (currentHashrate > 1000) {
                snprintf(hrText, sizeof(hrText), "%.2f KH/s", currentHashrate / 1000.0);
            } else {
                snprintf(hrText, sizeof(hrText), "%.0f H/s", currentHashrate);
            }
            // Check pointer before use
            if (minerHashrateLabel) {
                lv_label_set_text(minerHashrateLabel, hrText);
            }

            // Update stats screen hashrate too if visible
            if (miningStatsScreen && miningStatsDeviceHashLabel) {
                lv_label_set_text(miningStatsDeviceHashLabel, hrText);
            }
            lastDisplayedHashrate = currentHashrate;
        }

        // TEST STEP 4: Add all stats labels (jobs, shares, blocks)
        // CRITICAL: Add null checks to prevent crashes when labels aren't initialized yet
        if (jobsReceived != lastJobs && minerJobsLabel) {
            lv_label_set_text_fmt(minerJobsLabel, "Jobs: %u", jobsReceived);
            lastJobs = jobsReceived;
        }
        if (sharesSubmitted != lastSubmitted && minerSubmittedLabel) {
            lv_label_set_text_fmt(minerSubmittedLabel, "Submitted: %u", sharesSubmitted);
            lastSubmitted = sharesSubmitted;
        }
        if (sharesAccepted != lastAccepted && minerAcceptedLabel) {
            lv_label_set_text_fmt(minerAcceptedLabel, "Accepted: %u", sharesAccepted);
            if (miningStatsScreen && miningStatsAcceptedLabel) {
                lv_label_set_text_fmt(miningStatsAcceptedLabel, "%u", sharesAccepted);
            }
            lastAccepted = sharesAccepted;
        }
        if (sharesRejected != lastRejected && minerRejectedLabel) {
            lv_label_set_text_fmt(minerRejectedLabel, "Rejected: %u", sharesRejected);
            if (miningStatsScreen && miningStatsRejectedLabel) {
                lv_label_set_text_fmt(miningStatsRejectedLabel, "%u", sharesRejected);
            }
            lastRejected = sharesRejected;
        }
        if (blocksFound != lastBlocks && minerBlocksLabel) {
            lv_label_set_text_fmt(minerBlocksLabel, "Blocks: %u", blocksFound);
            lv_obj_set_style_text_color(minerBlocksLabel,
                blocksFound > 0 ? lv_color_hex(0xFFD700) : lv_color_hex(0x888888), 0);
            if (miningStatsScreen && miningStatsBlocksLabel) {
                lv_label_set_text_fmt(miningStatsBlocksLabel, "%u", blocksFound);
            }
            lastBlocks = blocksFound;
        }
    }
    // updateMinerDisplay() is now FULLY ENABLED - all features restored
}

// Animation callback for counting price up/down
void price_anim_cb(void * var, int32_t value) {
  currentAnimatedPrice = value / 10000.0f;  // Scale back from integer (10000x for precision)
  char buf[32];
  snprintf(buf, sizeof(buf), "$%.4f", currentAnimatedPrice);
  lv_label_set_text(priceLabel, buf);
}

void fetch_data() {
  // Show loading spinner
  if (loadingSpinner) {
    lv_obj_clear_flag(loadingSpinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(priceLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(changeLabel, LV_OBJ_FLAG_HIDDEN);
  }

  if (WiFi.status() != WL_CONNECTED) {
    // Hide spinner on error
    if (loadingSpinner) {
      lv_obj_add_flag(loadingSpinner, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(priceLabel, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(changeLabel, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(priceLabel, "No WiFi");
    lv_label_set_text(changeLabel, "");
    hashrateAPIWorking = false;
    lv_label_set_text(apiStatusLabel, "! No Connection");
    lv_obj_set_style_text_color(apiStatusLabel, lv_color_hex(0xFF0000), 0);
    return;
  }

  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;

  // Fetch price from CoinGecko (including market cap)
  http.begin(secureClient, "https://api.coingecko.com/api/v3/simple/price?ids=kaspa&vs_currencies=usd&include_24hr_change=true&include_market_cap=true");
  http.setTimeout(15000);

  int httpCode = http.GET();

  if (httpCode == 200) {
    DynamicJsonDocument doc(2048);
    DeserializationError error = deserializeJson(doc, http.getString());

    if (!error) {
      float p = doc["kaspa"]["usd"];
      float c = doc["kaspa"]["usd_24h_change"];
      float mc = doc["kaspa"]["usd_market_cap"];

      price_usd = "$" + String(p, 4);
      change_24h = (c >= 0 ? "+" : "") + String(c, 2) + "%";

      // Format market cap in billions with 2 decimal places
      if (mc >= 1e9) {
        market_cap = "$" + String(mc / 1e9, 2) + "B";
      } else if (mc >= 1e6) {
        market_cap = "$" + String(mc / 1e6, 2) + "M";
      } else {
        market_cap = "$" + String(mc, 0);
      }

      Serial.printf("✓ Price: %s, Change: %s, Market Cap: %s\n", price_usd.c_str(), change_24h.c_str(), market_cap.c_str());

      // Store price in history with Unix timestamp
      priceHistory[priceIndex] = p;
      time_t now;
      time(&now);

      // Debug: Print current time info
      struct tm timeinfo;
      localtime_r(&now, &timeinfo);
      Serial.printf("DEBUG: Storing timestamp - Raw time_t: %ld\n", (long)now);
      Serial.printf("DEBUG: Local time: %04d-%02d-%02d %02d:%02d:%02d\n",
                    timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                    timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

      priceTimestamps[priceIndex] = (uint64_t)now * 1000ULL;  // Convert to milliseconds (64-bit to avoid overflow)
      priceIndex = (priceIndex + 1) % PRICE_HISTORY_POINTS;
      if (!priceHistoryInitialized && priceIndex == 0) {
        priceHistoryInitialized = true;
      }

      // Animate price change (count up/down effect)
      if (previousPrice > 0 && abs(p - previousPrice) > 0.0001) {
        // Only animate if we have a previous price and it changed
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, priceLabel);
        lv_anim_set_values(&a, (int32_t)(previousPrice * 10000), (int32_t)(p * 10000));
        lv_anim_set_time(&a, 800);  // 800ms animation
        lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)price_anim_cb);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
      } else {
        // First load or no change - just set the text directly
        lv_label_set_text(priceLabel, price_usd.c_str());
      }
      previousPrice = p;  // Store for next update

      lv_label_set_text(changeLabel, change_24h.c_str());

      // Set color based on positive/negative change
      if (c >= 0) {
        lv_obj_set_style_text_color(changeLabel, lv_color_hex(0x00FF00), 0);  // Green
      } else {
        lv_obj_set_style_text_color(changeLabel, lv_color_hex(0xFF0000), 0);  // Red
      }

      // Clear error indicator if it was showing
      hashrateAPIWorking = true;
      lv_label_set_text(apiStatusLabel, "");

      // Hide spinner and show labels on success
      if (loadingSpinner) {
        lv_obj_add_flag(loadingSpinner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(priceLabel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(changeLabel, LV_OBJ_FLAG_HIDDEN);
      }
    } else {
      // Silenced: Serial.printf("❌ JSON parse error: %s\n", error.c_str());
      lv_label_set_text(priceLabel, "Parse Error");
      lv_label_set_text(apiStatusLabel, "! Data Error");
      lv_obj_set_style_text_color(apiStatusLabel, lv_color_hex(0xFF9900), 0);
      hashrateAPIWorking = false;

      // Hide spinner and show labels on error
      if (loadingSpinner) {
        lv_obj_add_flag(loadingSpinner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(priceLabel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(changeLabel, LV_OBJ_FLAG_HIDDEN);
      }
    }
  } else {
    Serial.printf("❌ API Error: HTTP %d\n", httpCode);
    lv_label_set_text(priceLabel, "API Error");
    lv_label_set_text(changeLabel, "Tap to retry");
    lv_obj_set_style_text_color(changeLabel, lv_color_hex(0x49EACB), 0);
    lv_label_set_text(apiStatusLabel, "! API Failed");
    lv_obj_set_style_text_color(apiStatusLabel, lv_color_hex(0xFF9900), 0);
    hashrateAPIWorking = false;

    // Hide spinner and show labels on HTTP error
    if (loadingSpinner) {
      lv_obj_add_flag(loadingSpinner, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(priceLabel, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(changeLabel, LV_OBJ_FLAG_HIDDEN);
    }
  }
  http.end();
  secureClient.stop();  // Explicitly close SSL connection
}

void fetchHistoricalPriceData() {
  Serial.println(">>> Fetching 24h historical price data from CoinGecko...");

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ No WiFi connection - skipping historical data fetch");
    return;
  }

  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;

  // Fetch 24 hours of price data (returns ~288 5-minute intervals)
  http.begin(secureClient, "https://api.coingecko.com/api/v3/coins/kaspa/market_chart?vs_currency=usd&days=1");
  http.setTimeout(20000);  // Longer timeout for historical data

  int httpCode = http.GET();

  if (httpCode == 200) {
    DynamicJsonDocument doc(16384);  // Larger buffer for historical data (~288 points)
    DeserializationError error = deserializeJson(doc, http.getString());

    if (!error) {
      JsonArray prices = doc["prices"];
      int totalPoints = prices.size();
      Serial.printf("✓ Received %d historical price points from CoinGecko\n", totalPoints);

      if (totalPoints > 0) {
        // Downsample: We want 48 points from ~288 points
        // Take every 6th point (288 / 6 = 48, roughly 30-minute intervals)
        int step = totalPoints / PRICE_HISTORY_POINTS;
        if (step < 1) step = 1;

        int pointsStored = 0;
        for (int i = 0; i < totalPoints && pointsStored < PRICE_HISTORY_POINTS; i += step) {
          JsonArray pricePoint = prices[i];
          uint64_t timestamp_ms = pricePoint[0];  // Unix timestamp in milliseconds
          float price = pricePoint[1];

          // Store in circular buffer
          priceHistory[pointsStored] = price;
          priceTimestamps[pointsStored] = timestamp_ms;
          pointsStored++;
        }

        // Update index to point after last stored point
        priceIndex = pointsStored % PRICE_HISTORY_POINTS;
        priceHistoryInitialized = true;

        Serial.printf("✓ Stored %d historical price points (downsampled from %d)\n", pointsStored, totalPoints);
        Serial.printf("  Oldest: $%.4f, Newest: $%.4f\n",
                     priceHistory[0],
                     priceHistory[pointsStored - 1]);
      }
    } else {
      Serial.printf("❌ JSON parse error: %s\n", error.c_str());
    }
  } else {
    Serial.printf("❌ Historical data API error: HTTP %d\n", httpCode);
  }

  http.end();
  secureClient.stop();
}

void create_ui() {
  Serial.println(">>> create_ui() called - building main dashboard...");
  lv_obj_clean(lv_scr_act());
  
  // Dark gradient background for visual depth
  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_grad_color(lv_scr_act(), lv_color_hex(0x0a0a0a), 0);
  lv_obj_set_style_bg_grad_dir(lv_scr_act(), LV_GRAD_DIR_VER, 0);
  
  // Kaspa green accent color
  lv_color_t kaspa_green = lv_color_hex(0x49EACB);
  lv_color_t text_white = lv_color_hex(0xFFFFFF);
  lv_color_t text_dim = lv_color_hex(0x888888);

  // ========== TOP BAR ==========
  
  // KASDeck Logo (centered top) - New horizontal layout
  lv_obj_t *logo = lv_img_create(lv_scr_act());
  if (logos_loaded) {
    lv_img_set_src(logo, &kaspa_logo_sd);
  }
  lv_obj_set_size(logo, 400, 126);  // New horizontal logo size
  lv_obj_align(logo, LV_ALIGN_TOP_MID, 0, 5);  // Small margin from top

  // Menu button (top left - minimal) - matching style with settings
  lv_obj_t *menuBtn = lv_btn_create(lv_scr_act());
  lv_obj_set_size(menuBtn, 100, 35);
  lv_obj_align(menuBtn, LV_ALIGN_TOP_LEFT, 15, 60);
  lv_obj_set_style_radius(menuBtn, 8, 0);  // Rounded corners for buttons
  lv_obj_set_style_bg_color(menuBtn, lv_color_hex(0x000000), 0);  // Transparent bg
  lv_obj_set_style_border_width(menuBtn, 2, 0);
  lv_obj_set_style_border_color(menuBtn, kaspa_green, 0);
  lv_obj_set_style_shadow_width(menuBtn, 0, 0);
  // Button press effect - invert colors and scale down
  lv_obj_set_style_bg_color(menuBtn, kaspa_green, LV_STATE_PRESSED);
  lv_obj_add_event_cb(menuBtn, menu_btn_event_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *menuLabel = lv_label_create(menuBtn);
  lv_label_set_text(menuLabel, "MENU");
  lv_obj_set_style_text_font(menuLabel, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(menuLabel, lv_color_hex(0xFFFFFF), 0);  // White text
  lv_obj_set_style_text_color(menuLabel, lv_color_hex(0x000000), LV_STATE_PRESSED);  // Black text on press
  lv_obj_center(menuLabel);

  // Settings button (top right - minimal) - moved down to avoid overlap
  lv_obj_t *settingsBtn = lv_btn_create(lv_scr_act());
  lv_obj_set_size(settingsBtn, 100, 35);
  lv_obj_align(settingsBtn, LV_ALIGN_TOP_RIGHT, -15, 60);  // Moved from Y=15 to Y=60
  lv_obj_set_style_radius(settingsBtn, 8, 0);  // Rounded corners for buttons
  lv_obj_set_style_bg_color(settingsBtn, lv_color_hex(0x000000), 0);  // Transparent bg
  lv_obj_set_style_border_width(settingsBtn, 2, 0);
  lv_obj_set_style_border_color(settingsBtn, kaspa_green, 0);
  lv_obj_set_style_shadow_width(settingsBtn, 0, 0);
  // Button press effect - invert colors and scale down
  lv_obj_set_style_bg_color(settingsBtn, kaspa_green, LV_STATE_PRESSED);
  lv_obj_add_event_cb(settingsBtn, settings_btn_event_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *settingsLabel = lv_label_create(settingsBtn);
  lv_label_set_text(settingsLabel, "SETTINGS");
  lv_obj_set_style_text_font(settingsLabel, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(settingsLabel, lv_color_hex(0xFFFFFF), 0);  // White text
  lv_obj_set_style_text_color(settingsLabel, lv_color_hex(0x000000), LV_STATE_PRESSED);  // Black text on press
  lv_obj_center(settingsLabel);

  // ========== LEFT COLUMN ==========
  
  // Mining Stats Panel (top left)
  lv_obj_t *minerStatsCard = lv_obj_create(lv_scr_act());
  lv_obj_set_size(minerStatsCard, 360, 100);
  lv_obj_set_pos(minerStatsCard, 15, 140);  // After 126px logo + 5px margin + 9px gap
  lv_obj_set_style_radius(minerStatsCard, 10, 0);  // Rounded corners for modern look
  lv_obj_set_style_bg_color(minerStatsCard, lv_color_hex(0x000000), 0);  // Pure black
  lv_obj_set_style_bg_opa(minerStatsCard, LV_OPA_100, 0);
  lv_obj_set_style_border_width(minerStatsCard, 2, 0);
  lv_obj_set_style_border_color(minerStatsCard, lv_color_hex(COLOR_SLATE_GRAY), 0);  // Neutral border for passive display
  lv_obj_set_style_pad_all(minerStatsCard, 0, 0);  // No padding, we'll handle it per section
  lv_obj_set_style_shadow_width(minerStatsCard, 8, 0);  // Smaller shadow to reduce memory
  lv_obj_set_style_shadow_opa(minerStatsCard, LV_OPA_20, 0);
  lv_obj_set_style_shadow_ofs_y(minerStatsCard, 3, 0);
  lv_obj_clear_flag(minerStatsCard, LV_OBJ_FLAG_SCROLLABLE);  // Disable scrolling

  // Status label at top (spans full width)
  minerStatusLabel = lv_label_create(minerStatsCard);
  lv_label_set_text(minerStatusLabel, "NOT CONFIGURED");
  lv_obj_set_style_text_font(minerStatusLabel, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(minerStatusLabel, lv_color_hex(COLOR_TEXT_SECONDARY), 0);
  lv_obj_align(minerStatusLabel, LV_ALIGN_TOP_MID, 0, 5);

  // Centered hashrate section (full width)
  lv_obj_t *hashrateSection = lv_obj_create(minerStatsCard);
  lv_obj_set_size(hashrateSection, 360, 75);  // Full width
  lv_obj_set_pos(hashrateSection, 0, 20);
  lv_obj_set_style_bg_opa(hashrateSection, LV_OPA_TRANSP, 0);  // Transparent
  lv_obj_set_style_border_width(hashrateSection, 0, 0);
  lv_obj_set_style_pad_all(hashrateSection, 0, 0);
  lv_obj_clear_flag(hashrateSection, LV_OBJ_FLAG_SCROLLABLE);  // Disable scrolling

  // "HASHRATE" label - Kaspa Green
  lv_obj_t *hashrateTitle = lv_label_create(hashrateSection);
  lv_label_set_text(hashrateTitle, "HASHRATE");
  lv_obj_set_style_text_font(hashrateTitle, &lv_font_montserrat_10, 0);
  lv_obj_set_style_text_color(hashrateTitle, lv_color_hex(COLOR_KASPA_GREEN), 0);
  lv_obj_align(hashrateTitle, LV_ALIGN_TOP_MID, 0, 0);

  minerHashrateLabel = lv_label_create(hashrateSection);
  lv_label_set_text(minerHashrateLabel, "--");
  lv_obj_set_style_text_font(minerHashrateLabel, &lv_font_montserrat_40, 0);  // Larger since it's centered
  lv_obj_set_style_text_color(minerHashrateLabel, lv_color_hex(COLOR_TEXT_PRIMARY), 0);
  lv_obj_align(minerHashrateLabel, LV_ALIGN_CENTER, 0, 0);

  // Progress bar for hashrate visualization
  lv_obj_t *hashrateBar = lv_bar_create(hashrateSection);
  lv_obj_set_size(hashrateBar, 340, 6);  // Almost full width
  lv_obj_align(hashrateBar, LV_ALIGN_BOTTOM_MID, 0, -2);
  lv_obj_set_style_bg_color(hashrateBar, lv_color_hex(COLOR_SLATE_GRAY), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(hashrateBar, LV_OPA_30, LV_PART_MAIN);
  lv_obj_set_style_bg_color(hashrateBar, lv_color_hex(COLOR_KASPA_GREEN), LV_PART_INDICATOR);
  lv_obj_set_style_radius(hashrateBar, 3, 0);
  lv_obj_set_style_pad_all(hashrateBar, 0, 0);  // Remove padding
  lv_bar_set_value(hashrateBar, 0, LV_ANIM_OFF);

  // Remove old minerSharesLabel (no longer used)

  // Price Panel (make clickable for retry)
  lv_obj_t *priceCard = lv_obj_create(lv_scr_act());
  lv_obj_set_size(priceCard, 360, 115);  // Shorter to fit
  lv_obj_set_pos(priceCard, 15, 245);  // After miner stats (15px earlier)
  lv_obj_set_style_radius(priceCard, 10, 0);  // Rounded corners
  lv_obj_set_style_bg_color(priceCard, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(priceCard, LV_OPA_100, 0);
  lv_obj_set_style_border_width(priceCard, 2, 0);
  lv_obj_set_style_border_color(priceCard, lv_color_hex(COLOR_SLATE_GRAY), 0);  // Neutral border for passive display
  lv_obj_set_style_pad_all(priceCard, 10, 0);
  lv_obj_set_style_shadow_width(priceCard, 8, 0);
  lv_obj_set_style_shadow_opa(priceCard, LV_OPA_20, 0);
  lv_obj_set_style_shadow_ofs_y(priceCard, 3, 0);
  // Make clickable for retry
  lv_obj_add_flag(priceCard, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(priceCard, [](lv_event_t *e) {
    if (e->code == LV_EVENT_CLICKED) {
      Serial.println("Price card clicked - refreshing historical & current data...");
      playBeep(800, 50);
      lv_label_set_text(priceLabel, "Fetching...");
      lv_label_set_text(changeLabel, "Please wait");
      lv_obj_set_style_text_color(changeLabel, lv_color_hex(0x49EACB), 0);
      priceRefreshRequested = true;  // Set flag for historical refresh
      #if ENABLE_PRICE_HISTORY
      fetchHistoricalPriceData();     // Re-fetch 24h historical data
      #endif
      fetch_data();                   // Fetch current price
      priceRefreshRequested = false;  // Clear flag
    }
  }, LV_EVENT_CLICKED, NULL);

  lv_obj_t *priceTitle = lv_label_create(priceCard);
  lv_label_set_text(priceTitle, "KASPA PRICE");
  lv_obj_set_style_text_font(priceTitle, &lv_font_montserrat_16, 0);  // Increased from 10 to 16 to match Network Hashrate
  lv_obj_set_style_text_color(priceTitle, lv_color_hex(COLOR_KASPA_GREEN), 0);
  lv_obj_align(priceTitle, LV_ALIGN_TOP_MID, 0, 5);

  priceLabel = lv_label_create(priceCard);
  lv_label_set_text(priceLabel, price_usd.c_str());
  lv_obj_set_style_text_font(priceLabel, &lv_font_montserrat_40, 0);
  lv_obj_set_style_text_color(priceLabel, lv_color_hex(COLOR_TEXT_PRIMARY), 0);
  lv_obj_align(priceLabel, LV_ALIGN_CENTER, 0, 5);

  changeLabel = lv_label_create(priceCard);
  lv_label_set_text(changeLabel, change_24h.c_str());
  lv_obj_set_style_text_font(changeLabel, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(changeLabel, lv_color_hex(COLOR_TEXT_SECONDARY), 0);
  lv_obj_align(changeLabel, LV_ALIGN_BOTTOM_MID, 0, -3);  // Moved down from -8 to -3 (5px more space below price)

  // Loading indicator (static text since fetch blocks LVGL loop)
  loadingSpinner = lv_label_create(priceCard);
  lv_label_set_text(loadingSpinner, "Loading...");
  lv_obj_set_style_text_font(loadingSpinner, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(loadingSpinner, kaspa_green, 0);
  lv_obj_align(loadingSpinner, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_flag(loadingSpinner, LV_OBJ_FLAG_HIDDEN);  // Start hidden

  // Network Hashrate Panel
  lv_obj_t *hashrateCard = lv_obj_create(lv_scr_act());
  lv_obj_set_size(hashrateCard, 360, 100);  // Adjust to fit
  lv_obj_set_pos(hashrateCard, 15, 365);  // After price (15px earlier)
  lv_obj_set_style_radius(hashrateCard, 10, 0);  // Rounded corners
  lv_obj_set_style_bg_color(hashrateCard, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(hashrateCard, LV_OPA_100, 0);
  lv_obj_set_style_border_width(hashrateCard, 2, 0);
  lv_obj_set_style_border_color(hashrateCard, lv_color_hex(COLOR_SLATE_GRAY), 0);  // Neutral border for passive display
  lv_obj_set_style_pad_all(hashrateCard, 10, 0);
  lv_obj_set_style_shadow_width(hashrateCard, 8, 0);
  lv_obj_set_style_shadow_opa(hashrateCard, LV_OPA_20, 0);
  lv_obj_set_style_shadow_ofs_y(hashrateCard, 3, 0);
  lv_obj_clear_flag(hashrateCard, LV_OBJ_FLAG_SCROLLABLE);  // Disable scrolling (removes scrollbar)
  // Make clickable for retry
  lv_obj_add_flag(hashrateCard, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(hashrateCard, [](lv_event_t *e) {
    if (e->code == LV_EVENT_CLICKED) {
      Serial.println("Hashrate card clicked - retrying data update...");
      playBeep(800, 50);
      lv_label_set_text(bigHashrateLabel, "Fetching...");
      #if ENABLE_NETWORK_HASHRATE_CHART
      updateHashrateChart();  // Fixed: correct function name
      #else
      fetchNetworkHashrate();  // Just update the number without chart
      if (bigHashrateLabel) {
        lv_label_set_text_fmt(bigHashrateLabel, "%d PH/s", (int)networkHashrate);
      }
      #endif
    }
  }, LV_EVENT_CLICKED, NULL);

  // Network hashrate number (no title label - "Mining Active/Paused" label above provides context)
  bigHashrateLabel = lv_label_create(hashrateCard);
  lv_label_set_text(bigHashrateLabel, "--- PH/s");
  lv_obj_set_style_text_font(bigHashrateLabel, &lv_font_montserrat_40, 0);
  lv_obj_set_style_text_color(bigHashrateLabel, lv_color_hex(COLOR_TEXT_PRIMARY), 0);
  lv_obj_align(bigHashrateLabel, LV_ALIGN_CENTER, 0, 0);  // Centered vertically now

  // ========== RIGHT SIDE - CHART ==========
  
  lv_obj_t *chartCard = lv_obj_create(lv_scr_act());
  lv_obj_set_size(chartCard, 395, 325);  // Fits remaining space
  lv_obj_set_pos(chartCard, 390, 140);  // Start after logo (15px earlier)
  lv_obj_set_style_radius(chartCard, 10, 0);  // Rounded corners
  lv_obj_set_style_bg_color(chartCard, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(chartCard, LV_OPA_100, 0);
  lv_obj_set_style_border_width(chartCard, 2, 0);
  lv_obj_set_style_border_color(chartCard, kaspa_green, 0);
  lv_obj_set_style_pad_all(chartCard, 12, 0);
  lv_obj_set_style_shadow_width(chartCard, 8, 0);
  lv_obj_set_style_shadow_opa(chartCard, LV_OPA_20, 0);
  lv_obj_set_style_shadow_ofs_y(chartCard, 3, 0);

  lv_obj_t *chartTitle = lv_label_create(chartCard);
  lv_label_set_text(chartTitle, "NETWORK HASHRATE GRAPH [12H]");
  lv_obj_set_style_text_font(chartTitle, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(chartTitle, lv_color_hex(0xFFFFFF), 0);  // Changed to white
  lv_obj_align(chartTitle, LV_ALIGN_TOP_LEFT, 5, 5);

  // Create chart
  hashrateChart = lv_chart_create(chartCard);
  lv_obj_set_size(hashrateChart, 350, 220);  // Fits in card
  lv_obj_align(hashrateChart, LV_ALIGN_TOP_LEFT, 10, 30);
  lv_chart_set_type(hashrateChart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(hashrateChart, HASHRATE_POINTS);
  lv_chart_set_range(hashrateChart, LV_CHART_AXIS_PRIMARY_Y, 400, 650);
  lv_chart_set_update_mode(hashrateChart, LV_CHART_UPDATE_MODE_SHIFT);

  // Pure black chart background
  lv_obj_set_style_bg_color(hashrateChart, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(hashrateChart, LV_OPA_100, 0);
  lv_obj_set_style_border_width(hashrateChart, 1, 0);
  lv_obj_set_style_border_color(hashrateChart, kaspa_green, 0);
  lv_obj_set_style_border_opa(hashrateChart, LV_OPA_30, 0);

  // Single horizontal grid line at middle for reference
  lv_chart_set_div_line_count(hashrateChart, 2, 0);  // 2 divisions = 1 middle line, no vertical lines
  lv_obj_set_style_line_color(hashrateChart, lv_color_hex(0x888888), LV_PART_MAIN);  // Grey
  lv_obj_set_style_line_opa(hashrateChart, LV_OPA_30, LV_PART_MAIN);  // Subtle

  // Add series
  hashrateSeries = lv_chart_add_series(hashrateChart, lv_color_hex(COLOR_DEEP_PURPLE), LV_CHART_AXIS_PRIMARY_Y);

  // THIS IS THE KEY - Set the draw type to fill area
  lv_obj_set_style_line_width(hashrateChart, 3, LV_PART_ITEMS);

  // Enable area fill under the line
  lv_obj_set_style_size(hashrateChart, 0, LV_PART_ITEMS);  // Remove point circles
  lv_obj_set_style_bg_opa(hashrateChart, LV_OPA_60, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(hashrateChart, lv_color_hex(COLOR_DEEP_PURPLE), LV_PART_ITEMS);
  lv_obj_set_style_bg_grad_color(hashrateChart, lv_color_hex(0x0a0a0f), LV_PART_ITEMS);
  lv_obj_set_style_bg_grad_dir(hashrateChart, LV_GRAD_DIR_VER, LV_PART_ITEMS);

  // CRITICAL: Enable the area drawing
  lv_chart_set_series_color(hashrateChart, hashrateSeries, lv_color_hex(COLOR_DEEP_PURPLE));

  // Initialize with zeros
  for (int i = 0; i < HASHRATE_POINTS; i++) {
    hashrateHistory[i] = 0;
    lv_chart_set_next_value(hashrateChart, hashrateSeries, 0);
  }

  // Y-axis labels (right side) - will be updated dynamically
  yAxisMaxLabel = lv_label_create(chartCard);
  lv_label_set_text(yAxisMaxLabel, "650");
  lv_obj_set_style_text_font(yAxisMaxLabel, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(yAxisMaxLabel, lv_color_hex(0xFFFFFF), 0);  // White
  lv_obj_align(yAxisMaxLabel, LV_ALIGN_TOP_RIGHT, -5, 28);

  yAxisMidLabel = lv_label_create(chartCard);
  lv_label_set_text(yAxisMidLabel, "525");
  lv_obj_set_style_text_font(yAxisMidLabel, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(yAxisMidLabel, lv_color_hex(0xFFFFFF), 0);  // White
  lv_obj_align(yAxisMidLabel, LV_ALIGN_TOP_RIGHT, -5, 138);  // Middle of 220px

  yAxisMinLabel = lv_label_create(chartCard);
  lv_label_set_text(yAxisMinLabel, "400");
  lv_obj_set_style_text_font(yAxisMinLabel, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(yAxisMinLabel, lv_color_hex(0xFFFFFF), 0);  // White
  lv_obj_align(yAxisMinLabel, LV_ALIGN_TOP_RIGHT, -5, 248);  // Bottom

  // X-axis labels (bottom) - moved "12h ago" below graph, removed "now" to prevent overlap
  xAxisStartLabel = lv_label_create(chartCard);
  lv_label_set_text(xAxisStartLabel, "12h ago");
  lv_obj_set_style_text_font(xAxisStartLabel, &lv_font_montserrat_10, 0);
  lv_obj_set_style_text_color(xAxisStartLabel, lv_color_hex(0x888888), 0);
  lv_obj_align(xAxisStartLabel, LV_ALIGN_BOTTOM_LEFT, 5, -35);  // Moved from -50 to -35 (closer to bottom)

  xAxisEndLabel = lv_label_create(chartCard);
  lv_label_set_text(xAxisEndLabel, "");  // Removed "Now" label to prevent overlap
  lv_obj_set_style_text_font(xAxisEndLabel, &lv_font_montserrat_10, 0);
  lv_obj_set_style_text_color(xAxisEndLabel, lv_color_hex(0x888888), 0);
  lv_obj_add_flag(xAxisEndLabel, LV_OBJ_FLAG_HIDDEN);  // Hide the label completely

  // Stats labels below chart
  avgHashLabel = lv_label_create(chartCard);
  lv_label_set_text(avgHashLabel, "Avg: -- PH/s");
  lv_obj_set_style_text_font(avgHashLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(avgHashLabel, lv_color_hex(0xCCCCCC), 0);
  lv_obj_align(avgHashLabel, LV_ALIGN_BOTTOM_LEFT, 5, -10);  // Moved down from -25 to -10

  maxHashLabel = lv_label_create(chartCard);
  lv_label_set_text(maxHashLabel, "Peak: -- PH/s");
  lv_obj_set_style_text_font(maxHashLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(maxHashLabel, lv_color_hex(0xCCCCCC), 0);
  lv_obj_align(maxHashLabel, LV_ALIGN_BOTTOM_RIGHT, -5, -10);  // Moved down from -25 to -10

  // API Status indicator with visual dot
  lv_obj_t *statusDot = lv_obj_create(chartCard);
  lv_obj_set_size(statusDot, 8, 8);
  lv_obj_set_style_radius(statusDot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(statusDot, lv_color_hex(COLOR_WARNING_YELLOW), 0);
  lv_obj_set_style_border_width(statusDot, 0, 0);
  lv_obj_align(statusDot, LV_ALIGN_BOTTOM_MID, -40, -8);

  apiStatusLabel = lv_label_create(chartCard);
  lv_label_set_text(apiStatusLabel, "Connecting...");
  lv_obj_set_style_text_font(apiStatusLabel, &lv_font_montserrat_10, 0);
  lv_obj_set_style_text_color(apiStatusLabel, lv_color_hex(COLOR_TEXT_SECONDARY), 0);
  lv_obj_align(apiStatusLabel, LV_ALIGN_BOTTOM_MID, -5, -5);
}

void show_miner_config_menu() {
  if (minerConfigMenu) return;
  
  lv_color_t kaspa_green = lv_color_hex(0x49EACB);
  
  // Fullscreen overlay
  minerConfigMenu = lv_obj_create(lv_scr_act());
  lv_obj_set_size(minerConfigMenu, 800, 480);
  lv_obj_set_pos(minerConfigMenu, 0, 0);
  lv_obj_set_style_bg_color(minerConfigMenu, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(minerConfigMenu, LV_OPA_90, 0);
  lv_obj_clear_flag(minerConfigMenu, LV_OBJ_FLAG_SCROLLABLE);
  
  // Main panel
  lv_obj_t *panel = lv_obj_create(minerConfigMenu);
  lv_obj_set_size(panel, 550, 460);
  lv_obj_align(panel, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_radius(panel, 0, 0);
  lv_obj_set_style_bg_color(panel, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(panel, LV_OPA_100, 0);
  lv_obj_set_style_border_width(panel, 2, 0);
  lv_obj_set_style_border_color(panel, kaspa_green, 0);
  lv_obj_set_style_shadow_width(panel, 0, 0);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
  
  // Title
  lv_obj_t *title = lv_label_create(panel);
  lv_label_set_text(title, "LOTTERY MINER CONFIG");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(title, kaspa_green, 0);
  lv_obj_set_pos(title, 20, 15);

  // Web Configuration header
  lv_obj_t *webText = lv_label_create(panel);
  lv_label_set_text(webText, "Configure wallet and pool via web:");
  lv_obj_set_style_text_font(webText, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(webText, lv_color_hex(0xFFFFFF), 0);  // White
  lv_obj_set_pos(webText, 40, 55);

  // Generate mDNS name
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  String mdnsName = "kaspa-" + mac.substring(8);
  String ipAddress = WiFi.localIP().toString();

  // Primary URL (mDNS)
  lv_obj_t *primaryUrlLabel = lv_label_create(panel);
  String primaryUrlText = "http://" + mdnsName + ".local";
  lv_label_set_text(primaryUrlLabel, primaryUrlText.c_str());
  lv_obj_set_style_text_color(primaryUrlLabel, kaspa_green, 0);
  lv_obj_set_style_text_font(primaryUrlLabel, &lv_font_montserrat_16, 0);
  lv_obj_set_pos(primaryUrlLabel, 40, 80);

  // Alternative URL label
  lv_obj_t *altText = lv_label_create(panel);
  lv_label_set_text(altText, "Alternative (IP address):");
  lv_obj_set_style_text_font(altText, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(altText, lv_color_hex(0x888888), 0);  // Gray
  lv_obj_set_pos(altText, 40, 110);

  // Alternative URL (IP)
  lv_obj_t *altUrlLabel = lv_label_create(panel);
  String altUrlText = "http://" + ipAddress;
  lv_label_set_text(altUrlLabel, altUrlText.c_str());
  lv_obj_set_style_text_color(altUrlLabel, lv_color_hex(0x888888), 0);  // Gray
  lv_obj_set_style_text_font(altUrlLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_pos(altUrlLabel, 40, 132);

  // Status section (all aligned)
  int statusX = 40;
  int statusLabelWidth = 180;
  int statusValueX = 240;
  int statusY = 165;
  int statusSpacing = 30;

  // WiFi Status (FIRST)
  lv_obj_t *wifiLabel = lv_label_create(panel);
  lv_label_set_text(wifiLabel, "WiFi Status:");
  lv_obj_set_style_text_font(wifiLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(wifiLabel, kaspa_green, 0);
  lv_obj_set_pos(wifiLabel, statusX, statusY);

  statusLabel = lv_label_create(panel);
  lv_label_set_text(statusLabel, WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
  lv_obj_set_style_text_color(statusLabel,
    WiFi.status() == WL_CONNECTED ? lv_color_hex(0x88FF88) : lv_color_hex(0xFF8888), 0);
  lv_obj_set_style_text_font(statusLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_pos(statusLabel, statusValueX, statusY);

  // Web Portal Status (SECOND)
  lv_obj_t *webPortalLabel = lv_label_create(panel);
  lv_label_set_text(webPortalLabel, "Web Portal Status:");
  lv_obj_set_style_text_font(webPortalLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(webPortalLabel, kaspa_green, 0);
  lv_obj_set_pos(webPortalLabel, statusX, statusY + statusSpacing);

  lv_obj_t *webStatusLabel = lv_label_create(panel);
  lv_label_set_text(webStatusLabel, "Running");
  lv_obj_set_style_text_color(webStatusLabel, lv_color_hex(0x88FF88), 0);  // Green
  lv_obj_set_style_text_font(webStatusLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_pos(webStatusLabel, statusValueX, statusY + statusSpacing);

  // Pool Connection Status (THIRD - NEW)
  lv_obj_t *poolLabel = lv_label_create(panel);
  lv_label_set_text(poolLabel, "Pool Connection:");
  lv_obj_set_style_text_font(poolLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(poolLabel, kaspa_green, 0);
  lv_obj_set_pos(poolLabel, statusX, statusY + statusSpacing * 2);

  poolStatusLabel = lv_label_create(panel);
  // Determine pool connection status (same logic as Web UI)
  const char* poolStatusText = "Disconnected";
  lv_color_t poolStatusColor = lv_color_hex(0xFF8888);  // Red
  if (stratumClient.connected()) {
    poolStatusText = "Connected";
    poolStatusColor = lv_color_hex(0x88FF88);  // Green
  }
  lv_label_set_text(poolStatusLabel, poolStatusText);
  lv_obj_set_style_text_color(poolStatusLabel, poolStatusColor, 0);
  lv_obj_set_style_text_font(poolStatusLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_pos(poolStatusLabel, statusValueX, statusY + statusSpacing * 2);

  // Disclaimer (above toggle) - adjusted position to fill space
  lv_obj_t *disclaimer = lv_label_create(panel);
  lv_label_set_text(disclaimer, "WARNING: Mining will reduce GUI responsiveness");
  lv_obj_set_style_text_font(disclaimer, &lv_font_montserrat_18, 0);  // Increased from 14 to 18 for visibility
  lv_obj_set_style_text_color(disclaimer, lv_color_hex(0xFF9900), 0);  // Orange
  lv_obj_set_pos(disclaimer, 40, 275);  // Moved up 15px from 290 to fill space

  // Mining Toggle Section - moved down to accommodate new pool status
  lv_obj_t *miningLabel = lv_label_create(panel);
  lv_label_set_text(miningLabel, "Enable Mining:");
  lv_obj_set_style_text_font(miningLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(miningLabel, kaspa_green, 0);
  lv_obj_set_pos(miningLabel, statusX, 320);  // Moved from 290 to 320 (+30px)

  miningToggle = lv_switch_create(panel);
  lv_obj_set_size(miningToggle, 70, 35);
  lv_obj_set_pos(miningToggle, statusValueX, 313);  // Moved from 283 to 313 (+30px)
  lv_obj_set_style_bg_color(miningToggle, lv_color_hex(0x333333), 0);
  lv_obj_set_style_bg_color(miningToggle, kaspa_green, LV_PART_INDICATOR | LV_STATE_CHECKED);
  if (miningEnabled) {
    lv_obj_add_state(miningToggle, LV_STATE_CHECKED);
  }
  lv_obj_add_event_cb(miningToggle, mining_toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);
  
  // Close Button (settings are auto-saved)
  lv_obj_t *backBtn = lv_btn_create(panel);
  lv_obj_set_size(backBtn, 300, 50);
  lv_obj_align(backBtn, LV_ALIGN_TOP_MID, 0, 360);  // Below mining toggle
  lv_obj_set_style_radius(backBtn, 8, 0);
  lv_obj_set_style_bg_color(backBtn, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(backBtn, 2, 0);
  lv_obj_set_style_border_color(backBtn, kaspa_green, 0);
  lv_obj_set_style_shadow_width(backBtn, 0, 0);
  // Button press effect
  lv_obj_set_style_bg_color(backBtn, kaspa_green, LV_STATE_PRESSED);
  lv_obj_add_event_cb(backBtn, close_miner_config_cb, LV_EVENT_CLICKED, NULL);
  
  lv_obj_t *backLabel = lv_label_create(backBtn);
  lv_label_set_text(backLabel, "Close");
  lv_obj_set_style_text_font(backLabel, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(backLabel, lv_color_hex(0xFFFFFF), 0);  // White
  lv_obj_set_style_text_color(backLabel, lv_color_hex(0x000000), LV_STATE_PRESSED);
  lv_obj_center(backLabel);
}

void wallet_textarea_clicked_cb(lv_event_t *e) {
  if (e->code == LV_EVENT_CLICKED) {
    // If keyboard already exists, delete it first
    if (currentKeyboard) {
      lv_obj_del(currentKeyboard);
      currentKeyboard = nullptr;
    }

    // Create on-screen keyboard with built-in layout
    currentKeyboard = lv_keyboard_create(lv_scr_act());
    lv_keyboard_set_textarea(currentKeyboard, walletTextarea);
    lv_keyboard_set_mode(currentKeyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_set_size(currentKeyboard, 800, 240);
    lv_obj_align(currentKeyboard, LV_ALIGN_BOTTOM_MID, 0, 0);

    // Listen for READY (checkmark), CANCEL (hide), and VALUE_CHANGED
    // The checkmark zone fix in keyboard_event_cb handles spurious characters
    lv_obj_add_event_cb(currentKeyboard, keyboard_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(currentKeyboard, keyboard_event_cb, LV_EVENT_CANCEL, NULL);
    lv_obj_add_event_cb(currentKeyboard, keyboard_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
  }
}

// Global to track last valid text length
static uint32_t lastTextareaLen = 0;
// Track the text when keyboard closes - we'll restore this if a spurious char appears
static String textWhenKeyboardClosed = "";
// keyboardCloseTime is now declared globally near currentKeyboard for touch blocking
// Done button shown above keyboard for easier closing
static lv_obj_t *keyboardDoneBtn = nullptr;
// Track last touch coordinates (updated by my_touch_read) for checkmark zone detection
int lastTouchX = 0;
int lastTouchY = 0;

// Check if coordinates are in a keyboard control button zone
// Built-in keyboard has: hide keyboard (bottom-left), checkmark (bottom-right)
// Keyboard is 800x240, aligned to bottom (Y=240-480)
// Bottom row control buttons are roughly at X<80 (hide) or X>720 (checkmark), Y>420
bool isInCheckmarkZone(int x, int y) {
  return (y > 420 && (x < 100 || x > 700));
}

void keyboard_event_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *kb = lv_event_get_target(e);
  lv_obj_t *ta = lv_keyboard_get_textarea(kb);

  // When OK button (checkmark) or hide keyboard button is pressed
  if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
    Serial.printf("=== %s ===\n", code == LV_EVENT_READY ? "READY (Checkmark)" : "CANCEL (Hide Keyboard)");

    if (ta) {
      const char *currentTxt = lv_textarea_get_text(ta);
      Serial.printf("  Final text: '%s'\n", currentTxt);
      textWhenKeyboardClosed = String(currentTxt);
      lastTextareaLen = strlen(currentTxt);
    }

    // Delete the Done button if it exists
    if (keyboardDoneBtn) {
      lv_obj_del(keyboardDoneBtn);
      keyboardDoneBtn = nullptr;
    }

    if (kb && currentKeyboard == kb) {
      lv_obj_del(kb);
      currentKeyboard = nullptr;
      keyboardCloseTime = millis();
      Serial.println("  Keyboard closed\n");
    }
  }
  else if (code == LV_EVENT_VALUE_CHANGED) {
    if (ta) {
      const char *txt = lv_textarea_get_text(ta);
      uint32_t currentLen = strlen(txt);

      // Detect if a character was ADDED (not deleted)
      if (currentLen > lastTextareaLen && currentLen > 0) {
        char newChar = txt[currentLen - 1];

        Serial.printf("VALUE_CHANGED: '%s' (len %d) +%c touch@(%d,%d)\n",
                      txt, currentLen, newChar, lastTouchX, lastTouchY);

        // CHECKMARK ZONE FIX: If the touch coordinates are in the control button zone
        // (checkmark or hide-keyboard) but a character was added, this is the LVGL
        // keyboard bug where it sends the last pressed key before processing the
        // control button. Remove the spurious character.
        if (isInCheckmarkZone(lastTouchX, lastTouchY)) {
          Serial.println("  *** CONTROL ZONE TAP - removing spurious char ***");
          String fixed = String(txt).substring(0, currentLen - 1);
          lv_textarea_set_text(ta, fixed.c_str());
          textWhenKeyboardClosed = fixed;
          lastTextareaLen = fixed.length();
          return;
        }
      } else {
        // Character was deleted or other change
        Serial.printf("VALUE_CHANGED: '%s' (len %d)\n", txt, currentLen);
      }

      textWhenKeyboardClosed = String(txt);
      lastTextareaLen = currentLen;
    }
  }
}

void mining_toggle_cb(lv_event_t *e) {
  if (e->code == LV_EVENT_VALUE_CHANGED) {
    // If currently mining, add elapsed time to total
    if (miningEnabled && lastMiningStateChange > 0) {
      totalMiningTime += (millis() - lastMiningStateChange) / 1000;
    }
    
    miningEnabled = lv_obj_has_state(miningToggle, LV_STATE_CHECKED);
    Serial.printf("Mining %s\n", miningEnabled ? "ENABLED" : "DISABLED");

    // Record when state changed
    lastMiningStateChange = millis();

    // Set or clear mining started timestamp
    if (miningEnabled) {
      struct tm timeinfo;
      if (getLocalTime(&timeinfo)) {
        char timeStr[32];
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M", &timeinfo);
        miningStartedTimestamp = String(timeStr);
      } else {
        miningStartedTimestamp = "Started";  // Fallback if NTP not synced
      }
    } else {
      miningStartedTimestamp = "";  // Clear when mining stops
    }

    // Save the setting to preferences immediately
    preferences.begin("kaspa", false);
    preferences.putBool("miningOn", miningEnabled);
    preferences.end();
    Serial.println("✓ Mining setting saved!");

    if (miningEnabled) {
      startMiningTask();
    } else {
      stopMiningTask();
    }
  }
}

// Beep function - simple tone using DAC or buzzer if connected
void playBeep(int frequency, int duration) {
  // Note: ESP32-S3 doesn't have built-in DAC, but we can simulate feedback
  // For now, just log - you can add external buzzer on a GPIO pin if desired
  Serial.printf("🔊 BEEP: %dHz for %dms\n", frequency, duration);
  // If you add a buzzer to a GPIO pin, use: tone(BUZZER_PIN, frequency, duration);
}

// Show save confirmation on screen
void showSaveConfirmation() {
  lv_color_t kaspa_green = lv_color_hex(0x49EACB);
  
  // Create overlay
  lv_obj_t *overlay = lv_obj_create(lv_scr_act());
  lv_obj_set_size(overlay, 800, 480);
  lv_obj_set_pos(overlay, 0, 0);
  lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
  lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
  
  // Create confirmation box
  lv_obj_t *box = lv_obj_create(overlay);
  lv_obj_set_size(box, 400, 250);
  lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_radius(box, 0, 0);
  lv_obj_set_style_bg_color(box, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(box, 3, 0);
  lv_obj_set_style_border_color(box, kaspa_green, 0);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  
  // Checkmark circle
  lv_obj_t *checkCircle = lv_obj_create(box);
  lv_obj_set_size(checkCircle, 80, 80);
  lv_obj_align(checkCircle, LV_ALIGN_TOP_MID, 0, 30);
  lv_obj_set_style_radius(checkCircle, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(checkCircle, kaspa_green, 0);
  lv_obj_set_style_border_width(checkCircle, 0, 0);
  
  // Checkmark symbol (use "OK" instead of unicode checkmark)
  lv_obj_t *checkLabel = lv_label_create(checkCircle);
  lv_label_set_text(checkLabel, "OK");
  lv_obj_set_style_text_font(checkLabel, &lv_font_montserrat_32, 0);
  lv_obj_set_style_text_color(checkLabel, lv_color_hex(0x000000), 0);
  lv_obj_center(checkLabel);
  
  // Success message
  lv_obj_t *msgLabel = lv_label_create(box);
  lv_label_set_text(msgLabel, "Settings Saved!");
  lv_obj_set_style_text_font(msgLabel, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(msgLabel, kaspa_green, 0);
  lv_obj_align(msgLabel, LV_ALIGN_CENTER, 0, 10);
  
  // Detail message
  lv_obj_t *detailLabel = lv_label_create(box);
  lv_label_set_text(detailLabel, "Configuration updated successfully");
  lv_obj_set_style_text_font(detailLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(detailLabel, lv_color_hex(0x888888), 0);
  lv_obj_align(detailLabel, LV_ALIGN_CENTER, 0, 45);
  
  // Play success beep
  playBeep(1000, 100);  // 1000Hz for 100ms
  
  // Auto-close after 2 seconds
  lv_timer_t *timer = lv_timer_create([](lv_timer_t *t) {
    lv_obj_t *overlay = (lv_obj_t*)t->user_data;
    lv_obj_del(overlay);
    lv_timer_del(t);
  }, 2000, overlay);
  lv_timer_set_repeat_count(timer, 1);
}

// Show block found celebration on LCD
void showBlockCelebration(bool isSample, uint32_t blockNum) {
  lv_color_t kaspa_green = lv_color_hex(0x49EACB);
  lv_color_t gold = lv_color_hex(0xFFD700);
  
  // Create overlay
  lv_obj_t *overlay = lv_obj_create(lv_scr_act());
  lv_obj_set_size(overlay, 800, 480);
  lv_obj_set_pos(overlay, 0, 0);
  lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_90, 0);
  lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
  
  // Create celebration box
  lv_obj_t *box = lv_obj_create(overlay);
  lv_obj_set_size(box, 600, 350);
  lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_radius(box, 0, 0);
  lv_obj_set_style_bg_color(box, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(box, 5, 0);
  lv_obj_set_style_border_color(box, isSample ? kaspa_green : gold, 0);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  
  // Top banner text instead of emoji (fonts don't support emojis)
  lv_obj_t *topBanner = lv_label_create(box);
  lv_label_set_text(topBanner, isSample ? "* * * *" : "* * * *");
  lv_obj_set_style_text_font(topBanner, &lv_font_montserrat_32, 0);
  lv_obj_set_style_text_color(topBanner, isSample ? kaspa_green : gold, 0);
  lv_obj_align(topBanner, LV_ALIGN_TOP_MID, 0, 30);
  
  // Main message
  lv_obj_t *msgLabel = lv_label_create(box);
  if (isSample) {
    lv_label_set_text(msgLabel, "SAMPLE BLOCK!");
  } else {
    lv_label_set_text(msgLabel, "BLOCK FOUND!");
  }
  lv_obj_set_style_text_font(msgLabel, &lv_font_montserrat_32, 0);
  lv_obj_set_style_text_color(msgLabel, isSample ? kaspa_green : gold, 0);
  lv_obj_align(msgLabel, LV_ALIGN_CENTER, 0, -20);
  
  // Block number / Sample message
  lv_obj_t *detailLabel = lv_label_create(box);
  if (isSample) {
    lv_label_set_text(detailLabel, "(Demo after 3 shares)\nThis is what a real block looks like!");
  } else {
    char blockText[50];
    snprintf(blockText, sizeof(blockText), "Block #%u", blockNum);
    lv_label_set_text(detailLabel, blockText);
  }
  lv_obj_set_style_text_font(detailLabel, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(detailLabel, lv_color_hex(0xCCCCCC), 0);
  lv_obj_set_style_text_align(detailLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(detailLabel, LV_ALIGN_CENTER, 0, 30);
  
  // Congratulations
  lv_obj_t *congratsLabel = lv_label_create(box);
  lv_label_set_text(congratsLabel, isSample ? "Keep Mining!" : "CONGRATULATIONS!");
  lv_obj_set_style_text_font(congratsLabel, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(congratsLabel, lv_color_hex(0x888888), 0);
  lv_obj_align(congratsLabel, LV_ALIGN_BOTTOM_MID, 0, -30);
  
  // Play celebration beep
  playBeep(2000, 200);  // High pitch for 200ms
  delay(250);
  playBeep(2500, 200);  // Even higher!
  delay(250);
  playBeep(3000, 300);  // Highest!
  
  // Auto-close after 5 seconds
  lv_timer_t *timer = lv_timer_create([](lv_timer_t *t) {
    lv_obj_t *overlay = (lv_obj_t*)t->user_data;
    lv_obj_del(overlay);
    lv_timer_del(t);
  }, 5000, overlay);
  lv_timer_set_repeat_count(timer, 1);
}

// Show error notification
void showErrorNotification(const char* message) {
  lv_color_t red = lv_color_hex(0xFF0000);
  
  lv_obj_t *overlay = lv_obj_create(lv_scr_act());
  lv_obj_set_size(overlay, 800, 480);
  lv_obj_set_pos(overlay, 0, 0);
  lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
  lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
  
  lv_obj_t *box = lv_obj_create(overlay);
  lv_obj_set_size(box, 400, 200);
  lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_radius(box, 0, 0);
  lv_obj_set_style_bg_color(box, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(box, 3, 0);
  lv_obj_set_style_border_color(box, red, 0);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  
  lv_obj_t *errorLabel = lv_label_create(box);
  lv_label_set_text(errorLabel, "!");
  lv_obj_set_style_text_font(errorLabel, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(errorLabel, red, 0);
  lv_obj_align(errorLabel, LV_ALIGN_TOP_MID, 0, 30);
  
  lv_obj_t *msgLabel = lv_label_create(box);
  lv_label_set_text(msgLabel, message);
  lv_obj_set_style_text_font(msgLabel, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(msgLabel, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_align(msgLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(msgLabel, LV_ALIGN_CENTER, 0, 20);
  lv_label_set_long_mode(msgLabel, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(msgLabel, 350);
  
  // Play error beep (lower frequency, longer)
  playBeep(400, 200);
  
  lv_timer_t *timer = lv_timer_create([](lv_timer_t *t) {
    lv_obj_t *overlay = (lv_obj_t*)t->user_data;
    lv_obj_del(overlay);
    lv_timer_del(t);
  }, 3000, overlay);
  lv_timer_set_repeat_count(timer, 1);
}

// Show success notification (green)
void showSuccessNotification(const char* message) {
  lv_color_t green = lv_color_hex(0x49EACB);  // Kaspa green

  lv_obj_t *overlay = lv_obj_create(lv_scr_act());
  lv_obj_set_size(overlay, 800, 480);
  lv_obj_set_pos(overlay, 0, 0);
  lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
  lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *box = lv_obj_create(overlay);
  lv_obj_set_size(box, 400, 200);
  lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_radius(box, 0, 0);
  lv_obj_set_style_bg_color(box, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(box, 3, 0);
  lv_obj_set_style_border_color(box, green, 0);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *successLabel = lv_label_create(box);
  lv_label_set_text(successLabel, "OK");
  lv_obj_set_style_text_font(successLabel, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(successLabel, green, 0);
  lv_obj_align(successLabel, LV_ALIGN_TOP_MID, 0, 30);

  lv_obj_t *msgLabel = lv_label_create(box);
  lv_label_set_text(msgLabel, message);
  lv_obj_set_style_text_font(msgLabel, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(msgLabel, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_align(msgLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(msgLabel, LV_ALIGN_CENTER, 0, 20);
  lv_label_set_long_mode(msgLabel, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(msgLabel, 350);

  // Play success beep (higher frequency, shorter)
  playBeep(1000, 100);

  lv_timer_t *timer = lv_timer_create([](lv_timer_t *t) {
    lv_obj_t *overlay = (lv_obj_t*)t->user_data;
    lv_obj_del(overlay);
    lv_timer_del(t);
  }, 2500, overlay);
  lv_timer_set_repeat_count(timer, 1);
}

void save_miner_config_cb(lv_event_t *e) {
  if (e->code == LV_EVENT_CLICKED) {
    // Save mining enabled state (only thing that can be changed now)
    preferences.begin("kaspa", false);
    preferences.putBool("mining", miningEnabled);
    preferences.end();
    
    Serial.println("✓ Miner config saved!");
    
    // Close menu
    if (minerConfigMenu) {
      lv_obj_del(minerConfigMenu);
      minerConfigMenu = nullptr;
      statusLabel = nullptr;  // Clear the pointer since the label is deleted
    }
    
    // Show visual confirmation
    showSaveConfirmation();
  }
}

void close_miner_config_cb(lv_event_t *e) {
  if (e->code == LV_EVENT_CLICKED) {
    // Settings are auto-saved when toggle is changed
    Serial.println("Miner config closed (settings auto-saved)");
    
    if (minerConfigMenu) {
      lv_obj_del(minerConfigMenu);
      minerConfigMenu = nullptr;
      statusLabel = nullptr;  // Clear the pointer since the label is deleted
    }
    
    // Return to Settings Menu
    show_settings_menu();
  }
}

void web_config_btn_cb(lv_event_t *e) {
  if (e->code == LV_EVENT_CLICKED) {
    String ip = WiFi.localIP().toString();
    
    Serial.println("\n====================================");
    Serial.println("WEB CONFIG NOT YET IMPLEMENTED");
    Serial.printf("Will be available at: http://%s/config\n", ip.c_str());
    Serial.println("For now, use the on-screen keyboard");
    Serial.println("====================================\n");
    
    // Show popup message on screen
    lv_obj_t *msgBox = lv_msgbox_create(lv_scr_act(), "Web Config", 
                                         "Coming Soon!\nUse keyboard for now.", 
                                         NULL, true);
    lv_obj_center(msgBox);
  }
}

void miner_config_btn_cb(lv_event_t *e) {
  if (e->code == LV_EVENT_CLICKED) {
    if (settingsMenu) {
      lv_obj_del(settingsMenu);
      settingsMenu = nullptr;
    }
    show_miner_config_menu();
  }
}

void testConnectivity() {
  Serial.println("\n=== Testing Network Connectivity ===");
  IPAddress testIP;
  if (WiFi.hostByName("api.coingecko.com", testIP)) {
    Serial.printf("DNS OK - api.coingecko.com: %s\n", testIP.toString().c_str());
  } else {
    Serial.println("DNS FAILED");
  }
  Serial.println("=== Connectivity Test Complete ===\n");
}

void wifi_network_selected_cb(lv_event_t *e) {
  if (e->code == LV_EVENT_CLICKED) {
    lv_color_t kaspa_green = lv_color_hex(0x49EACB);
    lv_obj_t *btn = lv_event_get_target(e);
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    selectedSSID = wifiNetworks[index];
    Serial.printf("Selected network: %s\n", selectedSSID.c_str());
    
    // Clear previous selection highlighting
    if (selectedWifiButton && selectedWifiButton != btn && lv_obj_is_valid(selectedWifiButton)) {
      lv_obj_set_style_bg_color(selectedWifiButton, lv_color_hex(0x000000), 0);
      lv_obj_set_style_border_color(selectedWifiButton, kaspa_green, 0);
      lv_obj_set_style_text_color(selectedWifiButton, kaspa_green, 0);
    }
    
    // Highlight the selected button - invert colors for visibility
    selectedWifiButton = btn;
    lv_obj_set_style_bg_color(btn, kaspa_green, 0);  // Fill with Kaspa green
    lv_obj_set_style_border_color(btn, kaspa_green, 0);
    lv_obj_set_style_text_color(btn, lv_color_hex(0x000000), 0);  // Black text on green
    
    // Update the selected SSID display label
    if (ssidDisplayLabel && lv_obj_is_valid(ssidDisplayLabel)) {
      lv_label_set_text(ssidDisplayLabel, selectedSSID.c_str());
    }
    
    // Force immediate visual update
    lv_obj_invalidate(btn);
  }
}

void wifi_connect_btn_cb(lv_event_t *e) {
  if (e->code == LV_EVENT_CLICKED) {
    playBeep(800, 50);  // Click feedback

    // Switch password to hidden mode before connecting
    if (passwordTextarea) {
      lv_textarea_set_password_mode(passwordTextarea, true);
    }

    const char *password = lv_textarea_get_text(passwordTextarea);

    if (selectedSSID.length() == 0) {
      Serial.println("No network selected!");
      showErrorNotification("Please select a network first");
      return;
    }

    // Store password for async connection
    pendingPassword = String(password);

    // Start WiFi connection state machine
    wifiState = WIFI_CONNECTING;
    wifiConnectStartTime = millis();

    // Create "Connecting..." overlay on top layer
    lv_color_t kaspa_green = lv_color_hex(0x49EACB);
    connectingOverlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(connectingOverlay, 800, 480);
    lv_obj_set_pos(connectingOverlay, 0, 0);
    lv_obj_set_style_bg_color(connectingOverlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(connectingOverlay, 242, 0);
    lv_obj_set_style_border_width(connectingOverlay, 0, 0);
    lv_obj_clear_flag(connectingOverlay, LV_OBJ_FLAG_SCROLLABLE);

    // Status label
    connectingStatusLabel = lv_label_create(connectingOverlay);
    lv_label_set_text(connectingStatusLabel, "Connecting to WiFi...");
    lv_obj_set_style_text_font(connectingStatusLabel, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(connectingStatusLabel, kaspa_green, 0);
    lv_obj_align(connectingStatusLabel, LV_ALIGN_CENTER, 0, -20);

    // SSID label
    lv_obj_t *ssidLabel = lv_label_create(connectingOverlay);
    lv_label_set_text(ssidLabel, selectedSSID.c_str());
    lv_obj_set_style_text_font(ssidLabel, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(ssidLabel, lv_color_hex(0xCCCCCC), 0);
    lv_obj_align(ssidLabel, LV_ALIGN_CENTER, 0, 20);

    Serial.printf("Connecting to: %s\n", selectedSSID.c_str());
    Serial.println("Enabling WiFi persistent mode...");

    // Enable WiFi credential persistence BEFORE connecting
    WiFi.persistent(true);

    Serial.printf("Starting WiFi.begin() with SSID: %s\n", selectedSSID.c_str());

    // Start WiFi connection (non-blocking)
    WiFi.begin(selectedSSID.c_str(), pendingPassword.c_str());

    // Exit callback immediately - connection will be handled in loop()
  }
}

void close_wifi_config_cb(lv_event_t *e) {
  if (e->code == LV_EVENT_CLICKED) {
    playBeep(800, 50);

    // Check if we're in first boot wizard mode
    if (firstBoot && wizardStep == 2) {
      // Show confirmation dialog during first boot
      lv_obj_t *confirmBox = lv_obj_create(lv_scr_act());
      lv_obj_set_size(confirmBox, 560, 320);  // Taller to fit all text above buttons
      lv_obj_align(confirmBox, LV_ALIGN_CENTER, 0, 0);
      lv_obj_set_style_bg_color(confirmBox, lv_color_hex(0x000000), 0);
      lv_obj_set_style_border_width(confirmBox, 3, 0);
      lv_obj_set_style_border_color(confirmBox, lv_color_hex(0xFF9900), 0);
      lv_obj_clear_flag(confirmBox, LV_OBJ_FLAG_SCROLLABLE);

      lv_obj_t *msgLabel = lv_label_create(confirmBox);
      lv_label_set_text(msgLabel,
        "Skip WiFi Setup?\n\n"
        "WARNING: KASDeck requires WiFi to function.\n"
        "Price data, mining, and all features will be\n"
        "unavailable without network connection.\n\n"
        "To configure WiFi later:\n"
        "1. Connect to WiFi network: KASDeck\n"
        "2. Password: kaspa123\n"
        "3. Use Settings menu on device");
      lv_obj_set_style_text_font(msgLabel, &lv_font_montserrat_12, 0);  // Smaller font (12 instead of 14)
      lv_obj_set_style_text_color(msgLabel, lv_color_hex(0xFFFFFF), 0);
      lv_obj_set_style_text_align(msgLabel, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_width(msgLabel, 520);
      lv_label_set_long_mode(msgLabel, LV_LABEL_LONG_WRAP);
      lv_obj_align(msgLabel, LV_ALIGN_TOP_MID, 0, 15);  // Slightly higher (15 instead of 20)

      // Skip button
      lv_obj_t *skipBtn = lv_btn_create(confirmBox);
      lv_obj_set_size(skipBtn, 200, 45);
      lv_obj_align(skipBtn, LV_ALIGN_BOTTOM_LEFT, 20, -15);
      lv_obj_set_style_radius(skipBtn, 0, 0);
      lv_obj_set_style_bg_color(skipBtn, lv_color_hex(0xFF9900), 0);
      lv_obj_set_style_border_width(skipBtn, 0, 0);
      lv_obj_add_event_cb(skipBtn, [](lv_event_t *e) {
        if (e->code == LV_EVENT_CLICKED) {
          playBeep(800, 50);
          // Close confirmation dialog
          lv_obj_t *box = lv_obj_get_parent(lv_event_get_target(e));
          lv_obj_del(box);

          // Close WiFi config
          if (currentKeyboard) {
            lv_obj_del(currentKeyboard);
            currentKeyboard = nullptr;
          }
          if (wifiScanInProgress) {
            WiFi.scanDelete();
            wifiScanInProgress = false;
            wifiScanComplete = false;
          }
          if (wifiConfigMenu) {
            lv_obj_del(wifiConfigMenu);
            wifiConfigMenu = nullptr;
          }

          // Exit wizard - mark as done and load main UI
          firstBoot = false;
          preferences.begin("kaspa", false);
          preferences.putBool("wizardDone", true);
          preferences.end();
          create_ui();
        }
      }, LV_EVENT_CLICKED, NULL);

      lv_obj_t *skipLabel = lv_label_create(skipBtn);
      lv_label_set_text(skipLabel, "SKIP");
      lv_obj_set_style_text_font(skipLabel, &lv_font_montserrat_16, 0);
      lv_obj_set_style_text_color(skipLabel, lv_color_hex(0x000000), 0);
      lv_obj_center(skipLabel);

      // Go Back button
      lv_obj_t *backBtn = lv_btn_create(confirmBox);
      lv_obj_set_size(backBtn, 200, 45);
      lv_obj_align(backBtn, LV_ALIGN_BOTTOM_RIGHT, -20, -15);
      lv_obj_set_style_radius(backBtn, 0, 0);
      lv_obj_set_style_bg_color(backBtn, lv_color_hex(0x49EACB), 0);
      lv_obj_set_style_border_width(backBtn, 0, 0);
      lv_obj_add_event_cb(backBtn, [](lv_event_t *e) {
        if (e->code == LV_EVENT_CLICKED) {
          playBeep(800, 50);
          lv_obj_t *box = lv_obj_get_parent(lv_event_get_target(e));
          lv_obj_del(box);
        }
      }, LV_EVENT_CLICKED, NULL);

      lv_obj_t *backLabel = lv_label_create(backBtn);
      lv_label_set_text(backLabel, "GO BACK");
      lv_obj_set_style_text_font(backLabel, &lv_font_montserrat_16, 0);
      lv_obj_set_style_text_color(backLabel, lv_color_hex(0x000000), 0);
      lv_obj_center(backLabel);

      return;  // Don't proceed with normal close logic
    }

    // Normal mode (not first boot) - standard close behavior
    // Close keyboard if it's open
    if (currentKeyboard) {
      lv_obj_del(currentKeyboard);
      currentKeyboard = nullptr;
    }

    // Clear WiFi selection pointers
    selectedWifiButton = nullptr;
    ssidDisplayLabel = nullptr;
    passwordTextarea = nullptr;
    scanLabel = nullptr;
    wifiList = nullptr;

    // Cancel any in-progress WiFi scan
    if (wifiScanInProgress) {
      WiFi.scanDelete();
      wifiScanInProgress = false;
      wifiScanComplete = false;
    }

    // Close WiFi config menu
    if (wifiConfigMenu) {
      lv_obj_del(wifiConfigMenu);
      wifiConfigMenu = nullptr;
    }

    // Show settings menu instead of going back to main screen
    show_settings_menu();
  }
}

void password_toggle_cb(lv_event_t *e) {
  if (e->code == LV_EVENT_CLICKED && passwordTextarea) {
    // Toggle password visibility
    bool isHidden = lv_textarea_get_password_mode(passwordTextarea);
    lv_textarea_set_password_mode(passwordTextarea, !isHidden);
    
    // Update button text
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    if (isHidden) {
      lv_label_set_text(label, "Hide Password");
    } else {
      lv_label_set_text(label, "Show Password");
    }
  }
}

// Called when password textarea value changes - checks for spurious chars after keyboard close
void wifi_password_value_changed_cb(lv_event_t *e) {
  if (e->code == LV_EVENT_VALUE_CHANGED) {
    lv_obj_t *ta = lv_event_get_target(e);

    // Only check if keyboard is NOT open (spurious char happens after keyboard closes)
    if (currentKeyboard == nullptr && keyboardCloseTime > 0) {
      unsigned long timeSinceClose = millis() - keyboardCloseTime;

      // If a character was added within 300ms of keyboard closing, it's spurious
      if (timeSinceClose < 300) {
        const char *currentTxt = lv_textarea_get_text(ta);
        uint32_t currentLen = strlen(currentTxt);

        if (currentLen > textWhenKeyboardClosed.length()) {
          Serial.printf("*** SPURIOUS CHAR DETECTED %lu ms after keyboard close! ***\n", timeSinceClose);
          Serial.printf("    Was: '%s', Now: '%s'\n", textWhenKeyboardClosed.c_str(), currentTxt);
          Serial.printf("    Restoring to: '%s'\n", textWhenKeyboardClosed.c_str());
          lv_textarea_set_text(ta, textWhenKeyboardClosed.c_str());
          keyboardCloseTime = 0;  // Clear so we don't keep checking
          return;
        }
      } else {
        // More than 300ms passed, clear the check
        keyboardCloseTime = 0;
      }
    }
  }
}

void wifi_password_clicked_cb(lv_event_t *e) {
  if (e->code == LV_EVENT_CLICKED) {
    // If keyboard already exists, don't create another
    if (currentKeyboard) {
      return;
    }

    Serial.println("Opening password keyboard");

    // Create terminal-themed keyboard with built-in layout
    lv_color_t kaspa_green = lv_color_hex(0x49EACB);
    currentKeyboard = lv_keyboard_create(lv_scr_act());
    lv_keyboard_set_textarea(currentKeyboard, passwordTextarea);
    lv_keyboard_set_mode(currentKeyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_set_size(currentKeyboard, 800, 240);
    lv_obj_align(currentKeyboard, LV_ALIGN_BOTTOM_MID, 0, 0);

    // Terminal theme styling
    lv_obj_set_style_bg_color(currentKeyboard, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(currentKeyboard, 2, 0);
    lv_obj_set_style_border_color(currentKeyboard, kaspa_green, 0);
    lv_obj_set_style_radius(currentKeyboard, 0, 0);

    // Style individual keys
    lv_obj_set_style_bg_color(currentKeyboard, lv_color_hex(0x1a1a1a), LV_PART_ITEMS);
    lv_obj_set_style_text_color(currentKeyboard, kaspa_green, LV_PART_ITEMS);
    lv_obj_set_style_radius(currentKeyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_border_width(currentKeyboard, 1, LV_PART_ITEMS);
    lv_obj_set_style_border_color(currentKeyboard, kaspa_green, LV_PART_ITEMS);
    lv_obj_set_style_border_opa(currentKeyboard, LV_OPA_30, LV_PART_ITEMS);

    // Listen for READY (checkmark), CANCEL (hide keyboard), and VALUE_CHANGED
    // The checkmark zone fix in keyboard_event_cb handles spurious characters
    lv_obj_add_event_cb(currentKeyboard, keyboard_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(currentKeyboard, keyboard_event_cb, LV_EVENT_CANCEL, NULL);
    lv_obj_add_event_cb(currentKeyboard, keyboard_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
  }
}

void wifi_config_btn_cb(lv_event_t *e) {
  if (e->code == LV_EVENT_CLICKED) {
    // Check if mining is enabled - WiFi scanning fails while mining
    if (miningEnabled) {
      playBeep(600, 100);

      lv_color_t kaspa_green = lv_color_hex(0x49EACB);
      lv_color_t orange = lv_color_hex(0xFF9900);

      // Create overlay
      lv_obj_t *confirmOverlay = lv_obj_create(lv_scr_act());
      lv_obj_set_size(confirmOverlay, 800, 480);
      lv_obj_set_pos(confirmOverlay, 0, 0);
      lv_obj_set_style_bg_color(confirmOverlay, lv_color_hex(0x000000), 0);
      lv_obj_set_style_bg_opa(confirmOverlay, LV_OPA_90, 0);
      lv_obj_set_style_border_width(confirmOverlay, 0, 0);
      lv_obj_clear_flag(confirmOverlay, LV_OBJ_FLAG_SCROLLABLE);

      // Create dialog box
      lv_obj_t *confirmBox = lv_obj_create(confirmOverlay);
      lv_obj_set_size(confirmBox, 500, 280);
      lv_obj_align(confirmBox, LV_ALIGN_CENTER, 0, 0);
      lv_obj_set_style_radius(confirmBox, 0, 0);
      lv_obj_set_style_bg_color(confirmBox, lv_color_hex(0x000000), 0);
      lv_obj_set_style_border_width(confirmBox, 3, 0);
      lv_obj_set_style_border_color(confirmBox, orange, 0);
      lv_obj_clear_flag(confirmBox, LV_OBJ_FLAG_SCROLLABLE);

      // Title
      lv_obj_t *titleLabel = lv_label_create(confirmBox);
      lv_label_set_text(titleLabel, "Mining Active");
      lv_obj_set_style_text_font(titleLabel, &lv_font_montserrat_24, 0);
      lv_obj_set_style_text_color(titleLabel, orange, 0);
      lv_obj_align(titleLabel, LV_ALIGN_TOP_MID, 0, 25);

      // Message
      lv_obj_t *msgLabel = lv_label_create(confirmBox);
      lv_label_set_text(msgLabel, "WiFi scanning cannot run while\nmining is active.\n\nWould you like to stop mining\nto configure WiFi?");
      lv_obj_set_style_text_font(msgLabel, &lv_font_montserrat_14, 0);
      lv_obj_set_style_text_color(msgLabel, lv_color_hex(0xCCCCCC), 0);
      lv_obj_set_style_text_align(msgLabel, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_align(msgLabel, LV_ALIGN_TOP_MID, 0, 70);
      lv_obj_set_width(msgLabel, 420);
      lv_label_set_long_mode(msgLabel, LV_LABEL_LONG_WRAP);

      // "Stop Mining" button (left)
      lv_obj_t *stopBtn = lv_btn_create(confirmBox);
      lv_obj_set_size(stopBtn, 190, 50);
      lv_obj_align(stopBtn, LV_ALIGN_BOTTOM_LEFT, 30, -25);
      lv_obj_set_style_radius(stopBtn, 0, 0);
      lv_obj_set_style_bg_color(stopBtn, orange, 0);
      lv_obj_set_style_border_width(stopBtn, 0, 0);
      lv_obj_set_style_bg_color(stopBtn, lv_color_hex(0x000000), LV_STATE_PRESSED);
      lv_obj_set_style_border_width(stopBtn, 2, LV_STATE_PRESSED);
      lv_obj_set_style_border_color(stopBtn, orange, LV_STATE_PRESSED);
      lv_obj_add_event_cb(stopBtn, [](lv_event_t *e) {
        if (e->code == LV_EVENT_CLICKED) {
          playBeep(800, 100);

          // Stop mining
          if (miningEnabled && lastMiningStateChange > 0) {
            totalMiningTime += (millis() - lastMiningStateChange) / 1000;
          }
          miningEnabled = false;
          lastMiningStateChange = millis();

          // Save state
          preferences.begin("kaspa", false);
          preferences.putBool("miningOn", false);
          preferences.end();

          Serial.println("Mining stopped for WiFi configuration");

          // Close the dialog
          lv_obj_t *overlay = lv_obj_get_parent(lv_obj_get_parent(lv_event_get_target(e)));
          lv_obj_del(overlay);

          // Close settings menu if open
          if (settingsMenu) {
            lv_obj_del(settingsMenu);
            settingsMenu = nullptr;
          }

          // Now show WiFi config
          show_wifi_config_menu();
        }
      }, LV_EVENT_CLICKED, NULL);

      lv_obj_t *stopLabel = lv_label_create(stopBtn);
      lv_label_set_text(stopLabel, "STOP MINING");
      lv_obj_set_style_text_font(stopLabel, &lv_font_montserrat_16, 0);
      lv_obj_set_style_text_color(stopLabel, lv_color_hex(0x000000), 0);
      lv_obj_set_style_text_color(stopLabel, orange, LV_STATE_PRESSED);
      lv_obj_center(stopLabel);

      // "Cancel" button (right)
      lv_obj_t *cancelBtn = lv_btn_create(confirmBox);
      lv_obj_set_size(cancelBtn, 190, 50);
      lv_obj_align(cancelBtn, LV_ALIGN_BOTTOM_RIGHT, -30, -25);
      lv_obj_set_style_radius(cancelBtn, 0, 0);
      lv_obj_set_style_bg_color(cancelBtn, lv_color_hex(0x000000), 0);
      lv_obj_set_style_border_width(cancelBtn, 2, 0);
      lv_obj_set_style_border_color(cancelBtn, kaspa_green, 0);
      lv_obj_set_style_bg_color(cancelBtn, kaspa_green, LV_STATE_PRESSED);
      lv_obj_add_event_cb(cancelBtn, [](lv_event_t *e) {
        if (e->code == LV_EVENT_CLICKED) {
          playBeep(800, 50);
          lv_obj_t *overlay = lv_obj_get_parent(lv_obj_get_parent(lv_event_get_target(e)));
          lv_obj_del(overlay);
        }
      }, LV_EVENT_CLICKED, NULL);

      lv_obj_t *cancelLabel = lv_label_create(cancelBtn);
      lv_label_set_text(cancelLabel, "CANCEL");
      lv_obj_set_style_text_font(cancelLabel, &lv_font_montserrat_16, 0);
      lv_obj_set_style_text_color(cancelLabel, kaspa_green, 0);
      lv_obj_set_style_text_color(cancelLabel, lv_color_hex(0x000000), LV_STATE_PRESSED);
      lv_obj_center(cancelLabel);

      return;  // Don't proceed to WiFi config while mining
    }

    // Mining not active, proceed normally
    if (settingsMenu) {
      lv_obj_del(settingsMenu);
      settingsMenu = nullptr;
    }
    show_wifi_config_menu();
  }
}

void fetchNetworkHashrate() {
  Serial.println("Fetching network hashrate from api.kaspa.org...");
  Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ WiFi not connected! Keeping last value.");
    if (apiStatusLabel) {
      lv_label_set_text(apiStatusLabel, "! No Connection");
      lv_obj_set_style_text_color(apiStatusLabel, lv_color_hex(0xFF0000), 0);
    }
    return;  // Don't update networkHashrate, keep last good value
  }
  
  WiFiClientSecure client;
  client.setInsecure();
  
  HTTPClient http;
  http.begin(client, "https://api.kaspa.org/info/hashrate");
  http.setTimeout(15000);
  
  Serial.println("Sending HTTPS GET request...");
  int httpCode = http.GET();
  Serial.printf("HTTP Response Code: %d\n", httpCode);
  
  if (httpCode == 200) {
    String payload = http.getString();
    Serial.printf("Payload size: %d bytes\n", payload.length());
    Serial.printf("Raw response: %s\n", payload.c_str());  // DEBUG - see what we're getting
    
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, payload);
    
    if (!error && doc.containsKey("hashrate")) {
      float rawHashrate = doc["hashrate"].as<float>();
      
      // kaspa.org returns hashrate in TH/s, we need PH/s
      networkHashrate = rawHashrate / 1000.0;  // TH/s to PH/s (divide by 1000)
      
      Serial.printf("✓ Network Hashrate: %.2f PH/s (raw: %.0f TH/s)\n", networkHashrate, rawHashrate);
      
      if (apiStatusLabel) {
        lv_label_set_text(apiStatusLabel, "Live Data");
        lv_obj_set_style_text_color(apiStatusLabel, lv_color_hex(0x00FF00), 0);
      }
    } else {
      Serial.println("❌ JSON parse error! Keeping last value.");
      // Silenced: Serial.printf("❌ JSON parse error: %s\n", error.c_str());
      if (apiStatusLabel) {
        lv_label_set_text(apiStatusLabel, "! Parse Error");
        lv_obj_set_style_text_color(apiStatusLabel, lv_color_hex(0xFFA500), 0);
      }
      http.end();
      return;  // Don't update chart, keep last good value
    }
  } else {
    Serial.printf("❌ HTTP error: %d! Keeping last value.\n", httpCode);
    if (apiStatusLabel) {
      lv_label_set_text(apiStatusLabel, "! API Error");
      lv_obj_set_style_text_color(apiStatusLabel, lv_color_hex(0xFFA500), 0);
    }
    http.end();
    return;  // Don't update chart, keep last good value
  }
  
  http.end();
  Serial.printf("Free heap after: %d bytes\n", ESP.getFreeHeap());
}

void saveHashrateHistory() {
  Preferences prefs;
  prefs.begin("kaspa", false);

  // Save the entire history array
  for (int i = 0; i < HASHRATE_POINTS; i++) {
    char key[16];
    sprintf(key, "hash_%d", i);
    prefs.putFloat(key, hashrateHistory[i]);
  }
  prefs.putInt("hashIndex", hashrateIndex);

  prefs.end();
  Serial.println("✓ Hashrate history saved to NVS");
}

void loadHashrateHistory() {
  Preferences prefs;
  prefs.begin("kaspa", true);  // Read-only

  for (int i = 0; i < HASHRATE_POINTS; i++) {
    char key[16];
    sprintf(key, "hash_%d", i);
    hashrateHistory[i] = prefs.getFloat(key, 0);
  }
  hashrateIndex = prefs.getInt("hashIndex", 0);

  prefs.end();
  Serial.println("✓ Hashrate history loaded from NVS");
  
  // Populate the LCD chart with loaded history
  populateChartFromHistory();
}

void populateChartFromHistory() {
  Serial.println("Populating chart from loaded history...");
  
  // Clear the chart first
  lv_chart_set_all_value(hashrateChart, hashrateSeries, LV_CHART_POINT_NONE);
  
  // Add all historical values to the chart
  for (int i = 0; i < HASHRATE_POINTS; i++) {
    if (hashrateHistory[i] > 0) {
      lv_chart_set_next_value(hashrateChart, hashrateSeries, (int)hashrateHistory[i]);
      Serial.printf("  Point %d: %.2f PH/s\n", i, hashrateHistory[i]);
    } else {
      lv_chart_set_next_value(hashrateChart, hashrateSeries, LV_CHART_POINT_NONE);
    }
  }
  
  // Update the chart range and labels based on loaded data
  float sum = 0, maxVal = 0, minVal = 999999;
  int count = 0;
  for (int i = 0; i < HASHRATE_POINTS; i++) {
    if (hashrateHistory[i] > 0) {
      sum += hashrateHistory[i];
      if (hashrateHistory[i] > maxVal) maxVal = hashrateHistory[i];
      if (hashrateHistory[i] < minVal) minVal = hashrateHistory[i];
      count++;
    }
  }
  
  if (count > 0) {
    float range = maxVal - minVal;
    float padding = range * 0.1;
    if (padding < 10) padding = 10;
    
    int chartMin = (int)(minVal - padding);
    int chartMax = (int)(maxVal + padding);
    
    if (chartMax - chartMin < 20) {
      int center = (chartMax + chartMin) / 2;
      chartMin = center - 10;
      chartMax = center + 10;
    }
    
    lv_chart_set_range(hashrateChart, LV_CHART_AXIS_PRIMARY_Y, chartMin, chartMax);
    
    // Update Y-axis labels
    int midValue = (chartMax + chartMin) / 2;
    if (yAxisMaxLabel) lv_label_set_text_fmt(yAxisMaxLabel, "%d", chartMax);
    if (yAxisMidLabel) lv_label_set_text_fmt(yAxisMidLabel, "%d", midValue);
    if (yAxisMinLabel) lv_label_set_text_fmt(yAxisMinLabel, "%d", chartMin);
    
    Serial.printf("Chart populated with %d points (range: %d-%d PH/s)\n", count, chartMin, chartMax);
  }
}

void miningLoopTask(void *pvParameters) {
    uint8_t hash_output[32];
    uint8_t work_buffer[80] = {0};  // 80 bytes zero-initialized to match runKeccak read size
    uint64_t nonce = esp_random();
    char lastJobId[65] = "";  // Track job changes - char array to avoid heap allocation

    // LOG: Mining task started (show which core it's actually running on)
    char core_msg[120];
    UBaseType_t stackHighWaterMark = uxTaskGetStackHighWaterMark(NULL);
    sprintf(core_msg, "Mining task started on Core %d, stack free: %u bytes", xPortGetCoreID(), stackHighWaterMark);
    logToSD(core_msg);
    Serial.println(core_msg);

    while (true) {
        if (miningEnabled && hasJob) {
            // PERFORMANCE: Consolidated counter - ONE increment instead of multiple
            // Used for both watchdog feeding AND job checking (every 1000 hashes)
            static uint32_t hash_counter = 0;
            hash_counter++;

            // PERFORMANCE CRITICAL: Static local copies to avoid mutex lock on EVERY hash
            // These are updated only every 1000 hashes (jobs change ~1/second, we hash 1500+/second)
            static uint8_t local_target[32];
            static char local_jobId[65] = "";
            static bool first_job_received = false;

            // FAST PATH: Skip all overhead for hashes 1-999 of every 1000
            // Only do periodic maintenance every 1000th hash
            bool shouldCheckJob = !first_job_received || (hash_counter >= 1000);

            if (shouldCheckJob) {
                // Reset counter and feed watchdog
                hash_counter = 0;
                vTaskDelay(1);  // Feed watchdog every 1000 hashes
                first_job_received = true;

                // Periodic job check and mutex lock (every 1000 hashes)
                bool jobChanged = false;
                if (xSemaphoreTake(miningStateMutex, portMAX_DELAY) == pdTRUE) {
                    // NO Serial.println() inside mutex - not thread-safe!
                    // Copy all shared state atomically
                    bool jobChangedLocal = (strcmp(currentJobId, lastJobId) != 0);
                    if (jobChangedLocal) {
                        memcpy(work_buffer, currentHeaderHash, 32);
                        strncpy(lastJobId, currentJobId, 64);
                        lastJobId[64] = '\0';
                        jobsReceived++;
                        jobChanged = true;
                    }

                    // ALWAYS copy target and job ID (can change independently)
                    memcpy(local_target, currentTarget, 32);
                    strncpy(local_jobId, currentJobId, 64);
                    local_jobId[64] = '\0';

                    // UNLOCK MUTEX after reading shared state
                    xSemaphoreGive(miningStateMutex);

                    // Debug logging removed for performance
                } else {
                    Serial.println("ERROR: Failed to acquire mutex in miningLoopTask");
                    vTaskDelay(100);
                    continue;  // Skip this iteration
                }
            }  // End of periodic job check

            // ===== FAST PATH: Critical mining loop (999 out of 1000 hashes) =====
            // Update nonce (direct pointer write - faster than memcpy)
            *((uint64_t*)(work_buffer + 32)) = nonce;

            // Compute KHeavyHash
            kheavyState.compute(work_buffer, hash_output);

            if (checkDifficulty(hash_output, local_target)) {
                Serial.println("\n🎉🎉🎉 SHARE FOUND! 🎉🎉🎉");
                Serial.printf("Nonce: %llu\n", nonce);
                Serial.print("Hash:   ");
                for (int i = 0; i < 32; i++) Serial.printf("%02x", hash_output[i]);
                Serial.println();
                Serial.print("Target: ");
                for (int i = 0; i < 32; i++) Serial.printf("%02x", local_target[i]);
                Serial.println();
                Serial.printf("Nonce:  %016llx\n", nonce);
                Serial.printf("Job ID: %s\n", local_jobId);
                Serial.printf("Total hashes: %llu\n\n", totalHashes);

                // Submit share
                submitShare(String(local_jobId), nonce);
            }

            hashes_done++;
            totalHashes++;
            nonce++;

            // Watchdog is fed at the top of the loop (every 1000 iterations)
        } else {
            vTaskDelay(100);
        }
    }
}

void scanWiFiNetworks() {
  Serial.println("Starting WiFi network scan...");

  // Update UI to show scanning status
  if (scanLabel) {
    lv_label_set_text(scanLabel, "> SCANNING...");
    lv_obj_set_style_text_color(scanLabel, lv_color_hex(0xFFFF00), 0);
  }

  // Force LVGL to render the scanning indicator before we block
  lv_timer_handler();
  lv_refr_now(NULL);  // Force immediate screen refresh
  delay(50);  // Brief delay to ensure display updates

  // Check if there's already a scan running
  int existingScan = WiFi.scanComplete();

  // Clean up any existing scan
  if (existingScan != WIFI_SCAN_RUNNING) {
    WiFi.scanDelete();
  }

  if (WiFi.status() == WL_DISCONNECTED || WiFi.status() == WL_NO_SHIELD) {
    // If disconnected, ensure STA mode
    WiFi.mode(WIFI_STA);
    delay(100);
  }

  wifiScanInProgress = true;
  wifiScanComplete = false;
  wifiCount = 0;

  // Use synchronous scan - async fails when connected to WiFi
  // The lv_refr_now() above ensures user sees "SCANNING..." before we block
  Serial.println("Starting synchronous WiFi scan...");
  int16_t result = WiFi.scanNetworks(false, false);  // false=sync, false=no hidden

  if (result >= 0) {
    // Scan succeeded - filter out empty SSIDs
    Serial.printf("WiFi scan complete, found %d networks\n", result);

    int validCount = 0;
    for (int i = 0; i < result && validCount < MAX_WIFI_NETWORKS; i++) {
      String ssid = WiFi.SSID(i);
      // Skip empty/hidden SSIDs
      if (ssid.length() > 0) {
        wifiNetworks[validCount] = ssid;
        wifiSignals[validCount] = WiFi.RSSI(i);
        Serial.printf("%d: %s (%d dBm)\n", validCount, wifiNetworks[validCount].c_str(), wifiSignals[validCount]);
        validCount++;
      }
    }
    wifiCount = validCount;
    Serial.printf("Filtered to %d visible networks\n", wifiCount);

    wifiScanComplete = true;
    wifiScanInProgress = false;

    if (scanLabel) {
      lv_label_set_text(scanLabel, "AVAILABLE NETWORKS");
      lv_obj_set_style_text_color(scanLabel, lv_color_hex(0x49EACB), 0);
    }
    populateWiFiList();
    WiFi.scanDelete();
  } else {
    // Scan failed
    Serial.println("❌ WiFi scan failed");
    wifiCount = 0;
    wifiScanComplete = true;
    wifiScanInProgress = false;

    if (scanLabel) {
      lv_label_set_text(scanLabel, "SCAN FAILED - Click RESCAN");
      lv_obj_set_style_text_color(scanLabel, lv_color_hex(0xFF0000), 0);
    }
  }
}

void populateWiFiList() {
  if (!wifiList) return;

  lv_color_t kaspa_green = lv_color_hex(0x49EACB);

  // Clear existing list items
  lv_obj_clean(wifiList);

  // Add networks to list with LVGL bar for signal strength
  for (int i = 0; i < wifiCount; i++) {
    // Convert RSSI to percentage (0-100)
    int rssi = wifiSignals[i];
    int signalPercent;
    if (rssi >= -50) {
      signalPercent = 100;  // Excellent
    } else if (rssi >= -60) {
      signalPercent = 75;   // Good
    } else if (rssi >= -70) {
      signalPercent = 50;   // Fair
    } else if (rssi >= -80) {
      signalPercent = 25;   // Weak
    } else {
      signalPercent = 10;   // Very weak
    }

    // Create button with just SSID
    lv_obj_t *btn = lv_list_add_btn(wifiList, NULL, wifiNetworks[i].c_str());
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(btn, lv_color_hex(0xFFFFFF), LV_STATE_DEFAULT);  // White text
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, kaspa_green, 0);
    lv_obj_set_style_pad_right(btn, 45, 0);  // Make room for signal bar
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(btn, wifi_network_selected_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);

    // Add signal strength bar (positioned on right side of button)
    lv_obj_t *bar = lv_bar_create(btn);
    lv_obj_set_size(bar, 8, 30);  // Thin vertical bar
    lv_obj_align(bar, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, signalPercent, LV_ANIM_OFF);

    // Color based on signal strength
    if (signalPercent >= 75) {
      lv_obj_set_style_bg_color(bar, lv_color_hex(0x00FF00), LV_PART_INDICATOR);  // Green - strong
    } else if (signalPercent >= 50) {
      lv_obj_set_style_bg_color(bar, lv_color_hex(0xFFFF00), LV_PART_INDICATOR);  // Yellow - good
    } else if (signalPercent >= 25) {
      lv_obj_set_style_bg_color(bar, lv_color_hex(0xFF9900), LV_PART_INDICATOR);  // Orange - fair
    } else {
      lv_obj_set_style_bg_color(bar, lv_color_hex(0xFF0000), LV_PART_INDICATOR);  // Red - weak
    }
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x333333), LV_PART_MAIN);  // Dark gray background
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
  }
}

void checkWiFiScanComplete() {
  if (!wifiScanInProgress) return;

  int n = WiFi.scanComplete();

  if (n == WIFI_SCAN_RUNNING) {
    // Still scanning, do nothing - let UI continue to show "SCANNING..."
    return;
  }

  if (n >= 0) {
    // Scan completed successfully - filter out empty SSIDs
    Serial.printf("WiFi scan complete, found %d networks\n", n);

    int validCount = 0;
    for (int i = 0; i < n && validCount < MAX_WIFI_NETWORKS; i++) {
      String ssid = WiFi.SSID(i);
      // Skip empty/hidden SSIDs
      if (ssid.length() > 0) {
        wifiNetworks[validCount] = ssid;
        wifiSignals[validCount] = WiFi.RSSI(i);
        Serial.printf("%d: %s (%d dBm)\n", validCount, wifiNetworks[validCount].c_str(), wifiSignals[validCount]);
        validCount++;
      }
    }
    wifiCount = validCount;
    Serial.printf("Filtered to %d visible networks\n", wifiCount);

    wifiScanComplete = true;
    wifiScanInProgress = false;

    if (scanLabel) {
      lv_label_set_text(scanLabel, "AVAILABLE NETWORKS");
      lv_obj_set_style_text_color(scanLabel, lv_color_hex(0x49EACB), 0);
    }
    populateWiFiList();
    WiFi.scanDelete();
  } else if (n == WIFI_SCAN_FAILED) {
    // Scan failed
    Serial.println("❌ WiFi scan failed - Click RESCAN to try again");
    wifiCount = 0;
    wifiScanComplete = true;
    wifiScanInProgress = false;

    if (scanLabel) {
      lv_label_set_text(scanLabel, "SCAN FAILED - Click RESCAN");
      lv_obj_set_style_text_color(scanLabel, lv_color_hex(0xFF0000), 0);
    }
  }
}

void show_wifi_config_menu() {
  if (wifiConfigMenu) return;
  
  lv_color_t kaspa_green = lv_color_hex(0x49EACB);
  
  // Create fullscreen terminal-style menu
  wifiConfigMenu = lv_obj_create(lv_scr_act());
  lv_obj_set_size(wifiConfigMenu, 800, 480);
  lv_obj_set_pos(wifiConfigMenu, 0, 0);
  lv_obj_set_style_bg_color(wifiConfigMenu, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(wifiConfigMenu, LV_OPA_100, 0);
  lv_obj_set_style_border_width(wifiConfigMenu, 0, 0);
  lv_obj_set_style_pad_all(wifiConfigMenu, 0, 0);
  lv_obj_clear_flag(wifiConfigMenu, LV_OBJ_FLAG_SCROLLABLE);
  
  // Title at top
  lv_obj_t *title = lv_label_create(wifiConfigMenu);
  lv_label_set_text(title, "WIFI CONFIG");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(title, kaspa_green, 0);
  lv_obj_set_pos(title, 20, 15);
  
  // Scanning status label (make global so checkWiFiScanComplete can update it)
  scanLabel = lv_label_create(wifiConfigMenu);
  lv_label_set_text(scanLabel, "> SCANNING...");
  lv_obj_set_style_text_color(scanLabel, lv_color_hex(0xFFFF00), 0);
  lv_obj_set_style_text_font(scanLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_pos(scanLabel, 20, 50);

  // Rescan button (right side of scanning label, above list)
  lv_obj_t *rescanBtn = lv_btn_create(wifiConfigMenu);
  lv_obj_set_size(rescanBtn, 120, 35);
  lv_obj_set_pos(rescanBtn, 360, 47);  // Moved right and positioned above list
  lv_obj_set_style_radius(rescanBtn, 4, 0);
  lv_obj_set_style_bg_color(rescanBtn, lv_color_hex(0x1a1a1a), 0);
  lv_obj_set_style_border_width(rescanBtn, 1, 0);
  lv_obj_set_style_border_color(rescanBtn, kaspa_green, 0);
  lv_obj_set_style_bg_color(rescanBtn, kaspa_green, LV_STATE_PRESSED);
  lv_obj_add_event_cb(rescanBtn, [](lv_event_t *e) {
    if (e->code == LV_EVENT_CLICKED) {
      playBeep(800, 50);
      Serial.println("Rescan button clicked - starting new WiFi scan");
      // Clear existing list
      if (wifiList) {
        lv_obj_clean(wifiList);
      }
      // Update scan label
      if (scanLabel) {
        lv_label_set_text(scanLabel, "> SCANNING...");
        lv_obj_set_style_text_color(scanLabel, lv_color_hex(0xFFFF00), 0);
      }
      // Start new scan
      scanWiFiNetworks();
    }
  }, LV_EVENT_CLICKED, NULL);

  lv_obj_t *rescanLabel = lv_label_create(rescanBtn);
  lv_label_set_text(rescanLabel, "RESCAN");
  lv_obj_set_style_text_font(rescanLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(rescanLabel, kaspa_green, 0);
  lv_obj_set_style_text_color(rescanLabel, lv_color_hex(0x000000), LV_STATE_PRESSED);
  lv_obj_center(rescanLabel);

  // ========== LEFT COLUMN - NETWORK LIST ==========
  wifiList = lv_list_create(wifiConfigMenu);
  lv_obj_set_size(wifiList, 460, 385);  // Slightly shorter to make room above
  lv_obj_set_pos(wifiList, 20, 90);  // Moved down to be below rescan button
  lv_obj_set_style_bg_color(wifiList, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(wifiList, 1, 0);
  lv_obj_set_style_border_color(wifiList, kaspa_green, 0);
  lv_obj_set_style_border_opa(wifiList, LV_OPA_30, 0);
  lv_obj_set_style_radius(wifiList, 0, 0);
  lv_obj_set_style_pad_all(wifiList, 5, 0);

  // Start async WiFi scan (non-blocking)
  // Networks will be populated by checkWiFiScanComplete() when scan finishes
  scanWiFiNetworks();

  // Manual SSID entry button (below network list)
  lv_obj_t *manualEntryBtn = lv_btn_create(wifiConfigMenu);
  lv_obj_set_size(manualEntryBtn, 460, 35);
  lv_obj_set_pos(manualEntryBtn, 20, 440);  // Below the list (list ends at ~385+90=475, but leave gap)
  lv_obj_set_style_radius(manualEntryBtn, 4, 0);
  lv_obj_set_style_bg_color(manualEntryBtn, lv_color_hex(0x1a1a1a), 0);
  lv_obj_set_style_border_width(manualEntryBtn, 1, 0);
  lv_obj_set_style_border_color(manualEntryBtn, kaspa_green, 0);
  lv_obj_set_style_bg_color(manualEntryBtn, kaspa_green, LV_STATE_PRESSED);
  lv_obj_add_event_cb(manualEntryBtn, [](lv_event_t *e) {
    if (e->code == LV_EVENT_CLICKED) {
      playBeep(800, 50);
      Serial.println("Manual SSID entry button clicked");

      // Create popup for manual SSID entry
      lv_obj_t *manualBox = lv_obj_create(lv_scr_act());
      lv_obj_set_size(manualBox, 500, 280);
      lv_obj_align(manualBox, LV_ALIGN_CENTER, 0, 0);
      lv_obj_set_style_bg_color(manualBox, lv_color_hex(0x000000), 0);
      lv_obj_set_style_border_width(manualBox, 3, 0);
      lv_obj_set_style_border_color(manualBox, lv_color_hex(0x49EACB), 0);
      lv_obj_clear_flag(manualBox, LV_OBJ_FLAG_SCROLLABLE);

      lv_obj_t *titleLabel = lv_label_create(manualBox);
      lv_label_set_text(titleLabel, "Enter Hidden Network SSID");
      lv_obj_set_style_text_font(titleLabel, &lv_font_montserrat_18, 0);
      lv_obj_set_style_text_color(titleLabel, lv_color_hex(0x49EACB), 0);
      lv_obj_align(titleLabel, LV_ALIGN_TOP_MID, 0, 15);

      lv_obj_t *instructLabel = lv_label_create(manualBox);
      lv_label_set_text(instructLabel, "Enter the exact network name (case-sensitive):");
      lv_obj_set_style_text_font(instructLabel, &lv_font_montserrat_12, 0);
      lv_obj_set_style_text_color(instructLabel, lv_color_hex(0xCCCCCC), 0);
      lv_obj_align(instructLabel, LV_ALIGN_TOP_MID, 0, 50);

      // SSID input textarea
      lv_obj_t *ssidInput = lv_textarea_create(manualBox);
      lv_obj_set_size(ssidInput, 400, 45);
      lv_obj_align(ssidInput, LV_ALIGN_TOP_MID, 0, 85);
      lv_obj_set_style_bg_color(ssidInput, lv_color_hex(0x000000), 0);
      lv_obj_set_style_border_width(ssidInput, 2, 0);
      lv_obj_set_style_border_color(ssidInput, lv_color_hex(0x49EACB), 0);
      lv_obj_set_style_text_color(ssidInput, lv_color_hex(0xFFFFFF), 0);
      lv_obj_set_style_bg_color(ssidInput, lv_color_hex(0xFFFFFF), LV_PART_CURSOR);
      lv_textarea_set_placeholder_text(ssidInput, "Network name...");
      lv_textarea_set_one_line(ssidInput, true);
      lv_obj_add_event_cb(ssidInput, [](lv_event_t *e) {
        if (e->code == LV_EVENT_CLICKED) {
          if (currentKeyboard) {
            lv_obj_del(currentKeyboard);
            currentKeyboard = nullptr;
          }
          lv_obj_t *textarea = lv_event_get_target(e);
          lv_color_t kaspa_green = lv_color_hex(0x49EACB);

          // Create on-screen keyboard with built-in layout
          currentKeyboard = lv_keyboard_create(lv_scr_act());
          lv_keyboard_set_textarea(currentKeyboard, textarea);
          lv_keyboard_set_mode(currentKeyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
          lv_obj_set_size(currentKeyboard, 800, 240);
          lv_obj_align(currentKeyboard, LV_ALIGN_BOTTOM_MID, 0, 0);

          // Apply terminal theme styling
          lv_obj_set_style_bg_color(currentKeyboard, lv_color_hex(0x000000), 0);
          lv_obj_set_style_border_width(currentKeyboard, 2, 0);
          lv_obj_set_style_border_color(currentKeyboard, kaspa_green, 0);
          lv_obj_set_style_radius(currentKeyboard, 0, 0);

          // Style individual keys
          lv_obj_set_style_bg_color(currentKeyboard, lv_color_hex(0x1a1a1a), LV_PART_ITEMS);
          lv_obj_set_style_text_color(currentKeyboard, kaspa_green, LV_PART_ITEMS);
          lv_obj_set_style_radius(currentKeyboard, 0, LV_PART_ITEMS);
          lv_obj_set_style_border_width(currentKeyboard, 1, LV_PART_ITEMS);
          lv_obj_set_style_border_color(currentKeyboard, kaspa_green, LV_PART_ITEMS);
          lv_obj_set_style_border_opa(currentKeyboard, LV_OPA_30, LV_PART_ITEMS);

          // Handle READY and CANCEL events
          lv_obj_add_event_cb(currentKeyboard, [](lv_event_t *e) {
            if (e->code == LV_EVENT_READY || e->code == LV_EVENT_CANCEL) {
              Serial.println("Keyboard closed");
              if (currentKeyboard) {
                lv_obj_del(currentKeyboard);
                currentKeyboard = nullptr;
                keyboardCloseTime = millis();
              }
            }
          }, LV_EVENT_ALL, NULL);
        }
      }, LV_EVENT_CLICKED, NULL);

      // OK button
      lv_obj_t *okBtn = lv_btn_create(manualBox);
      lv_obj_set_size(okBtn, 180, 45);
      lv_obj_align(okBtn, LV_ALIGN_BOTTOM_LEFT, 30, -20);
      lv_obj_set_style_radius(okBtn, 0, 0);
      lv_obj_set_style_bg_color(okBtn, lv_color_hex(0x49EACB), 0);
      lv_obj_set_style_border_width(okBtn, 0, 0);
      lv_obj_add_event_cb(okBtn, [](lv_event_t *e) {
        if (e->code == LV_EVENT_CLICKED) {
          playBeep(800, 50);
          lv_obj_t *box = lv_obj_get_parent(lv_event_get_target(e));
          lv_obj_t *ssidInput = lv_obj_get_child(box, 2); // Get the textarea
          const char *manualSSID = lv_textarea_get_text(ssidInput);

          if (strlen(manualSSID) > 0) {
            Serial.printf("Manual SSID entered: %s\n", manualSSID);
            // Set the SSID display label
            if (ssidDisplayLabel) {
              lv_label_set_text(ssidDisplayLabel, manualSSID);
            }
            // Store in global selected SSID
            selectedSSID = String(manualSSID);
          }

          // Close keyboard and dialog
          if (currentKeyboard) {
            lv_obj_del(currentKeyboard);
            currentKeyboard = nullptr;
          }
          lv_obj_del(box);
        }
      }, LV_EVENT_CLICKED, NULL);

      lv_obj_t *okLabel = lv_label_create(okBtn);
      lv_label_set_text(okLabel, "OK");
      lv_obj_set_style_text_font(okLabel, &lv_font_montserrat_16, 0);
      lv_obj_set_style_text_color(okLabel, lv_color_hex(0x000000), 0);
      lv_obj_center(okLabel);

      // Cancel button
      lv_obj_t *cancelBtn = lv_btn_create(manualBox);
      lv_obj_set_size(cancelBtn, 180, 45);
      lv_obj_align(cancelBtn, LV_ALIGN_BOTTOM_RIGHT, -30, -20);
      lv_obj_set_style_radius(cancelBtn, 0, 0);
      lv_obj_set_style_bg_color(cancelBtn, lv_color_hex(0xFF0000), 0);
      lv_obj_set_style_border_width(cancelBtn, 0, 0);
      lv_obj_add_event_cb(cancelBtn, [](lv_event_t *e) {
        if (e->code == LV_EVENT_CLICKED) {
          playBeep(800, 50);
          lv_obj_t *box = lv_obj_get_parent(lv_event_get_target(e));
          if (currentKeyboard) {
            lv_obj_del(currentKeyboard);
            currentKeyboard = nullptr;
          }
          lv_obj_del(box);
        }
      }, LV_EVENT_CLICKED, NULL);

      lv_obj_t *cancelLabel = lv_label_create(cancelBtn);
      lv_label_set_text(cancelLabel, "CANCEL");
      lv_obj_set_style_text_font(cancelLabel, &lv_font_montserrat_16, 0);
      lv_obj_set_style_text_color(cancelLabel, lv_color_hex(0xFFFFFF), 0);
      lv_obj_center(cancelLabel);
    }
  }, LV_EVENT_CLICKED, NULL);

  lv_obj_t *manualLabel = lv_label_create(manualEntryBtn);
  lv_label_set_text(manualLabel, "MANUAL ENTRY (Hidden Network)");
  lv_obj_set_style_text_font(manualLabel, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(manualLabel, kaspa_green, 0);
  lv_obj_set_style_text_color(manualLabel, lv_color_hex(0x000000), LV_STATE_PRESSED);
  lv_obj_center(manualLabel);

  // ========== RIGHT COLUMN - PASSWORD & CONTROLS ==========
  // Layout adjusted to keep password area above keyboard (keyboard is 240px at bottom)

  // Selected network display
  lv_obj_t *selectedLabel = lv_label_create(wifiConfigMenu);
  lv_label_set_text(selectedLabel, "> SELECTED");
  lv_obj_set_style_text_color(selectedLabel, kaspa_green, 0);
  lv_obj_set_style_text_font(selectedLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_pos(selectedLabel, 500, 55);

  // Display selected SSID (store in global variable)
  ssidDisplayLabel = lv_label_create(wifiConfigMenu);
  lv_label_set_text(ssidDisplayLabel, "None");
  lv_obj_set_style_text_color(ssidDisplayLabel, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(ssidDisplayLabel, &lv_font_montserrat_16, 0);
  lv_obj_set_pos(ssidDisplayLabel, 500, 75);
  lv_label_set_long_mode(ssidDisplayLabel, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(ssidDisplayLabel, 280);

  // Password section
  lv_obj_t *passLabel = lv_label_create(wifiConfigMenu);
  lv_label_set_text(passLabel, "> PASSWORD");
  lv_obj_set_style_text_color(passLabel, kaspa_green, 0);
  lv_obj_set_style_text_font(passLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_pos(passLabel, 500, 110);

  // Password input
  passwordTextarea = lv_textarea_create(wifiConfigMenu);
  lv_obj_set_size(passwordTextarea, 280, 45);
  lv_obj_set_pos(passwordTextarea, 500, 130);
  lv_obj_set_style_bg_color(passwordTextarea, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(passwordTextarea, 1, 0);
  lv_obj_set_style_border_color(passwordTextarea, kaspa_green, 0);
  lv_obj_set_style_border_opa(passwordTextarea, LV_OPA_30, 0);
  lv_obj_set_style_radius(passwordTextarea, 0, 0);
  lv_obj_set_style_text_color(passwordTextarea, lv_color_hex(0xFFFFFF), 0);
  // White cursor for visibility
  lv_obj_set_style_anim_time(passwordTextarea, 400, LV_PART_CURSOR);
  lv_obj_set_style_bg_color(passwordTextarea, lv_color_hex(0xFFFFFF), LV_PART_CURSOR);
  lv_obj_set_style_bg_opa(passwordTextarea, LV_OPA_COVER, LV_PART_CURSOR);
  lv_textarea_set_placeholder_text(passwordTextarea, "enter password...");
  lv_textarea_set_password_mode(passwordTextarea, true);  // Start HIDDEN with dots
  lv_textarea_set_one_line(passwordTextarea, true);
  lv_obj_add_event_cb(passwordTextarea, wifi_password_clicked_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(passwordTextarea, wifi_password_value_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

  // Show password toggle button
  lv_obj_t *toggleBtn = lv_btn_create(wifiConfigMenu);
  lv_obj_set_size(toggleBtn, 280, 35);
  lv_obj_set_pos(toggleBtn, 500, 185);
  lv_obj_set_style_radius(toggleBtn, 0, 0);
  lv_obj_set_style_bg_color(toggleBtn, lv_color_hex(0x1a1a1a), 0);
  lv_obj_set_style_border_width(toggleBtn, 1, 0);
  lv_obj_set_style_border_color(toggleBtn, kaspa_green, 0);
  lv_obj_set_style_border_opa(toggleBtn, LV_OPA_30, 0);
  lv_obj_add_event_cb(toggleBtn, password_toggle_cb, LV_EVENT_CLICKED, NULL);
  
  lv_obj_t *toggleLabel = lv_label_create(toggleBtn);
  lv_label_set_text(toggleLabel, "Show Password");
  lv_obj_set_style_text_font(toggleLabel, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(toggleLabel, kaspa_green, 0);
  lv_obj_center(toggleLabel);
  
  // Status message area
  lv_obj_t *statusMsgLabel = lv_label_create(wifiConfigMenu);
  lv_label_set_text(statusMsgLabel, "Select a network");
  lv_obj_set_style_text_color(statusMsgLabel, lv_color_hex(0x888888), 0);
  lv_obj_set_style_text_font(statusMsgLabel, &lv_font_montserrat_12, 0);
  lv_obj_set_pos(statusMsgLabel, 500, 280);
  lv_label_set_long_mode(statusMsgLabel, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(statusMsgLabel, 280);
  
  // Connect button (GREEN)
  lv_obj_t *connectBtn = lv_btn_create(wifiConfigMenu);
  lv_obj_set_size(connectBtn, 280, 50);
  lv_obj_set_pos(connectBtn, 500, 360);
  lv_obj_set_style_radius(connectBtn, 0, 0);
  lv_obj_set_style_bg_color(connectBtn, kaspa_green, 0);
  lv_obj_set_style_border_width(connectBtn, 0, 0);
  lv_obj_set_style_shadow_width(connectBtn, 0, 0);
  // Button press effect
  lv_obj_set_style_bg_color(connectBtn, lv_color_hex(0x000000), LV_STATE_PRESSED);
  lv_obj_set_style_border_width(connectBtn, 2, LV_STATE_PRESSED);
  lv_obj_set_style_border_color(connectBtn, kaspa_green, LV_STATE_PRESSED);
  lv_obj_add_event_cb(connectBtn, wifi_connect_btn_cb, LV_EVENT_CLICKED, NULL);
  
  lv_obj_t *connectLabel = lv_label_create(connectBtn);
  lv_label_set_text(connectLabel, "CONNECT");
  lv_obj_set_style_text_font(connectLabel, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(connectLabel, lv_color_hex(0x000000), 0);
  lv_obj_center(connectLabel);
  
  // Cancel button (RED)
  lv_obj_t *cancelBtn = lv_btn_create(wifiConfigMenu);
  lv_obj_set_size(cancelBtn, 280, 50);
  lv_obj_set_pos(cancelBtn, 500, 420);
  lv_obj_set_style_radius(cancelBtn, 0, 0);
  lv_obj_set_style_bg_color(cancelBtn, lv_color_hex(0xFF0000), 0);
  lv_obj_set_style_border_width(cancelBtn, 0, 0);
  lv_obj_set_style_shadow_width(cancelBtn, 0, 0);
  lv_obj_add_event_cb(cancelBtn, close_wifi_config_cb, LV_EVENT_CLICKED, NULL);
  
  lv_obj_t *cancelLabel = lv_label_create(cancelBtn);
  lv_label_set_text(cancelLabel, "CANCEL");
  lv_obj_set_style_text_font(cancelLabel, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(cancelLabel, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(cancelLabel);
}

// ==================== SD CARD HTML HELPERS ====================
// Helper function to load HTML from SD card (saves flash space)
String loadHTMLFromSD(const char* filename) {
  File file = SD.open(filename);
  if (!file) {
    Serial.printf("⚠️  Failed to open %s from SD card\n", filename);
    return "<html><body><h1>Error: HTML file not found on SD card</h1><p>Please ensure " + String(filename) + " exists on the SD card.</p></body></html>";
  }

  String content = file.readString();
  file.close();
  return content;
}

// Helper to serve HTML from SD card with dynamic placeholders replaced
void serveHTMLFromSD(const char* filename) {
  String html = loadHTMLFromSD(filename);

  // Replace placeholders with actual values
  html.replace("{{VERSION}}", FIRMWARE_VERSION);
  html.replace("{{MINING_STATUS}}", miningEnabled ? "RUNNING" : "STOPPED");
  html.replace("{{HASHRATE}}", String(currentHashrate, 2));
  html.replace("{{JOBS}}", String(jobsReceived));
  html.replace("{{SUBMITTED}}", String(sharesSubmitted));
  html.replace("{{ACCEPTED}}", String(sharesAccepted));
  html.replace("{{REJECTED}}", String(sharesRejected));
  html.replace("{{BLOCKS}}", String(blocksFound));

  server.send(200, "text/html", html);
}
// =================================================================
// API endpoint for dashboard stats (JSON format)
// =================================================================
void handleApiStats() {
  // Calculate device uptime (since boot)
  unsigned long uptimeSeconds = millis() / 1000;
  unsigned long uptimeDays = uptimeSeconds / 86400;
  unsigned long uptimeHours = (uptimeSeconds % 86400) / 3600;
  unsigned long uptimeMinutes = (uptimeSeconds % 3600) / 60;

  String uptimeStr;
  if (uptimeDays > 0) {
    uptimeStr = String(uptimeDays) + "d " + String(uptimeHours) + "h " + String(uptimeMinutes) + "m";
  } else if (uptimeHours > 0) {
    uptimeStr = String(uptimeHours) + "h " + String(uptimeMinutes) + "m";
  } else {
    uptimeStr = String(uptimeMinutes) + "m";
  }

  String json = "{";
  json += "\"mining\":" + String(miningEnabled ? "true" : "false") + ",";
  json += "\"poolConnected\":" + String(stratumClient.connected() ? "true" : "false") + ",";
  json += "\"hashrate\":\"" + String(currentHashrate, 2) + "\",";
  json += "\"submitted\":\"" + String(sharesSubmitted) + "\",";
  json += "\"accepted\":\"" + String(sharesAccepted) + "\",";
  json += "\"rejected\":\"" + String(sharesRejected) + "\",";
  json += "\"blocks\":\"" + String(blocksFound) + "\",";
  json += "\"miningStarted\":\"" + miningStartedTimestamp + "\",";
  json += "\"uptime\":\"" + uptimeStr + "\",";
  json += "\"wifi\":\"" + String(WiFi.SSID()) + "\",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"pool\":\"" + minerPoolUrl + "\",";
  json += "\"price\":\"" + price_usd + "\",";
  json += "\"change\":\"" + change_24h + "\",";
  json += "\"netHashrate\":\"" + String(networkHashrate, 2) + "\",";
  json += "\"marketCap\":\"" + market_cap + "\",";
  json += "\"version\":\"" + String(FIRMWARE_VERSION) + "\"";

  // Add hashrate history (last 5 entries)
  json += ",\"hashrateHistory\":[";
  int historyCount = 0;
  for (int i = 4; i >= 0; i--) {  // Show last 5 entries
    if (i < HASHRATE_POINTS) {
      int idx = (hashrateIndex - i - 1 + HASHRATE_POINTS) % HASHRATE_POINTS;
      if (hashrateHistory[idx] > 0) {
        if (historyCount > 0) json += ",";

        // Calculate time ago
        int minsAgo = i * 5;  // Assuming 5-min intervals
        String timeStr = String(minsAgo) + "m ago";
        if (minsAgo == 0) timeStr = "now";

        json += "{\"time\":\"" + timeStr + "\",\"hashrate\":\"" + String(hashrateHistory[idx], 2) + "\"}";
        historyCount++;
      }
    }
  }
  json += "]";

  // Add miner hashrate history (last 6 entries)
  json += ",\"minerHashrateHistory\":[";
  int minerHistoryCount = 0;
  for (int i = 5; i >= 0; i--) {  // Show last 6 entries
    if (i < MINER_HASHRATE_POINTS) {
      int idx = (minerHashrateIndex - i - 1 + MINER_HASHRATE_POINTS) % MINER_HASHRATE_POINTS;
      if (minerHashrateHistory[idx] > 0) {
        if (minerHistoryCount > 0) json += ",";

        // Calculate time ago
        int minsAgo = i * 5;  // Assuming 5-min intervals
        String timeStr = String(minsAgo) + "m ago";
        if (minsAgo == 0) timeStr = "now";

        json += "{\"time\":\"" + timeStr + "\",\"hashrate\":\"" + String(minerHashrateHistory[idx], 2) + "\",\"unit\":\"H/s\"}";
        minerHistoryCount++;
      }
    }
  }
  json += "]";

  json += "}";
  server.send(200, "application/json", json);
}

// =================================================================
// AUTHENTICATION ENDPOINTS
// =================================================================

// Serve login page HTML
void handleLogin() {
  String html = R"rawliteral(<!DOCTYPE html>
<html>
<head>
<title>KASDeck Login</title>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
*{margin:0;padding:0;box-sizing:border-box;}
body{font-family:'Segoe UI',sans-serif;background:#0a0e27;color:#e0e0e0;display:flex;align-items:center;justify-content:center;min-height:100vh;padding:20px;}
.login-card{background:linear-gradient(145deg,#16213e,#1a1a2e);padding:40px;border-radius:15px;border:2px solid #49EACB;max-width:400px;width:100%;box-shadow:0 8px 32px rgba(0,0,0,0.3);}
h1{color:#49EACB;text-align:center;margin-bottom:10px;font-size:2em;}
.subtitle{color:#888;text-align:center;margin-bottom:30px;}
.form-group{margin-bottom:20px;}
.form-group label{display:block;color:#aaa;margin-bottom:8px;font-weight:bold;}
.form-group input{width:100%;padding:12px;border:2px solid #49EACB;background:#0a0e27;color:#e0e0e0;border-radius:8px;font-size:1em;}
.form-group input:focus{outline:none;border-color:#3dd4b5;box-shadow:0 0 10px rgba(73,234,203,0.3);}
.btn{width:100%;padding:15px;border:none;border-radius:8px;cursor:pointer;font-weight:bold;font-size:1.1em;background:#49EACB;color:#0a0e27;transition:all 0.3s;}
.btn:hover{background:#3dd4b5;transform:translateY(-2px);}
.error{background:rgba(255,76,76,0.2);color:#ff4c4c;padding:12px;border-radius:8px;margin-bottom:20px;display:none;text-align:center;}
</style>
</head>
<body>
<div class="login-card">
<h1>KASDeck</h1>
<div class="subtitle">Enter your password to access the dashboard</div>
<div class="error" id="error">Invalid password</div>
<form onsubmit="login(event)">
<div class="form-group">
<label>Password</label>
<input type="password" id="password" placeholder="Enter device password" required autofocus>
</div>
<button type="submit" class="btn">Login</button>
</form>
</div>
<script>
function login(e){
  e.preventDefault();
  const pwd=document.getElementById('password').value;
  fetch('/api/auth',{
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({password:pwd})
  })
  .then(r=>r.json())
  .then(d=>{
    if(d.success){
      localStorage.setItem('authToken',d.token);
      document.cookie='authToken='+d.token+';path=/;max-age=86400';
      const params=new URLSearchParams(window.location.search);
      const returnUrl=params.get('return')||'/';
      window.location.href=returnUrl;
    }else{
      document.getElementById('error').style.display='block';
      document.getElementById('password').value='';
    }
  })
  .catch(()=>{
    document.getElementById('error').textContent='Connection error';
    document.getElementById('error').style.display='block';
  });
}
</script>
</body>
</html>)rawliteral";
  server.send(200, "text/html", html);
}

// Serve password setup page HTML (first-time setup)
void handleSetupPassword() {
  // If password already set, redirect to login
  if (isPasswordSet()) {
    redirectToLogin();
    return;
  }

  String html = R"rawliteral(<!DOCTYPE html>
<html>
<head>
<title>KASDeck - Setup Password</title>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
*{margin:0;padding:0;box-sizing:border-box;}
body{font-family:'Segoe UI',sans-serif;background:#0a0e27;color:#e0e0e0;display:flex;align-items:center;justify-content:center;min-height:100vh;padding:20px;}
.setup-card{background:linear-gradient(145deg,#16213e,#1a1a2e);padding:40px;border-radius:15px;border:2px solid #49EACB;max-width:450px;width:100%;box-shadow:0 8px 32px rgba(0,0,0,0.3);}
h1{color:#49EACB;text-align:center;margin-bottom:10px;font-size:2em;}
.subtitle{color:#888;text-align:center;margin-bottom:30px;line-height:1.5;}
.form-group{margin-bottom:20px;}
.form-group label{display:block;color:#aaa;margin-bottom:8px;font-weight:bold;}
.form-group input{width:100%;padding:12px;border:2px solid #49EACB;background:#0a0e27;color:#e0e0e0;border-radius:8px;font-size:1em;}
.form-group input:focus{outline:none;border-color:#3dd4b5;}
.btn{width:100%;padding:15px;border:none;border-radius:8px;cursor:pointer;font-weight:bold;font-size:1.1em;background:#49EACB;color:#0a0e27;transition:all 0.3s;}
.btn:hover{background:#3dd4b5;transform:translateY(-2px);}
.error{background:rgba(255,76,76,0.2);color:#ff4c4c;padding:12px;border-radius:8px;margin-bottom:20px;display:none;text-align:center;}
.info{background:rgba(73,234,203,0.1);border-left:4px solid #49EACB;padding:15px;margin-bottom:20px;border-radius:0 8px 8px 0;font-size:0.9em;color:#aaa;}
</style>
</head>
<body>
<div class="setup-card">
<h1>Welcome to KASDeck</h1>
<div class="subtitle">Set a password to secure your device</div>
<div class="info">This password will protect the web dashboard. Choose a strong password that you will remember.</div>
<div class="error" id="error">Error setting password</div>
<form onsubmit="setupPassword(event)">
<div class="form-group">
<label>New Password</label>
<input type="password" id="password" placeholder="Enter password (min 6 characters)" required minlength="6" autofocus>
</div>
<div class="form-group">
<label>Confirm Password</label>
<input type="password" id="confirm" placeholder="Confirm password" required minlength="6">
</div>
<button type="submit" class="btn">Set Password & Continue</button>
</form>
</div>
<script>
function setupPassword(e){
  e.preventDefault();
  const pwd=document.getElementById('password').value;
  const confirm=document.getElementById('confirm').value;
  if(pwd!==confirm){
    document.getElementById('error').textContent='Passwords do not match';
    document.getElementById('error').style.display='block';
    return;
  }
  if(pwd.length<6){
    document.getElementById('error').textContent='Password must be at least 6 characters';
    document.getElementById('error').style.display='block';
    return;
  }
  fetch('/api/auth/setup',{
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({password:pwd})
  })
  .then(r=>r.json())
  .then(d=>{
    if(d.success){
      localStorage.setItem('authToken',d.token);
      document.cookie='authToken='+d.token+';path=/;max-age=86400';
      window.location.href='/';
    }else{
      document.getElementById('error').textContent=d.message||'Error setting password';
      document.getElementById('error').style.display='block';
    }
  })
  .catch(()=>{
    document.getElementById('error').textContent='Connection error';
    document.getElementById('error').style.display='block';
  });
}
</script>
</body>
</html>)rawliteral";
  server.send(200, "text/html", html);
}

// API: Authenticate with password
void handleApiAuth() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"No data received\"}");
    return;
  }

  String body = server.arg("plain");

  // Parse password from JSON
  int pwdStart = body.indexOf("\"password\":\"");
  if (pwdStart < 0) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"Password required\"}");
    return;
  }
  pwdStart += 12;
  int pwdEnd = body.indexOf("\"", pwdStart);
  String password = body.substring(pwdStart, pwdEnd);

  // Get stored password (uses default if not set)
  String storedPassword = getDevicePassword();

  if (password == storedPassword) {
    // Generate and store session token
    String token = generateToken();
    addSessionToken(token);
    Serial.printf("Auth: Login successful, token issued. Active sessions: %d\n", activeTokenCount);
    server.send(200, "application/json", "{\"success\":true,\"token\":\"" + token + "\"}");
  } else {
    Serial.println("Auth: Login failed - invalid password");
    server.send(200, "application/json", "{\"success\":false,\"message\":\"Invalid password\"}");
  }
}

// API: Check authentication status
void handleApiAuthStatus() {
  bool authenticated = checkAuth();
  bool passwordSet = isPasswordSet();
  String json = "{\"authenticated\":" + String(authenticated ? "true" : "false");
  json += ",\"passwordSet\":" + String(passwordSet ? "true" : "false") + "}";
  server.send(200, "application/json", json);
}

// API: First-time password setup
void handleApiAuthSetup() {
  // Only allow if password not already set
  if (isPasswordSet()) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"Password already set\"}");
    return;
  }

  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"No data received\"}");
    return;
  }

  String body = server.arg("plain");

  // Parse password from JSON
  int pwdStart = body.indexOf("\"password\":\"");
  if (pwdStart < 0) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"Password required\"}");
    return;
  }
  pwdStart += 12;
  int pwdEnd = body.indexOf("\"", pwdStart);
  String password = body.substring(pwdStart, pwdEnd);

  if (password.length() < 6) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"Password must be at least 6 characters\"}");
    return;
  }

  // Store password in NVS
  Preferences prefs;
  prefs.begin("kaspa", false);
  prefs.putString("devicePassword", password);
  prefs.end();

  // Generate session token for immediate login
  String token = generateToken();
  addSessionToken(token);

  Serial.println("Auth: Device password set successfully");
  server.send(200, "application/json", "{\"success\":true,\"token\":\"" + token + "\"}");
}

// API: Change password (requires current password)
void handleApiAuthChangePassword() {
  // Require authentication
  if (!requireAuth()) return;

  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"No data received\"}");
    return;
  }

  String body = server.arg("plain");

  // Parse current password
  int currentStart = body.indexOf("\"currentPassword\":\"");
  if (currentStart < 0) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"Current password required\"}");
    return;
  }
  currentStart += 19;
  int currentEnd = body.indexOf("\"", currentStart);
  String currentPassword = body.substring(currentStart, currentEnd);

  // Verify current password
  String storedPassword = getDevicePassword();
  if (currentPassword != storedPassword) {
    server.send(200, "application/json", "{\"success\":false,\"message\":\"Current password is incorrect\"}");
    return;
  }

  // Parse new password
  int newStart = body.indexOf("\"newPassword\":\"");
  if (newStart < 0) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"New password required\"}");
    return;
  }
  newStart += 15;
  int newEnd = body.indexOf("\"", newStart);
  String newPassword = body.substring(newStart, newEnd);

  if (newPassword.length() < 6) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"Password must be at least 6 characters\"}");
    return;
  }

  // Save new password
  Preferences prefs;
  prefs.begin("kaspa", false);
  prefs.putString("devicePassword", newPassword);
  prefs.end();

  Serial.println("Auth: Device password changed successfully");
  addLog("Web UI password changed", "info");
  server.send(200, "application/json", "{\"success\":true,\"message\":\"Password changed successfully\"}");
}

// API: Reset password to default (requires current password)
void handleApiAuthResetPassword() {
  // Require authentication
  if (!requireAuth()) return;

  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"No data received\"}");
    return;
  }

  String body = server.arg("plain");

  // Parse current password for verification
  int currentStart = body.indexOf("\"currentPassword\":\"");
  if (currentStart < 0) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"Current password required\"}");
    return;
  }
  currentStart += 19;
  int currentEnd = body.indexOf("\"", currentStart);
  String currentPassword = body.substring(currentStart, currentEnd);

  // Verify current password
  String storedPassword = getDevicePassword();
  if (currentPassword != storedPassword) {
    server.send(200, "application/json", "{\"success\":false,\"message\":\"Current password is incorrect\"}");
    return;
  }

  // Remove custom password (will use default)
  Preferences prefs;
  prefs.begin("kaspa", false);
  prefs.remove("devicePassword");
  prefs.end();

  Serial.println("Auth: Device password reset to default");
  addLog("Web UI password reset to default", "info");
  server.send(200, "application/json", "{\"success\":true,\"message\":\"Password reset to default (kaspa123)\"}");
}

// API: Logout (invalidate session)
void handleApiAuthLogout() {
  // Get token from header or cookie
  String token = "";
  if (server.hasHeader("X-Auth-Token")) {
    token = server.header("X-Auth-Token");
  } else if (server.hasHeader("Cookie")) {
    String cookies = server.header("Cookie");
    int tokenStart = cookies.indexOf("authToken=");
    if (tokenStart >= 0) {
      tokenStart += 10;
      int tokenEnd = cookies.indexOf(";", tokenStart);
      if (tokenEnd < 0) tokenEnd = cookies.length();
      token = cookies.substring(tokenStart, tokenEnd);
    }
  }

  if (token.length() > 0) {
    removeSessionToken(token);
    Serial.printf("Auth: Logout successful. Active sessions: %d\n", activeTokenCount);
  }

  server.send(200, "application/json", "{\"success\":true}");
}

// =================================================================
// END AUTHENTICATION ENDPOINTS
// =================================================================

// API endpoint to get wallet/stratum configuration
void handleApiGetConfig() {
  Preferences prefs;
  prefs.begin("kaspa", true);
  String wallet = prefs.getString("wallet", "");
  String stratum = prefs.getString("pool", "");  // Fixed: use "pool" key to match save
  String worker = prefs.getString("worker", "worker1");
  String stratumPassword = prefs.getString("stratumPassword", "x");
  prefs.end();

  String json = "{";
  json += "\"wallet\":\"" + wallet + "\",";
  json += "\"stratum\":\"" + stratum + "\",";
  json += "\"worker\":\"" + worker + "\",";
  json += "\"password\":\"" + stratumPassword + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

// API endpoint to save wallet/stratum configuration
void handleApiPostConfig() {
  // Require authentication for config changes
  if (!requireAuth()) return;

  if (server.hasArg("plain")) {
    String body = server.arg("plain");

    // Simple JSON parsing (assumes proper format)
    int walletStart = body.indexOf("\"wallet\":\"") + 10;
    int walletEnd = body.indexOf("\"", walletStart);
    String wallet = body.substring(walletStart, walletEnd);

    int stratumStart = body.indexOf("\"stratum\":\"") + 11;
    int stratumEnd = body.indexOf("\"", stratumStart);
    String stratum = body.substring(stratumStart, stratumEnd);

    int workerStart = body.indexOf("\"worker\":\"") + 10;
    int workerEnd = body.indexOf("\"", workerStart);
    String worker = body.substring(workerStart, workerEnd);

    int passwordStart = body.indexOf("\"password\":\"") + 12;
    int passwordEnd = body.indexOf("\"", passwordStart);
    String stratumPassword = body.substring(passwordStart, passwordEnd);

    // Save to preferences (use "pool" key to match what connectToPool reads)
    Preferences prefs;
    prefs.begin("kaspa", false);
    prefs.putString("wallet", wallet);
    prefs.putString("pool", stratum);  // Fixed: use "pool" key, not "stratum"
    prefs.putString("worker", worker);
    prefs.putString("stratumPassword", stratumPassword);
    prefs.end();

    // Update in-memory variables immediately
    minerWalletAddress = wallet;
    minerPoolUrl = stratum;

    Serial.println("Configuration updated:");
    Serial.printf("  Wallet: %s\n", maskWallet(wallet).c_str());
    Serial.printf("  Stratum: %s\n", stratum.c_str());
    Serial.printf("  Worker: %s\n", worker.c_str());

    addLog("Configuration saved: " + stratum, "success");

    server.send(200, "application/json", "{\"message\":\"Configuration saved. Reconnect to pool to apply changes.\"}");
  } else {
    server.send(400, "application/json", "{\"error\":\"No data received\"}");
  }
}

// System logs ring buffer
#define SYS_LOG_BUFFER_SIZE 15  // Keep last 15 log entries
String sysLogBuffer[SYS_LOG_BUFFER_SIZE];
int sysLogIndex = 0;
int sysLogCount = 0;

// Function to add log entry
void addLog(String message, String type = "") {
  // Format timestamp with actual date/time if NTP synced, otherwise use uptime
  String timestamp;
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char timeStr[20];
    strftime(timeStr, sizeof(timeStr), "%m-%d %H:%M:%S", &timeinfo);
    timestamp = String(timeStr);
  } else {
    // Fallback to uptime seconds if NTP not synced
    timestamp = String(millis() / 1000) + "s";
  }

  Serial.println("[" + timestamp + "] " + message);

  // Store in ring buffer for web UI
  sysLogBuffer[sysLogIndex] = "[" + timestamp + "] " + message;
  sysLogIndex = (sysLogIndex + 1) % SYS_LOG_BUFFER_SIZE;
  if (sysLogCount < SYS_LOG_BUFFER_SIZE) sysLogCount++;
}

// API endpoint to get system logs
void handleApiLogs() {
  String json = "{\"logs\":[";

  int startIdx = (sysLogCount < SYS_LOG_BUFFER_SIZE) ? 0 : sysLogIndex;
  for (int i = 0; i < min(sysLogCount, SYS_LOG_BUFFER_SIZE); i++) {
    int idx = (startIdx + i) % SYS_LOG_BUFFER_SIZE;
    if (sysLogBuffer[idx].length() > 0) {
      if (i > 0) json += ",";

      // Detect log type based on message content
      String logType = "";
      String message = sysLogBuffer[idx];
      if (message.indexOf("ERROR") >= 0 || message.indexOf("Failed") >= 0 || message.indexOf("failed") >= 0) {
        logType = "error";
      } else if (message.indexOf("Connected") >= 0 || message.indexOf("Success") >= 0 || message.indexOf("successful") >= 0) {
        logType = "success";
      }

      json += "{\"message\":\"" + message + "\"";
      if (logType.length() > 0) {
        json += ",\"type\":\"" + logType + "\"";
      }
      json += "}";
    }
  }

  json += "]}";
  server.send(200, "application/json", json);
}

// =================================================================
// Serve logo from SD card
// =================================================================
void handleLogo() {
  String path = server.uri();  // Get the requested path

  if (SD.exists(path.c_str())) {
    File file = SD.open(path.c_str());
    if (file) {
      server.streamFile(file, "image/png");
      file.close();
      return;
    }
  }

  server.send(404, "text/plain", "Logo not found on SD card");
}

void handleDebugLog() {
  if (SD.exists("/debug.txt")) {
    File file = SD.open("/debug.txt");
    if (file) {
      server.streamFile(file, "text/plain");
      file.close();
      return;
    }
  }
  server.send(404, "text/plain", "Debug log not found");
}

// =================================================================
// Main dashboard page - loads from SD card
// =================================================================
void handleRoot() {
  // Require authentication for dashboard
  if (!requireAuth()) return;

  // Try to load HTML from SD card first
  if (SD.exists("/dashboard.html")) {
    File file = SD.open("/dashboard.html");
    if (file) {
      server.streamFile(file, "text/html");
      file.close();
      return;
    }
  }

  // Fallback: minimal error page if SD card not found
  String html = "<!DOCTYPE html><html><head><title>KASDeck - SD Card Required</title>";
  html += "<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<style>body{font-family:'Segoe UI',sans-serif;background:#0a0e27;color:#e0e0e0;margin:0;padding:40px;text-align:center;}";
  html += "h1{color:#ff4c4c;font-size:2.5em;}p{color:#aaa;font-size:1.2em;margin:20px 0;}";
  html += ".card{max-width:600px;margin:40px auto;background:#16213e;padding:30px;border-radius:15px;border:2px solid #ff4c4c;}</style></head><body>";
  html += "<div class='card'><h1>⚠️ SD Card Required</h1>";
  html += "<p>Please insert an SD card with the required files in the root directory.</p>";
  html += "<p>The web interface files have been moved to SD card to save flash memory for firmware updates.</p>";
  html += "<p style='margin-top:30px;'><a href='/upload' style='color:#49EACB;text-decoration:none;font-weight:bold;padding:10px 20px;background:#49EACB;color:#000;border-radius:5px;'>📁 Upload Files to SD Card</a></p>";
  html += "<p style='margin-top:15px;'><a href='/update' style='color:#49EACB;text-decoration:none;font-weight:bold;'>or go to Firmware Update Page</a></p></div></body></html>";
  server.send(503, "text/html", html);
}

void handleOTAPage() {
  // Require authentication for firmware update page
  if (!requireAuth()) return;

  // Try to load OTA page from SD card
  if (SD.exists("/ota.html")) {
    File file = SD.open("/ota.html");
    if (file) {
      server.streamFile(file, "text/html");
      file.close();
      return;
    }
  }

  // Fallback: minimal error page if SD card not found
  String html = "<!DOCTYPE html><html><head><title>OTA Update - SD Card Required</title>";
  html += "<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<style>body{font-family:'Segoe UI',sans-serif;background:#0a0e27;color:#e0e0e0;margin:0;padding:40px;text-align:center;}";
  html += "h1{color:#ff4c4c;font-size:2.5em;}p{color:#aaa;font-size:1.2em;margin:20px 0;}</style></head><body>";
  html += "<h1>⚠️ SD Card Required</h1><p>OTA update page requires SD card with ota.html file.</p></body></html>";
  server.send(503, "text/html", html);
}


// Validation function for Kaspa wallet address
bool isValidKaspaAddress(String address) {
  // Kaspa addresses start with "kaspa:"
  if (!address.startsWith("kaspa:")) {
    return false;
  }
  
  // Remove the "kaspa:" prefix for length check
  String addr = address.substring(6);
  
  // Kaspa addresses should be 61-63 characters after the prefix
  if (addr.length() < 61 || addr.length() > 63) {
    return false;
  }
  
  // Check if it contains only valid characters (lowercase alphanumeric)
  // Kaspa uses bech32 encoding which allows all lowercase letters and digits
  for (unsigned int i = 0; i < addr.length(); i++) {
    char c = addr.charAt(i);
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z'))) {
      return false;
    }
  }
  
  return true;
}

// Validation function for pool URL
bool isValidPoolUrl(String url) {
  // Remove protocol if present
  url.replace("stratum+tcp://", "");
  url.trim();
  
  // Must have a colon separating host and port
  int colonIndex = url.indexOf(':');
  if (colonIndex == -1) {
    return false;
  }
  
  String host = url.substring(0, colonIndex);
  String portStr = url.substring(colonIndex + 1);
  
  // Host should not be empty
  if (host.length() == 0) {
    return false;
  }
  
  // Port should be a valid number between 1 and 65535
  int port = portStr.toInt();
  if (port < 1 || port > 65535) {
    return false;
  }
  
  // Check if host is valid IP or hostname
  // For IP: should have 3 dots and valid numbers
  int dotCount = 0;
  for (unsigned int i = 0; i < host.length(); i++) {
    if (host.charAt(i) == '.') dotCount++;
  }
  
  if (dotCount == 3) {
    // Looks like an IP, validate octets
    int octetCount = 0;
    int start = 0;
    for (unsigned int i = 0; i <= host.length(); i++) {
      if (i == host.length() || host.charAt(i) == '.') {
        String octet = host.substring(start, i);
        int val = octet.toInt();
        if (val < 0 || val > 255 || (octet != "0" && octet.charAt(0) == '0')) {
          return false;
        }
        octetCount++;
        start = i + 1;
      }
    }
    if (octetCount != 4) return false;
  } else {
    // Hostname: should contain only valid characters
    for (unsigned int i = 0; i < host.length(); i++) {
      char c = host.charAt(i);
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || 
            (c >= '0' && c <= '9') || c == '.' || c == '-')) {
        return false;
      }
    }
  }
  
  return true;
}

// Show error page with validation message
void showErrorPage(String errorTitle, String errorMessage) {
  String html = "<!DOCTYPE html><html><head><title>Validation Error</title>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>";
  html += "body{font-family:'Segoe UI',sans-serif; background:#0a0e27; color:#e0e0e0; display:flex; align-items:center; justify-content:center; min-height:100vh; margin:0; padding:20px;}";
  html += ".error-box{background:linear-gradient(145deg,#16213e,#1a1a2e); padding:40px; border-radius:20px; border:3px solid #ff4c4c; text-align:center; max-width:500px; box-shadow:0 10px 30px rgba(255,76,76,0.3);}";
  html += ".error-icon{width:80px; height:80px; border-radius:50%; background:#ff4c4c; margin:0 auto 20px; display:flex; align-items:center; justify-content:center; font-size:50px; color:white;}";
  html += "h1{color:#ff4c4c; margin:20px 0;}";
  html += "p{color:#aaa; margin:15px 0; line-height:1.6;}";
  html += ".error-details{background:#0a0e27; padding:20px; border-radius:10px; margin-top:20px; text-align:left;}";
  html += ".error-message{color:#ff9999; font-weight:bold; margin-bottom:15px;}";
  html += ".help-text{color:#888; font-size:0.9em; margin-top:10px;}";
  html += "a{display:inline-block; margin-top:20px; padding:12px 30px; background:#49EACB; color:#0a0e27; text-decoration:none; font-weight:bold; border-radius:8px;}";
  html += "a:hover{background:#3dd4b5;}";
  html += "</style></head><body>";
  html += "<div class='error-box'>";
  html += "<div class='error-icon'>!</div>";
  html += "<h1>" + errorTitle + "</h1>";
  html += "<p>Your settings could not be saved due to validation errors.</p>";
  html += "<div class='error-details'>";
  html += "<div class='error-message'>" + errorMessage + "</div>";
  html += "</div>";
  html += "<a href='/'>Back to Portal</a>";
  html += "</div></body></html>";
  
  server.send(400, "text/html; charset=UTF-8", html);
}

void handleSave() {
  // Require authentication for settings changes
  if (!requireAuth()) return;

  String newWallet = server.arg("wallet");
  String newPool = server.arg("pool");
  
  // Trim whitespace from inputs
  newWallet.trim();
  newPool.trim();
  
  // Validate wallet address
  if (newWallet.length() > 0 && !isValidKaspaAddress(newWallet)) {
    String errorMsg = "Invalid Kaspa wallet address format.<br><br>";
    errorMsg += "<div class='help-text'>";
    errorMsg += "Valid format: kaspa:qpxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx<br>";
    errorMsg += "• Must start with 'kaspa:'<br>";
    errorMsg += "• Must be 61-63 characters after the prefix<br>";
    errorMsg += "• Only valid base58 characters allowed<br><br>";
    errorMsg += "Your address length: " + String(newWallet.length()) + " total, " + String(newWallet.substring(6).length()) + " after prefix";
    errorMsg += "</div>";
    showErrorPage("Invalid Wallet Address", errorMsg);
    return;
  }
  
  // Validate pool URL
  if (newPool.length() > 0 && !isValidPoolUrl(newPool)) {
    String errorMsg = "Invalid pool URL format.<br><br>";
    errorMsg += "<div class='help-text'>";
    errorMsg += "Valid formats:<br>";
    errorMsg += "• 192.168.1.100:5555<br>";
    errorMsg += "• pool.example.com:5555<br>";
    errorMsg += "• Must include port number (1-65535)";
    errorMsg += "</div>";
    showErrorPage("Invalid Pool URL", errorMsg);
    return;
  }
  
  // Validation passed - save settings
  if (server.hasArg("wallet")) minerWalletAddress = newWallet;
  if (server.hasArg("pool")) minerPoolUrl = newPool;
  
  // Save timezone if provided
  if (server.hasArg("timezone")) {
    int newTimezone = server.arg("timezone").toInt();
    if (newTimezone >= -12 && newTimezone <= 12) {
      timezoneOffsetHours = newTimezone;
      Serial.printf("Timezone updated to UTC%+d\n", timezoneOffsetHours);
      
      // Reconfigure NTP with new timezone
      int timezoneOffsetSeconds = timezoneOffsetHours * 3600;
      configTime(timezoneOffsetSeconds, 3600, ntpServers[0]);
    }
  }

  Preferences prefs;
  prefs.begin("kaspa", false);
  prefs.putString("wallet", minerWalletAddress);
  prefs.putString("pool", minerPoolUrl);
  prefs.putInt("timezone", timezoneOffsetHours);
  prefs.end();

  // Try to load saved confirmation page from SD card
  if (SD.exists("/saved.html")) {
    // Redirect to saved.html with query parameters
    String redirectUrl = "/saved.html?wallet=" + String(minerWalletAddress.substring(0, 20)) + "...&pool=" + minerPoolUrl;
    server.sendHeader("Location", redirectUrl);
    server.send(302, "text/plain", "");
    showSaveConfirmation();
    return;
  }

  // Fallback: simple success message
  String html = "<!DOCTYPE html><html><head><title>Saved</title>";
  html += "<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='2;url=/'>";
  html += "<style>body{font-family:'Segoe UI',sans-serif;background:#0a0e27;color:#4caf50;display:flex;align-items:center;justify-content:center;height:100vh;margin:0;text-align:center;}</style>";
  html += "</head><body><h1>✓ Settings Saved!</h1><p>Redirecting...</p></body></html>";
  server.send(200, "text/html; charset=UTF-8", html);

  // Show confirmation on device screen too
  showSaveConfirmation();
}

// =================================================================
// FILE UPLOAD TO SD CARD - Web interface for uploading files
// =================================================================

// Global variable for file upload
File uploadFile;

// Whitelist of allowed files for SD card upload
const char* ALLOWED_FILES[] = {
  "dashboard.html",
  "ota.html",
  "kaspa_logo.bin",
  "kasdeck_logo.bin",
  "pop_logo.bin",
  "update.bin",       // Firmware updates (saved to /firmware/update.bin)
  NULL  // Sentinel
};

bool isFileAllowed(const String& filename) {
  // Extract just the filename (remove path)
  String name = filename;
  int lastSlash = name.lastIndexOf('/');
  if (lastSlash >= 0) {
    name = name.substring(lastSlash + 1);
  }

  // Check against whitelist
  for (int i = 0; ALLOWED_FILES[i] != NULL; i++) {
    if (name.equalsIgnoreCase(ALLOWED_FILES[i])) {
      return true;
    }
  }
  return false;
}

// Track if current upload is rejected
bool uploadRejected = false;
String rejectedFilename = "";

// Show file upload form
void handleFileUpload() {
  // Require authentication for file uploads
  if (!requireAuth()) return;

  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>File Upload - KASDeck</title>";
  html += "<style>";
  html += "body{font-family:'Segoe UI',sans-serif;background:linear-gradient(135deg,#1a1a2e,#16213e);color:#fff;margin:0;padding:20px;min-height:100vh;}";
  html += ".container{max-width:800px;margin:0 auto;background:rgba(255,255,255,0.05);border-radius:15px;padding:30px;box-shadow:0 8px 32px rgba(0,0,0,0.3);}";
  html += "h1{color:#49EACB;margin-bottom:10px;}";
  html += ".info-box{background:rgba(73,234,203,0.1);border-left:4px solid #49EACB;padding:15px;margin:20px 0;border-radius:5px;}";
  html += ".upload-section{margin:30px 0;padding:20px;background:rgba(255,255,255,0.03);border-radius:10px;}";
  html += ".file-list{margin:15px 0;padding:10px;background:rgba(0,0,0,0.3);border-radius:5px;}";
  html += ".file-item{padding:8px;margin:5px 0;background:rgba(73,234,203,0.1);border-radius:3px;font-family:monospace;}";
  html += "input[type='file']{display:block;margin:15px 0;padding:10px;background:#000;border:2px solid #49EACB;border-radius:5px;color:#fff;width:100%;}";
  html += "button{background:#49EACB;color:#1a1a2e;border:none;padding:12px 30px;font-size:16px;border-radius:8px;cursor:pointer;font-weight:bold;margin:10px 5px;}";
  html += "button:hover{background:#3dd4b5;}";
  html += ".status{margin:15px 0;padding:10px;border-radius:5px;font-weight:bold;}";
  html += ".success{background:rgba(76,175,80,0.2);color:#4caf50;border:1px solid #4caf50;}";
  html += ".error{background:rgba(244,67,54,0.2);color:#f44336;border:1px solid #f44336;}";
  html += "a{color:#49EACB;text-decoration:none;font-weight:bold;}";
  html += "</style></head><body>";
  html += "<div class='container'>";
  html += "<h1>📁 SD Card File Upload</h1>";
  html += "<p style='opacity:0.7;margin-bottom:20px'>Upload HTML and logo files to SD card</p>";

  // Show current allowed files on SD card (only show whitelisted files for security)
  html += "<div class='info-box'>";
  html += "<strong>Uploadable Files on SD Card:</strong>";
  html += "<div class='file-list'>";

  bool hasFiles = false;
  for (int i = 0; ALLOWED_FILES[i] != NULL; i++) {
    String filepath = "/" + String(ALLOWED_FILES[i]);
    if (SD.exists(filepath.c_str())) {
      File file = SD.open(filepath.c_str(), FILE_READ);
      if (file) {
        html += "<div class='file-item'>" + String(ALLOWED_FILES[i]) + " (" + String(file.size()) + " bytes)</div>";
        file.close();
        hasFiles = true;
      }
    }
  }

  if (!hasFiles) {
    html += "<div style='opacity:0.6;padding:10px;'>No uploadable files found on SD card</div>";
  }

  html += "</div></div>";

  // Upload form
  html += "<div class='upload-section'>";
  html += "<h2 style='margin-bottom:15px;'>Upload New File</h2>";
  html += "<form method='POST' action='/upload' enctype='multipart/form-data'>";
  html += "<input type='file' name='file' accept='.html,.bin' required>";
  html += "<button type='submit'>Upload to SD Card</button>";
  html += "</form>";
  html += "<div style='margin-top:20px;padding:15px;background:rgba(255,152,0,0.1);border:1px solid #ff9800;border-radius:8px;'>";
  html += "<p style='color:#ff9800;font-weight:bold;margin:0 0 10px 0;'>🔒 Security: Only approved files allowed</p>";
  html += "<p style='opacity:0.8;font-size:0.9em;margin:0;font-family:monospace;'>dashboard.html • kaspa_logo.bin • kasdeck_logo.bin</p>";
  html += "</div>";
  html += "</div>";

  html += "<div style='text-align:center;margin-top:30px;'>";
  html += "<a href='/'>← Back to Dashboard</a>";
  html += "</div>";

  html += "</div></body></html>";

  server.send(200, "text/html; charset=UTF-8", html);
}

// Handle file upload to SD card
void handleFileUploadPost() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    String filename = upload.filename;

    // Security: Check if file is in whitelist
    if (!isFileAllowed(filename)) {
      Serial.printf("SECURITY: Rejected upload of non-whitelisted file: %s\n", filename.c_str());
      addLog("Upload rejected: " + filename + " (not in whitelist)", "error");
      uploadRejected = true;
      rejectedFilename = filename;
      return;  // Don't create the file
    }

    uploadRejected = false;
    rejectedFilename = "";

    // Special handling for firmware updates - save to /firmware/ directory
    if (filename.equalsIgnoreCase("update.bin")) {
      // Create /firmware directory if it doesn't exist
      if (!SD.exists("/firmware")) {
        SD.mkdir("/firmware");
        Serial.println("Created /firmware directory");
      }
      filename = "/firmware/update.bin";
      Serial.println("Firmware file detected - saving to /firmware/update.bin");
    } else {
      // Regular files go to root directory
      if (!filename.startsWith("/")) {
        filename = "/" + filename;
      }
    }

    Serial.printf("Upload Start: %s\n", filename.c_str());
    addLog("File upload started: " + filename, "info");

    // Delete existing file if it exists
    if (SD.exists(filename.c_str())) {
      SD.remove(filename.c_str());
      Serial.printf("Removed existing file: %s\n", filename.c_str());
    }

    uploadFile = SD.open(filename.c_str(), FILE_WRITE);
    if (!uploadFile) {
      Serial.printf("Failed to open file for writing: %s\n", filename.c_str());
    }
  }
  else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadRejected) return;  // Skip writing for rejected files

    if (uploadFile) {
      size_t written = uploadFile.write(upload.buf, upload.currentSize);
      if (written != upload.currentSize) {
        Serial.printf("Write error: only wrote %d of %d bytes\n", written, upload.currentSize);
      }
      uploadFile.flush();  // Flush to SD card immediately
      Serial.printf("Writing %d bytes...\n", upload.currentSize);
    }
  }
  else if (upload.status == UPLOAD_FILE_END) {
    if (uploadRejected) return;  // Skip for rejected files

    if (uploadFile) {
      uploadFile.flush();  // Final flush
      uploadFile.close();
      Serial.printf("Upload Complete: %d bytes\n", upload.totalSize);
      addLog("File upload complete: " + String(upload.totalSize) + " bytes", "success");
    }
  }
}

// Handle upload completion
void handleFileUploadComplete() {
  // Note: Authentication check is done in handleFileUpload (GET), not here
  // This is called after multipart upload completes
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='3;url=/upload'>";

  if (uploadRejected) {
    // Show error for rejected files
    html += "<title>Upload Rejected</title>";
    html += "<style>";
    html += "body{font-family:'Segoe UI',sans-serif;background:#0a0e27;color:#e0e0e0;display:flex;align-items:center;justify-content:center;height:100vh;margin:0;text-align:center;}";
    html += ".error-box{background:linear-gradient(145deg,#16213e,#1a1a2e);padding:40px;border-radius:20px;border:3px solid #f44336;max-width:450px;}";
    html += ".error-icon{width:80px;height:80px;border-radius:50%;background:#f44336;margin:0 auto 20px;display:flex;align-items:center;justify-content:center;font-size:50px;}";
    html += "h1{color:#f44336;margin:20px 0;}";
    html += "p{color:#aaa;margin:15px 0;}";
    html += ".filename{color:#ff6b6b;font-family:monospace;background:rgba(244,67,54,0.1);padding:5px 10px;border-radius:3px;}";
    html += ".allowed{color:#49EACB;font-family:monospace;font-size:0.9em;}";
    html += "a{color:#49EACB;text-decoration:none;font-weight:bold;}";
    html += "</style></head><body>";
    html += "<div class='error-box'>";
    html += "<div class='error-icon'>✕</div>";
    html += "<h1>Upload Rejected</h1>";
    html += "<p>File <span class='filename'>" + rejectedFilename + "</span> is not allowed.</p>";
    html += "<p style='margin-top:20px;'>Only these files can be uploaded:</p>";
    html += "<p class='allowed'>dashboard.html<br>kaspa_logo.bin<br>kasdeck_logo.bin</p>";
    html += "<p style='margin-top:25px;color:#666;'>Redirecting in 3 seconds...</p>";
    html += "<p><a href='/upload'>Try Again</a></p>";
    html += "</div></body></html>";

    // Reset rejection state
    uploadRejected = false;
    rejectedFilename = "";
  } else {
    // Show success
    html += "<title>Upload Complete</title>";
    html += "<style>";
    html += "body{font-family:'Segoe UI',sans-serif;background:#0a0e27;color:#e0e0e0;display:flex;align-items:center;justify-content:center;height:100vh;margin:0;text-align:center;}";
    html += ".success-box{background:linear-gradient(145deg,#16213e,#1a1a2e);padding:40px;border-radius:20px;border:3px solid #4caf50;max-width:400px;}";
    html += ".checkmark{width:80px;height:80px;border-radius:50%;background:#4caf50;margin:0 auto 20px;display:flex;align-items:center;justify-content:center;font-size:50px;}";
    html += "h1{color:#4caf50;margin:20px 0;}";
    html += "p{color:#aaa;margin:15px 0;}";
    html += "a{color:#49EACB;text-decoration:none;font-weight:bold;}";
    html += "</style></head><body>";
    html += "<div class='success-box'>";
    html += "<div class='checkmark'>✓</div>";
    html += "<h1>Upload Successful!</h1>";
    html += "<p>File uploaded to SD card successfully.</p>";
    html += "<p style='margin-top:25px;color:#666;'>Redirecting in 3 seconds...</p>";
    html += "<p><a href='/upload'>Upload Another File</a></p>";
    html += "</div></body></html>";
  }

  server.send(200, "text/html; charset=UTF-8", html);
}

// =================================================================
// SD CARD OTA UPDATE SYSTEM - Safe firmware updates from SD card
// =================================================================
// Note: FirmwareHeader struct is defined near top of file with other structs

// Calculate CRC32 for firmware validation
uint32_t calculateCRC32(const uint8_t* data, size_t length) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < length; i++) {
    crc ^= data[i];
    for (int j = 0; j < 8; j++) {
      crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
  }
  return ~crc;
}

// Validate firmware file on SD card
// Global to store last validation error for API response
String lastValidationError = "";

bool validateFirmware(const char* path, FirmwareHeader* header) {
  lastValidationError = "";

  if (!SD.exists(path)) {
    lastValidationError = "Firmware file not found: " + String(path);
    Serial.println(lastValidationError);
    return false;
  }

  File file = SD.open(path, FILE_READ);
  if (!file) {
    lastValidationError = "Failed to open firmware file";
    Serial.println(lastValidationError);
    return false;
  }

  // Read header
  if (file.size() < FIRMWARE_HEADER_SIZE) {
    lastValidationError = "File too small to contain valid header";
    Serial.println(lastValidationError);
    file.close();
    return false;
  }

  file.read((uint8_t*)header, FIRMWARE_HEADER_SIZE);

  // Validate magic bytes
  if (header->magic != FIRMWARE_MAGIC) {
    char buf[100];
    sprintf(buf, "Invalid magic bytes: 0x%08X (expected 0x%08X)", header->magic, FIRMWARE_MAGIC);
    lastValidationError = String(buf);
    Serial.println(lastValidationError);
    file.close();
    return false;
  }

  // Validate hardware version
  if (strcmp(header->hardwareVersion, HARDWARE_VERSION) != 0) {
    lastValidationError = "Hardware version mismatch: " + String(header->hardwareVersion) + " (this device: " + String(HARDWARE_VERSION) + ")";
    Serial.println(lastValidationError);
    file.close();
    return false;
  }

  // Validate firmware size
  size_t expectedSize = FIRMWARE_HEADER_SIZE + header->firmwareSize;
  if (file.size() != expectedSize) {
    char buf[100];
    sprintf(buf, "File size mismatch: %d bytes (expected %d)", file.size(), expectedSize);
    lastValidationError = String(buf);
    Serial.println(lastValidationError);
    file.close();
    return false;
  }

  // Validate CRC32
  Serial.println("Validating firmware CRC32...");
  uint8_t* buffer = (uint8_t*)malloc(4096);
  if (!buffer) {
    lastValidationError = "Failed to allocate buffer for CRC validation";
    Serial.println(lastValidationError);
    file.close();
    return false;
  }

  uint32_t calculatedCRC = 0xFFFFFFFF;
  size_t bytesRemaining = header->firmwareSize;

  while (bytesRemaining > 0) {
    size_t bytesToRead = min(bytesRemaining, (size_t)4096);
    size_t bytesRead = file.read(buffer, bytesToRead);

    for (size_t i = 0; i < bytesRead; i++) {
      calculatedCRC ^= buffer[i];
      for (int j = 0; j < 8; j++) {
        calculatedCRC = (calculatedCRC >> 1) ^ (0xEDB88320 & -(calculatedCRC & 1));
      }
    }

    bytesRemaining -= bytesRead;
  }

  free(buffer);
  file.close();

  calculatedCRC = ~calculatedCRC;

  if (calculatedCRC != header->crc32) {
    char buf[100];
    sprintf(buf, "CRC32 mismatch: calculated 0x%08X (file header says 0x%08X)", calculatedCRC, header->crc32);
    lastValidationError = String(buf);
    Serial.println(lastValidationError);
    return false;
  }

  Serial.println("✓ Firmware validation passed!");
  Serial.printf("  Version: %s\n", header->version);
  Serial.printf("  Hardware: %s\n", header->hardwareVersion);
  Serial.printf("  Size: %u bytes\n", header->firmwareSize);
  Serial.printf("  Build: %s\n", header->buildDate);

  return true;
}

// Apply firmware update from SD card
bool applyFirmwareUpdate(const char* path) {
  FirmwareHeader header;

  // Validate firmware first
  if (!validateFirmware(path, &header)) {
    Serial.println("Firmware validation failed - update aborted");
    return false;
  }

  // Check version compatibility
  if (compareVersions(header.version, MIN_FIRMWARE_VERSION) < 0) {
    Serial.printf("Firmware too old: %s (minimum: %s)\n", header.version, MIN_FIRMWARE_VERSION);
    return false;
  }

  Serial.println("\n========== FIRMWARE UPDATE ==========");
  Serial.printf("Current:  %s\n", FIRMWARE_VERSION);
  Serial.printf("New:      %s\n", header.version);
  Serial.println("=====================================\n");

  // CRITICAL: Stop mining task to free 48KB memory and prevent flash write conflicts
  if (MinerTask != NULL) {
    Serial.println("Stopping mining task for firmware update...");
    vTaskDelete(MinerTask);
    MinerTask = NULL;
    delay(100);  // Let task cleanup complete
  }

  // Disconnect from pool to prevent network interrupts during flash
  if (stratumClient.connected()) {
    Serial.println("Disconnecting from pool...");
    stratumClient.stop();
  }

  // Disable mining flag
  miningEnabled = false;

  // Set flag FIRST to stop main loop from interfering
  firmwareUpdateInProgress = true;

  // Wait for any pending main loop iteration to complete
  delay(50);

  // Show initial warning message with countdown
  tft.fillScreen(TFT_RED);
  tft.setTextColor(TFT_WHITE, TFT_RED);
  tft.setTextSize(3);
  tft.setTextDatum(MC_DATUM);

  tft.drawString("FIRMWARE UPDATE STARTING", 400, 180);
  tft.drawString("Display will turn off", 400, 230);
  tft.drawString("until update completes", 400, 270);
  tft.drawString("DO NOT REMOVE POWER", 400, 330);

  // Show countdown
  for (int i = 5; i > 0; i--) {
    tft.fillRect(350, 380, 100, 40, TFT_RED);  // Clear countdown area
    tft.drawString(String(i), 400, 400);
    delay(1000);
  }

  // Turn off display backlight
  tft.setBrightness(0);

  Serial.println("Display turned off - firmware update in progress");
  Serial.println("Progress shown in serial output only");

  // CRITICAL: Stop LVGL and display DMA before flash operations
  // LovyanGFX uses DMA which can conflict with flash writes
  Serial.println("Stopping display DMA...");
  tft.endWrite();  // End any pending SPI transaction
  tft.waitDMA();   // Wait for any DMA transfer to complete
  delay(50);

  // CRITICAL: Stop the web server to prevent interrupts during flash write
  Serial.println("Stopping web server...");
  server.stop();
  delay(100);

  // Disconnect WiFi to prevent any network activity during flash
  Serial.println("Disconnecting WiFi...");
  WiFi.disconnect(true);
  delay(100);

  Serial.printf("Free heap before update: %d bytes\n", ESP.getFreeHeap());
  Serial.printf("PSRAM available: %s\n", psramFound() ? "YES" : "NO");
  if (psramFound()) {
    Serial.printf("Free PSRAM: %d bytes\n", ESP.getFreePsram());
  }

  // STEP 1: Read entire firmware from SD into PSRAM BEFORE any flash operations
  // This completely separates SD access from flash access
  Serial.println("Opening firmware file...");
  File file = SD.open(path, FILE_READ);
  if (!file) {
    Serial.println("Failed to open firmware file");
    return false;
  }

  // Skip header - firmware data starts after header
  file.seek(FIRMWARE_HEADER_SIZE);

  Serial.printf("Firmware size: %u bytes\n", header.firmwareSize);
  Serial.flush();

  // Allocate buffer in PSRAM for entire firmware
  Serial.println("Allocating PSRAM buffer...");
  uint8_t* firmwareBuffer = (uint8_t*)heap_caps_malloc(header.firmwareSize, MALLOC_CAP_SPIRAM);
  if (!firmwareBuffer) {
    Serial.println("PSRAM allocation failed, trying heap...");
    firmwareBuffer = (uint8_t*)malloc(header.firmwareSize);
  }

  if (!firmwareBuffer) {
    Serial.println("Failed to allocate firmware buffer!");
    file.close();
    return false;
  }

  // Read entire firmware from SD
  Serial.println("Reading firmware from SD card...");
  Serial.flush();
  size_t bytesRead = file.read(firmwareBuffer, header.firmwareSize);
  file.close();

  if (bytesRead != header.firmwareSize) {
    Serial.printf("SD read failed: got %d of %d bytes\n", bytesRead, header.firmwareSize);
    free(firmwareBuffer);
    return false;
  }

  Serial.printf("Read %d bytes from SD card\n", bytesRead);

  // STEP 2: Completely close SD card before any flash operations
  Serial.println("Unmounting SD card...");
  SD.end();
  delay(100);

  // STEP 3: Now do the flash update with SD completely closed
  Serial.printf("Running on Core %d\n", xPortGetCoreID());
  Serial.flush();

  // Disable Task Watchdog Timer
  Serial.println("Disabling watchdog timer...");
  esp_task_wdt_deinit();
  delay(100);

  // Begin OTA update
  Serial.println("Calling Update.begin()...");
  Serial.flush();

  if (!Update.begin(header.firmwareSize, U_FLASH)) {
    Serial.printf("Update.begin failed: %s\n", Update.errorString());
    free(firmwareBuffer);
    return false;
  }

  Serial.println("Update.begin() succeeded");
  Serial.flush();

  // CRITICAL: ESP32 flash controller can't DMA directly from PSRAM
  // We must copy to a small INTERNAL RAM buffer before each write
  const size_t CHUNK_SIZE = 4096;  // 4KB chunks
  uint8_t* heapBuffer = (uint8_t*)heap_caps_malloc(CHUNK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

  if (!heapBuffer) {
    Serial.println("Failed to allocate internal RAM buffer!");
    Update.abort();
    free(firmwareBuffer);
    return false;
  }

  Serial.println("Writing firmware to flash (chunked via internal RAM)...");
  Serial.flush();

  size_t totalWritten = 0;
  size_t remaining = header.firmwareSize;
  uint8_t* srcPtr = firmwareBuffer;
  int lastPercent = 0;

  while (remaining > 0) {
    size_t toWrite = (remaining > CHUNK_SIZE) ? CHUNK_SIZE : remaining;

    // Copy from PSRAM to internal RAM
    memcpy(heapBuffer, srcPtr, toWrite);

    // Write from internal RAM to flash
    size_t written = Update.write(heapBuffer, toWrite);
    if (written != toWrite) {
      Serial.printf("\nFlash write failed at %d: wrote %d of %d\n", totalWritten, written, toWrite);
      Serial.printf("Update error: %s\n", Update.errorString());
      Update.abort();
      free(heapBuffer);
      free(firmwareBuffer);
      return false;
    }

    totalWritten += written;
    srcPtr += written;
    remaining -= written;

    // Progress update
    int percent = (totalWritten * 100) / header.firmwareSize;
    if (percent >= lastPercent + 10) {
      Serial.printf("Progress: %d%%\n", percent);
      lastPercent = percent;
    }

    yield();
  }

  free(heapBuffer);
  Serial.printf("Flash write complete: %d bytes\n", totalWritten);

  // Cleanup
  free(firmwareBuffer);

  // Finish update
  if (!Update.end(true)) {
    Serial.printf("Update.end failed: %s\n", Update.errorString());
    return false;
  }

  Serial.println("✓ Firmware update successful!");
  Serial.println("Device will reboot in 3 seconds...");

  // Turn display back on and show success message
  tft.setBrightness(255);
  tft.fillScreen(TFT_GREEN);
  tft.setTextColor(TFT_BLACK, TFT_GREEN);
  tft.setTextSize(3);
  tft.setTextDatum(MC_DATUM);

  tft.drawString("FIRMWARE UPDATE", 400, 200);
  tft.drawString("SUCCESSFUL!", 400, 250);
  tft.drawString("Rebooting...", 400, 320);

  delay(3000);  // Show success message for 3 seconds

  return true;
}

// Web handler to check for SD card firmware update
void handleCheckSDUpdate() {
  const char* firmwarePath = "/firmware/update.bin";

  if (!SD.exists(firmwarePath)) {
    server.send(200, "application/json", "{\"updateAvailable\":false,\"valid\":false,\"message\":\"No update file found on SD card\"}");
    return;
  }

  FirmwareHeader header;
  if (!validateFirmware(firmwarePath, &header)) {
    // Return detailed error message
    String json = "{\"updateAvailable\":false,\"valid\":false,\"message\":\"" + lastValidationError + "\"}";
    server.send(200, "application/json", json);
    return;
  }

  // Compare versions (allow same version for testing, >= for production use >)
  bool isNewer = (compareVersions(header.version, FIRMWARE_VERSION) >= 0);

  // Check if mining task is running - warn user they need to stop mining first
  bool miningActive = isMiningTaskRunning();
  Serial.printf("SD Update Check: miningActive=%s, MinerTask=%p\n",
                miningActive ? "true" : "false", MinerTask);

  String json = "{\"updateAvailable\":" + String(isNewer ? "true" : "false");
  json += ",\"valid\":true";
  json += ",\"miningActive\":" + String(miningActive ? "true" : "false");
  json += ",\"currentVersion\":\"" + String(FIRMWARE_VERSION) + "\"";
  json += ",\"newVersion\":\"" + String(header.version) + "\"";
  json += ",\"hardwareVersion\":\"" + String(header.hardwareVersion) + "\"";
  json += ",\"firmwareSize\":" + String(header.firmwareSize);
  json += ",\"buildDate\":\"" + String(header.buildDate) + "\"";
  json += "}";

  server.send(200, "application/json", json);
}

// Web handler to apply SD card firmware update
void handleApplySDUpdate() {
  // Require authentication for firmware updates
  if (!requireAuth()) return;

  const char* firmwarePath = "/firmware/update.bin";

  Serial.println("\n========== SD CARD UPDATE REQUESTED ==========");

  // Read the new firmware version before applying
  FirmwareHeader header;
  String newVersion = "unknown";
  if (validateFirmware(firmwarePath, &header)) {
    newVersion = String(header.version);
  }

  // Save update metadata to preferences BEFORE applying (in case update succeeds but we can't save after)
  // We do this early because after applyFirmwareUpdate, WiFi is disconnected
  Preferences updatePrefs;
  updatePrefs.begin("kaspa", false);
  updatePrefs.putString("lastUpdateVer", newVersion);
  updatePrefs.putString("prevVersion", FIRMWARE_VERSION);

  // Get current time for display
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char timeStr[32];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
    updatePrefs.putString("lastUpdateDate", String(timeStr));
  } else {
    updatePrefs.putString("lastUpdateDate", "Just now");
  }
  updatePrefs.putBool("justUpdated", true);
  updatePrefs.end();

  Serial.println("Update metadata saved to preferences");

  if (applyFirmwareUpdate(firmwarePath)) {
    // Update succeeded - reboot (server already stopped by applyFirmwareUpdate)
    ESP.restart();
  } else {
    // Update failed - clear the justUpdated flag since it didn't actually update
    updatePrefs.begin("kaspa", false);
    updatePrefs.putBool("justUpdated", false);
    updatePrefs.end();
    server.send(500, "application/json", "{\"status\":\"error\",\"message\":\"Firmware update failed. Check serial log.\"}");
  }
}

// Web handler to check if update just completed (called after reboot)
void handleUpdateStatus() {
  Preferences updatePrefs;
  updatePrefs.begin("kaspa", false);

  bool justUpdated = updatePrefs.getBool("justUpdated", false);
  String lastUpdateVer = updatePrefs.getString("lastUpdateVer", "");
  String prevVersion = updatePrefs.getString("prevVersion", "");
  String lastUpdateDate = updatePrefs.getString("lastUpdateDate", "");

  // Clear the justUpdated flag so it only shows once
  if (justUpdated) {
    updatePrefs.putBool("justUpdated", false);
  }
  updatePrefs.end();

  String json = "{";
  json += "\"justUpdated\":" + String(justUpdated ? "true" : "false");
  json += ",\"currentVersion\":\"" + String(FIRMWARE_VERSION) + "\"";
  json += ",\"previousVersion\":\"" + prevVersion + "\"";
  json += ",\"updateDate\":\"" + lastUpdateDate + "\"";
  json += "}";

  server.send(200, "application/json", json);
}

void connectToPool() {
    if (stratumClient.connected()) return;

    // FORCE reload from preferences
    preferences.begin("kaspa", true);
    String loadedWallet = preferences.getString("wallet", "");
    preferences.end();
    
    Serial.println("\n========== CONNECT TO POOL DEBUG ==========");
    Serial.printf("minerWalletAddress variable: '%s' (len: %d)\n",
                  maskWallet(minerWalletAddress).c_str(), minerWalletAddress.length());
    Serial.printf("Loaded from preferences: '%s' (len: %d)\n",
                  maskWallet(loadedWallet).c_str(), loadedWallet.length());
    
    // Use the loaded wallet
    if (loadedWallet.length() > 0) {
        minerWalletAddress = loadedWallet;
        Serial.println("✓ Using wallet from preferences");
    } else {
        Serial.println("❌ ERROR: No wallet in preferences!");
        poolConnected = false;
        return;
    }
    Serial.println("===========================================\n");

    // Clean up the URL
    String addr = minerPoolUrl;
    addr.replace("stratum+tcp://", "");
    
    int colonIndex = addr.indexOf(':');
    if (colonIndex == -1) {
        Serial.println("Invalid Pool URL format. Use IP:PORT");
        addLog("POOL ERROR: Invalid URL format - " + minerPoolUrl, "error");
        return;
    }

    String host = addr.substring(0, colonIndex);
    int port = addr.substring(colonIndex + 1).toInt();

    Serial.printf("Connecting to Stratum Bridge at %s:%d...\n", host.c_str(), port);

    if (stratumClient.connect(host.c_str(), port)) {
        Serial.println("Connected! Sending Handshake...");
        addMiningLog("POOL CONNECTED", host + ":" + String(port));
        addLog("POOL CONNECTED: " + host + ":" + String(port), "success");

        // Load worker name from preferences
        Preferences prefs;
        prefs.begin("kaspa", true);
        String workerName = prefs.getString("worker", "KASDeck");
        prefs.end();

        // 1. Subscribe with worker name
        String sub = "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[\"" + workerName + "\"]}\n";
        Serial.print(">>> SENDING: ");
        Serial.println(sub);
        stratumClient.write((const uint8_t*)sub.c_str(), sub.length());
        stratumClient.flush();

        delay(1000);

        // 2. Authorize with wallet.worker format
        String auth = "{\"id\":2,\"method\":\"mining.authorize\",\"params\":[\"";
        auth += minerWalletAddress + "." + workerName;
        auth += "\"]}\n";
        
        Serial.print(">>> SENDING: ");
        Serial.println(auth);
        Serial.printf("Message length: %d bytes\n", auth.length());
        
        stratumClient.write((const uint8_t*)auth.c_str(), auth.length());
        stratumClient.flush();
        
        Serial.println("Sent both messages, waiting for responses...");
        
    } else {
        Serial.println("❌ Failed to connect");
        addMiningLog("POOL ERROR", "Failed to connect to " + host + ":" + String(port));
        addLog("POOL ERROR: Failed to connect to " + host + ":" + String(port), "error");
        poolConnected = false;
    }
}

void handleStratumMessages() {
  logToSD("=== handleStratumMessages ENTRY ===");  // DIAGNOSTIC: Function entry

  int messagesProcessed = 0;
  const int MAX_MESSAGES_PER_LOOP = 5;  // Limit to prevent UI blocking

  while (stratumClient.available() && messagesProcessed < MAX_MESSAGES_PER_LOOP) {
    logToSD("Message available from pool");  // DIAGNOSTIC: Before reading

    // CRITICAL FIX: Use char buffer instead of String to eliminate heap allocation
    static char line[1024];  // Static buffer for JSON message (reused each call)
    int lineLen = 0;

    // Read until newline, building into char buffer
    while (stratumClient.available() && lineLen < 1023) {
      char c = stratumClient.read();
      if (c == '\n') break;
      if (c != '\r') {  // Skip carriage return
        line[lineLen++] = c;
      }
    }
    line[lineLen] = '\0';  // Null terminate

    // Trim leading/trailing whitespace
    int start = 0;
    while (start < lineLen && (line[start] == ' ' || line[start] == '\t')) start++;
    int end = lineLen - 1;
    while (end > start && (line[end] == ' ' || line[end] == '\t')) end--;

    // If all whitespace, skip
    if (start > end) {
      logToSD("Empty line, skipping");  // DIAGNOSTIC: Empty line
      continue;
    }

    // Move trimmed string to start of buffer
    if (start > 0) {
      int trimLen = end - start + 1;
      memmove(line, line + start, trimLen);
      line[trimLen] = '\0';
      lineLen = trimLen;
    } else {
      line[end + 1] = '\0';
      lineLen = end + 1;
    }

    char msg[150];
    sprintf(msg, "Received line (len=%d): %.50s...", lineLen, line);
    logToSD(msg);  // DIAGNOSTIC: Show raw message

    messagesProcessed++;

    DynamicJsonDocument doc(2048);
    DeserializationError error = deserializeJson(doc, line);

    if (error) {
      char err_msg[100];
      sprintf(err_msg, "JSON parse error: %s", error.c_str());
      logToSD(err_msg);  // DIAGNOSTIC: JSON error
      continue;
    }

    logToSD("JSON parsed successfully");  // DIAGNOSTIC: JSON success

    // DISABLED ALL SD LOGGING - causes crashes during mining
    // if (doc.containsKey("method")) {
    //   const char* method = doc["method"];
    //   char method_msg[100];
    //   sprintf(method_msg, "Method: %s", method);
    //   logToSD(method_msg);
    // } else {
    //   logToSD("No method field in JSON");
    // }

    // Handle mining.notify (new job)
    if (doc.containsKey("method") && doc["method"] == "mining.notify") {
      // DISABLED: logToSD("Job notification received from pool");

      // LOCK MUTEX before modifying shared state
      if (xSemaphoreTake(miningStateMutex, portMAX_DELAY) == pdTRUE) {
        JsonArray params = doc["params"];
        // Copy job ID as C-string to avoid String heap allocation
        const char* jobIdFromJson = params[0].as<const char*>();
        strncpy(currentJobId, jobIdFromJson, 64);
        currentJobId[64] = '\0';

        // DISABLED: logToSD job ID parsing

        // The Kaspa bridge sends header as an array of uint64 numbers, not hex strings
        // params format: ["job_id", [uint64, uint64, uint64, uint64], timestamp]

        if (params[1].is<JsonArray>()) {
          // DISABLED: logToSD("Parsing header array (4x uint64)");

          // New format: array of numbers (4 x uint64 = 32 bytes)
          JsonArray headerArray = params[1];

          // Convert each uint64 to 8 bytes (little-endian)
          for (int i = 0; i < 4 && i < headerArray.size(); i++) {
            uint64_t value = headerArray[i].as<uint64_t>();

            // Store as little-endian bytes
            for (int j = 0; j < 8; j++) {
              currentHeaderHash[i * 8 + j] = (value >> (j * 8)) & 0xFF;
            }
          }

          // DISABLED: logToSD("Header array parsed successfully");
        } else {
          // DISABLED: logToSD("Parsing header hex string");
          // Old format: hex string - use const char* to avoid String allocation
          const char* headerHex = params[1].as<const char*>();
          if (headerHex != nullptr) {
            // hexToBytes expects a C-string, not a String
            int hexLen = strlen(headerHex);
            for (int i = 0; i < 32 && i * 2 < hexLen; i++) {
              char hexByte[3] = {headerHex[i * 2], headerHex[i * 2 + 1], '\0'};
              currentHeaderHash[i] = strtoul(hexByte, nullptr, 16);
            }
          }
          // DISABLED: logToSD("Header hex parsed successfully");
        }

        // For Kaspa, there's no separate target in mining.notify
        // Target is set via mining.set_difficulty, which we already handle

        hasJob = true;
        poolConnected = true;

        // DISABLED: logToSD("Job reception complete - hasJob=true");

        // UNLOCK MUTEX after modifying shared state
        xSemaphoreGive(miningStateMutex);
      } else {
        Serial.println("ERROR: Failed to acquire mutex in handleStratumMessages");
        // DISABLED: logToSD("ERROR: Failed to acquire mutex in handleStratumMessages");
      }
      
      // Only show job updates if mining is enabled
      // COMMENTED OUT - Uncomment for debugging job notifications
      // if (miningEnabled) {
      //   Serial.printf("✓ New Job: %s (Diff: %.2f)\n", currentJobId, currentDifficulty);
      // }
    }
    
    //Handle mining.set_difficulty (difficulty number updates)
    if (doc.containsKey("method") && doc["method"] == "mining.set_difficulty") {
        double diff = doc["params"][0].as<double>();

        // LOCK MUTEX before modifying currentTarget
        if (xSemaphoreTake(miningStateMutex, portMAX_DELAY) == pdTRUE) {
          setTargetFromDifficulty(diff); // This updates currentTarget using the math function
          xSemaphoreGive(miningStateMutex);
        } else {
          Serial.println("ERROR: Failed to acquire mutex in set_difficulty handler");
          // DISABLED: logToSD("ERROR: Failed to acquire mutex in set_difficulty handler");
        }
    }

    // Handle authorization response (id: 2)
    if (doc.containsKey("id") && doc["id"] == 2) {
      if (doc.containsKey("result")) {
        isAuthorized = doc["result"].as<bool>();
        poolConnected = isAuthorized;
        Serial.printf("Authorization: %s\n", isAuthorized ? "SUCCESS ✓" : "FAILED ✗");

        if (!isAuthorized && doc.containsKey("error")) {
          // Use const char* to avoid String allocation
          const char* errorMsg = doc["error"].as<const char*>();
          Serial.printf("Auth error: %s\n", errorMsg ? errorMsg : "unknown");
        }
      }
    }
    
    // Handle share submission response (id: 4)
    // FIXED: Only process if this is NOT a method call (mining.notify, etc)
    if (doc.containsKey("id") && doc["id"] == 4 && !doc.containsKey("method")) {
      if (doc.containsKey("result")) {
        // Check if result is true (accepted)
        if (doc["result"].is<bool>() && doc["result"].as<bool>()) {
          // Accepted!
          sharesAccepted++;
          
          // Check if this was a block!
          // Some pools send "block" in the error field (not actually an error, just info)
          // Some send it in result as a string instead of bool
          // Some don't send anything special at all
          bool isBlock = false;
          
          // Method 1: Check error field for "block" keyword (some pools do this)
          if (doc.containsKey("error") && !doc["error"].isNull()) {
            const char* errorStr = doc["error"].as<const char*>();
            if (errorStr != nullptr) {
              // Check for "block" case-insensitively (convert to lowercase manually)
              char errorLower[128];
              int i = 0;
              while (errorStr[i] && i < 127) {
                errorLower[i] = tolower(errorStr[i]);
                i++;
              }
              errorLower[i] = '\0';
              if (strstr(errorLower, "block") != nullptr) {
                isBlock = true;
                Serial.println("🎯 Block detected via error field!");
                blocksFound++;
              }
            }
          }

          // Method 2: Check if result is a string containing "block"
          if (!isBlock && doc["result"].is<const char*>()) {
            const char* resultStr = doc["result"].as<const char*>();
            if (resultStr != nullptr) {
              // Check for "block" case-insensitively
              char resultLower[128];
              int i = 0;
              while (resultStr[i] && i < 127) {
                resultLower[i] = tolower(resultStr[i]);
                i++;
              }
              resultLower[i] = '\0';
              if (strstr(resultLower, "block") != nullptr) {
                isBlock = true;
                Serial.println("🎯 Block detected via result string!");
                blocksFound++;
              }
            }
          }
          
          // Method 3: Check for extremely low hash (< network difficulty)
          // Unfortunately we don't have the actual hash value here to check
          // This would require computing the hash and comparing to network difficulty

          if (isBlock) {
            // 🎉 BLOCK FOUND! 🎉
            Serial.println("\n");
            Serial.println("██████╗ ██╗      ██████╗  ██████╗██╗  ██╗");
            Serial.println("██╔══██╗██║     ██╔═══██╗██╔════╝██║ ██╔╝");
            Serial.println("██████╔╝██║     ██║   ██║██║     █████╔╝ ");
            Serial.println("██╔══██╗██║     ██║   ██║██║     ██╔═██╗ ");
            Serial.println("██████╔╝███████╗╚██████╔╝╚██████╗██║  ██╗");
            Serial.println("╚═════╝ ╚══════╝ ╚═════╝  ╚═════╝╚═╝  ╚═╝");
            Serial.println("");
            Serial.println("🎉🎉🎉 BLOCK FOUND! 🎉🎉🎉");
            Serial.println("💎💎💎 YOU FOUND A BLOCK! 💎💎💎");
            Serial.printf("Block #%u | Total Blocks: %u\n", blocksFound, blocksFound);
            Serial.println("================================\n");
            addMiningLog("🏆 BLOCK FOUND!", "Block #" + String(blocksFound));
            
            // Show celebration on LCD!
            showBlockCelebration(false, blocksFound);  // false = real block, not sample
          } else {
            // Regular share accepted
            Serial.println("\n✅✅✅ SHARE ACCEPTED! ✅✅✅");
            Serial.printf("Accepted: %u | Rejected: %u | Total: %u\n", 
                         sharesAccepted, sharesRejected, sharesSubmitted);
            if (sharesSubmitted > 0) {
              float acceptRate = (float)sharesAccepted / (float)sharesSubmitted * 100.0;
              Serial.printf("Accept Rate: %.1f%%\n", acceptRate);
            }
            Serial.println("================================\n");
            addMiningLog("SHARE ACCEPTED", "Share #" + String(sharesSubmitted));
            addLog("SHARE ACCEPTED: Share #" + String(sharesSubmitted), "success");

            // Real block celebration only happens when isBlock == true (detected via pool response)
          }
        } else if (doc["result"].isNull() || (doc["result"].is<bool>() && !doc["result"].as<bool>())) {
          // Rejected - figure out why
          sharesRejected++;

          const char* rejectReason = "Unknown";
          if (doc.containsKey("error") && !doc["error"].isNull()) {
            if (doc["error"].is<JsonArray>()) {
              JsonArray err = doc["error"];
              if (err.size() > 1) {
                const char* errorMsg = err[1].as<const char*>();
                if (errorMsg != nullptr) {
                  // Convert to lowercase for comparison
                  char errorLower[128];
                  int i = 0;
                  while (errorMsg[i] && i < 127) {
                    errorLower[i] = tolower(errorMsg[i]);
                    i++;
                  }
                  errorLower[i] = '\0';

                  // Categorize the rejection
                  if (strstr(errorLower, "stale") != nullptr || strstr(errorLower, "old") != nullptr) {
                    sharesStale++;
                    rejectReason = "Stale";
                  } else if (strstr(errorLower, "duplicate") != nullptr || strstr(errorLower, "dup") != nullptr) {
                    sharesDuplicate++;
                    rejectReason = "Duplicate";
                  } else {
                    sharesInvalid++;
                    rejectReason = "Invalid";
                  }
                }
              }
            } else if (doc["error"].is<const char*>()) {
              const char* errorMsg = doc["error"].as<const char*>();
              if (errorMsg != nullptr) {
                // Convert to lowercase for comparison
                char errorLower[128];
                int i = 0;
                while (errorMsg[i] && i < 127) {
                  errorLower[i] = tolower(errorMsg[i]);
                  i++;
                }
                errorLower[i] = '\0';

                if (strstr(errorLower, "stale") != nullptr || strstr(errorLower, "old") != nullptr) {
                  sharesStale++;
                  rejectReason = "Stale";
                } else if (strstr(errorLower, "duplicate") != nullptr || strstr(errorLower, "dup") != nullptr) {
                  sharesDuplicate++;
                  rejectReason = "Duplicate";
                } else {
                  sharesInvalid++;
                  rejectReason = "Invalid";
                }
              }
            }
          } else {
            // No error field, assume invalid
            sharesInvalid++;
            rejectReason = "Invalid";
          }
          
          Serial.println("\n❌❌❌ SHARE REJECTED ❌❌❌");
          Serial.printf("Reason: %s\n", rejectReason);
          Serial.printf("Accepted: %u | Rejected: %u (Stale: %u, Dupe: %u, Invalid: %u)\n", 
                       sharesAccepted, sharesRejected, sharesStale, sharesDuplicate, sharesInvalid);
          if (sharesSubmitted > 0) {
            float acceptRate = (float)sharesAccepted / (float)sharesSubmitted * 100.0;
            Serial.printf("Accept Rate: %.1f%%\n", acceptRate);
          }
          Serial.println("========================\n");

          addMiningLog("SHARE REJECTED", rejectReason);
          addLog("SHARE REJECTED: " + String(rejectReason), "error");
        }
      } else {
        // No result field - this shouldn't happen for a share response
        // Silenced for production - uncomment for debugging
        // Serial.println("!️ WARNING: Share response (id:4) has no result field!");
        // Serial.println("Response was: " + line);
      }
    }
  }
}

void handleToggle() {
  // Require authentication for mining toggle
  if (!requireAuth()) return;

  // If currently mining, add elapsed time to total
  if (miningEnabled && lastMiningStateChange > 0) {
    totalMiningTime += (millis() - lastMiningStateChange) / 1000;
  }

  miningEnabled = !miningEnabled; // Flip the switch

  // Record when state changed
  lastMiningStateChange = millis();

  // Set or clear mining started timestamp
  if (miningEnabled) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      char timeStr[32];
      strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M", &timeinfo);
      miningStartedTimestamp = String(timeStr);
    } else {
      miningStartedTimestamp = "Started";  // Fallback if NTP not synced
    }
  } else {
    miningStartedTimestamp = "";  // Clear when mining stops
  }

  // Start or stop mining task based on new state
  if (miningEnabled) {
    startMiningTask();
  } else {
    stopMiningTask();
  }

  // Save state so it remembers after reboot
  Preferences prefs;
  prefs.begin("kaspa", false);
  prefs.putBool("miningOn", miningEnabled);
  prefs.end();

  // Redirect back to home
  server.sendHeader("Location", "/");
  server.send(303);
}

// ==================== OTA UPDATE FUNCTIONS ====================

// Compare two version strings (e.g., "1.2.3" vs "1.2.4")
int compareVersions(String v1, String v2) {
  int parts1[3] = {0}, parts2[3] = {0};
  sscanf(v1.c_str(), "%d.%d.%d", &parts1[0], &parts1[1], &parts1[2]);
  sscanf(v2.c_str(), "%d.%d.%d", &parts2[0], &parts2[1], &parts2[2]);
  for (int i = 0; i < 3; i++) {
    if (parts1[i] > parts2[i]) return 1;
    if (parts1[i] < parts2[i]) return -1;
  }
  return 0;
}

bool checkForUpdates() {
  Serial.println("\n=== Checking for firmware updates ===");
  Serial.printf("Current version: %s\n", FIRMWARE_VERSION);
  
  if (!WiFi.isConnected()) return false;
  
  HTTPClient http;
  http.begin(GITHUB_API_URL);
  http.addHeader("Accept", "application/vnd.github.v3+json");
  
  int httpCode = http.GET();
  if (httpCode != 200) {
    http.end();
    return false;
  }
  
  String payload = http.getString();
  http.end();
  
  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, payload)) return false;
  
  String latestVersion = doc["tag_name"].as<String>();
  latestVersion.replace("v", "");
  
  latestFirmware.version = latestVersion;
  latestFirmware.changelog = doc["body"].as<String>();
  
  JsonArray assets = doc["assets"];
  for (JsonObject asset : assets) {
    String name = asset["name"].as<String>();
    if (name.endsWith(".bin")) {
      latestFirmware.downloadUrl = asset["browser_download_url"].as<String>();
      latestFirmware.fileSize = asset["size"].as<int>();
      break;
    }
  }
  
  latestFirmware.updateAvailable = (compareVersions(latestVersion, FIRMWARE_VERSION) > 0);
  
  if (latestFirmware.updateAvailable) {
    Serial.printf("✓ Update available: %s → %s\n", FIRMWARE_VERSION, latestVersion.c_str());
  }
  
  lastUpdateCheck = millis();
  return latestFirmware.updateAvailable;
}

bool performOTAUpdate(String firmwareUrl) {
  if (updateInProgress) return false;
  
  updateInProgress = true;
  updateProgress = 0;
  
  Serial.println("\n=== Starting OTA Update ===");
  Serial.printf("Downloading from: %s\n", firmwareUrl.c_str());
  
  HTTPClient http;
  http.begin(firmwareUrl);
  
  int httpCode = http.GET();
  if (httpCode != 200) {
    http.end();
    updateInProgress = false;
    return false;
  }
  
  int contentLength = http.getSize();
  if (contentLength <= 0 || !Update.begin(contentLength)) {
    http.end();
    updateInProgress = false;
    return false;
  }
  
  WiFiClient *stream = http.getStreamPtr();
  size_t written = 0;
  uint8_t buffer[128];
  
  while (http.connected() && (written < contentLength)) {
    size_t available = stream->available();
    if (available) {
      int bytesToRead = ((available > sizeof(buffer)) ? sizeof(buffer) : available);
      int bytesRead = stream->readBytes(buffer, bytesToRead);
      if (bytesRead > 0) {
        Update.write(buffer, bytesRead);
        written += bytesRead;
        updateProgress = (contentLength > 0) ? (written * 100) / contentLength : 0;
      }
    }
    delay(1);
  }
  
  http.end();
  
  if (written == contentLength && Update.end() && Update.isFinished()) {
    Serial.println("✓ Update complete! Rebooting...");
    delay(2000);
    ESP.restart();
    return true;
  }
  
  updateInProgress = false;
  return false;
}

void handleCheckUpdate() {
  if (checkForUpdates()) {
    String json = "{\"updateAvailable\":true,\"currentVersion\":\"" + String(FIRMWARE_VERSION) + 
                  "\",\"latestVersion\":\"" + latestFirmware.version + 
                  "\",\"fileSize\":" + String(latestFirmware.fileSize) + 
                  ",\"changelog\":\"" + latestFirmware.changelog + "\"}";
    server.send(200, "application/json", json);
  } else {
    server.send(200, "application/json", "{\"updateAvailable\":false,\"currentVersion\":\"" + String(FIRMWARE_VERSION) + "\"}");
  }
}

void handleInstallUpdate() {
  // Require authentication for firmware updates
  if (!requireAuth()) return;

  if (!latestFirmware.updateAvailable) {
    server.send(400, "application/json", "{\"error\":\"No update available\"}");
    return;
  }
  server.send(200, "application/json", "{\"status\":\"started\"}");
  delay(100);
  performOTAUpdate(latestFirmware.downloadUrl);
}

void handleUpdateProgress() {
  String json = "{\"inProgress\":" + String(updateInProgress ? "true" : "false") + 
                ",\"progress\":" + String(updateProgress) + "}";
  server.send(200, "application/json", json);
}

// ElegantOTA handles all firmware upload logic automatically

void handleOTAPage();  // Forward declaration

void setupOTA() {
  // ELEGANTOTA DISABLED - Removed to save memory and fix mining crash
  // ElegantOTA was causing BSS/heap corruption issues
  // ElegantOTA.begin(&server);

  // Keep existing custom GitHub update check endpoints
  server.on("/check-update", handleOTAPage);  // Custom UI for GitHub updates
  server.on("/api/check-update", handleCheckUpdate);
  server.on("/api/install-update", HTTP_POST, handleInstallUpdate);
  server.on("/api/update-progress", handleUpdateProgress);

  Serial.println("✓ OTA update system initialized (custom only - ElegantOTA disabled)");
  Serial.printf("  Current version: %s\n", FIRMWARE_VERSION);
  Serial.println("  Use your device's mDNS name (shown above) or IP address for web access");

  // Check for GitHub updates once on boot
  Serial.println("Checking for firmware updates on boot...");
  checkForUpdates();
}

void loopOTA() {
  if (millis() - lastUpdateCheck > UPDATE_CHECK_INTERVAL) {
    checkForUpdates();
  }
}

// ==================== END OTA FUNCTIONS ====================

void setup() {
  setCpuFrequencyMhz(240);
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n=== Kaspa Dashboard Starting ===");

  // Initialize SD card for HTML storage and logos (saves 1.8MB flash space)
  Serial.print("Initializing SD card...");
  if (!SD.begin()) {
    Serial.println(" FAILED!");
    Serial.println("⚠️  SD card not found - web interface and logos will be limited");
  } else {
    Serial.println(" OK");
    Serial.printf("SD card size: %llu MB\n", SD.cardSize() / (1024 * 1024));

    // Load logos from SD card
    initLogosFromSD();

    // Initialize debug log file
    File debugLog = SD.open("/debug.txt", FILE_WRITE);
    if (debugLog) {
      debugLog.printf("=== KASDeck Debug Log Started at %lu ===\n", millis());
      debugLog.close();
      Serial.println("Debug logging to SD card enabled");
    }
  }

  tft.init();
  tft.setRotation(0);
  tft.setBrightness(255);
  tft.setColorDepth(16);

  Wire.begin(TOUCH_GT911_SDA, TOUCH_GT911_SCL);
  ts.begin();
  ts.setRotation(TOUCH_GT911_ROTATION);
  
  // Clear any phantom touches on startup
  delay(100);
  for (int i = 0; i < 10; i++) {
    ts.read();  // Read and discard
    delay(10);
  }
  Serial.println("Touch sensor initialized and cleared");

  lv_init();

  // Use original simple buffer configuration (proven stable)
  lv_disp_draw_buf_init(&draw_buf, buf, NULL, 800 * 10);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = 800;
  disp_drv.ver_res = 480;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touch_read;
  lv_indev_drv_register(&indev_drv);

  // Check for first boot / wizard completion BEFORE creating UI
  Preferences prefs;
  prefs.begin("kaspa", true);  // Read-only first
  bool wizardDone = prefs.getBool("wizardDone", false);
  prefs.end();

  // Clear any stale "justUpdated" flag on fresh install (prevents false positive success banner)
  // This handles the case where Arduino IDE uploads new firmware - not an OTA update
  if (!wizardDone) {
    Preferences updatePrefs;
    updatePrefs.begin("kaspa", false);
    updatePrefs.putBool("justUpdated", false);  // Clear stale flag
    updatePrefs.end();
  }

  if (!wizardDone) {
    // First boot - show dark gradient screen then launch wizard
    Serial.println("First boot detected - launching setup wizard");
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_grad_color(lv_scr_act(), lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_bg_grad_dir(lv_scr_act(), LV_GRAD_DIR_VER, 0);
    
    firstBoot = true;
    wizardStep = 0;
    show_wizard_welcome();
    
    // Run LVGL loop for wizard - blocks until wizard completes
    while (firstBoot) {
      checkWiFiScanComplete();  // Check for WiFi scan completion during wizard

      // Handle WiFi connection state machine during wizard
      if (wifiState == WIFI_CONNECTING) {
        if (WiFi.status() == WL_CONNECTED) {
          Serial.println("\nWizard: WiFi Connected!");
          wifiState = WIFI_CONNECTED;

          // Update overlay
          if (connectingStatusLabel) {
            lv_label_set_text(connectingStatusLabel, "Connected!");
          }

          // Save credentials
          Preferences prefs;
          prefs.begin("kaspa", false);
          prefs.putString("ssid", selectedSSID);
          prefs.putString("password", pendingPassword);
          prefs.end();

          Serial.printf("Saved credentials for: %s\n", selectedSSID.c_str());

          // Wait a moment to show success
          delay(1000);

          // Clean up overlay
          if (connectingOverlay) {
            lv_obj_del(connectingOverlay);
            connectingOverlay = nullptr;
          }

          // Show success
          showSaveConfirmation();
          delay(1500);

          // Clean up WiFi menu
          selectedWifiButton = nullptr;
          ssidDisplayLabel = nullptr;
          passwordTextarea = nullptr;
          scanLabel = nullptr;
          wifiList = nullptr;

          if (wifiScanInProgress) {
            WiFi.scanDelete();
            wifiScanInProgress = false;
            wifiScanComplete = false;
          }

          if (wifiConfigMenu) {
            lv_obj_del(wifiConfigMenu);
            wifiConfigMenu = nullptr;
          }

          // Advance wizard to mining setup
          wizardStep = 3;
          show_wizard_mining_setup();

          wifiState = WIFI_IDLE;
        } else if (millis() - wifiConnectStartTime > 15000) {
          // Timeout after 15 seconds (longer for wizard)
          Serial.println("\nWizard: Connection failed!");
          wifiState = WIFI_FAILED;

          if (connectingOverlay) {
            lv_obj_del(connectingOverlay);
            connectingOverlay = nullptr;
          }

          showErrorNotification("Connection failed!\nCheck password and try again");
          wifiState = WIFI_IDLE;
        }
      }

      lv_timer_handler();
      delay(5);
    }
  }
  
  // Now create the main UI (either after wizard or on normal boot)
  create_ui();
  
  // Check for WiFi reset flag
  Preferences prefs2;
  prefs2.begin("kaspa", false);
  bool wifiReset = prefs2.getBool("wifiReset", false);

  if (wifiReset) {
    // Clear the flag so we don't loop
    prefs2.putBool("wifiReset", false);
    prefs2.end();

    // WiFi credentials were already cleared by wifi_reset_event_cb
    // Just update UI labels and continue - don't block
    Serial.println("WiFi reset - credentials cleared");
    lv_label_set_text(priceLabel, "No WiFi");
    lv_label_set_text(changeLabel, "Connect to KASDeck");
    lv_timer_handler();
  } else {
    prefs2.end();
  }

  // Initialize WiFi with persistent mode enabled
  WiFi.mode(WIFI_STA);
  WiFi.persistent(true);  // Enable credential persistence globally

  // Give WiFi a moment to initialize and check for saved credentials
  delay(100);

  // Check for credentials in BOTH places:
  // 1. ESP32's built-in WiFi storage (set via WiFi.begin in Settings menu)
  // 2. App preferences (also set in Settings menu)
  // 3. WiFiManager's storage (set via captive portal)

  Serial.printf("Checking for saved WiFi credentials...\n");
  Serial.printf("  ESP32 WiFi storage: '%s'\n", WiFi.SSID().c_str());

  Preferences prefs3;
  prefs3.begin("kaspa", true);
  String appSSID = prefs3.getString("ssid", "");
  String appPassword = prefs3.getString("password", "");
  prefs3.end();
  Serial.printf("  App preferences: '%s'\n", appSSID.c_str());

  // Update UI to show we're connecting
  if (priceLabel) lv_label_set_text(priceLabel, "Connecting...");
  if (changeLabel) lv_label_set_text(changeLabel, "Please wait");
  lv_timer_handler();  // Force UI update

  // Try to connect using any available credentials
  if (WiFi.SSID().length() > 0) {
    // ESP32 has saved credentials - use them
    Serial.println("Saved WiFi credentials found in ESP32 storage - attempting connection...");
    WiFi.begin();  // Connect using saved credentials

    // Wait up to 10 seconds for connection
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      Serial.print(".");
      lv_timer_handler();  // Keep UI responsive
      attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nConnected!");
    } else {
      Serial.println("\nConnection failed - clearing bad credentials");
      WiFi.disconnect(true, true);  // Clear bad credentials
    }
  } else if (appSSID.length() > 0) {
    // App preferences have credentials - use them
    Serial.println("Saved WiFi credentials found in app preferences - attempting connection...");
    WiFi.begin(appSSID.c_str(), appPassword.c_str());

    // Wait up to 10 seconds for connection
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      Serial.print(".");
      lv_timer_handler();  // Keep UI responsive
      attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nConnected!");
    } else {
      Serial.println("\nConnection failed - resetting WiFi for scan capability");
      // Disconnect and reset WiFi to ensure scanning works
      WiFi.disconnect(false, false);  // Disconnect but don't erase credentials or turn off WiFi
      delay(100);
      WiFi.mode(WIFI_STA);  // Reset to STA mode
      delay(100);
    }
  } else {
    // Try WiFiManager autoConnect as last resort
    Serial.println("No saved WiFi credentials found - trying WiFiManager...");
    wm.setConfigPortalTimeout(30);
    if (!wm.autoConnect("KASDeck", "kaspa123")) {
      Serial.println("WiFiManager connection failed - AP mode available");
      Serial.println("! Connect to 'KASDeck' WiFi (password: kaspa123) to configure network");
    }
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    testConnectivity();
    
    // Configure NTP time sync with timezone and multiple fallback servers
    // Convert hours to seconds
    int timezoneOffsetSeconds = timezoneOffsetHours * 3600;
    
    Serial.printf("Configuring timezone: UTC%+d (%d seconds)\n", timezoneOffsetHours, timezoneOffsetSeconds);
    Serial.println("Syncing time with NTP servers...");
    
    // Try each NTP server until one works
    bool timeSynced = false;
    for (int i = 0; i < numNtpServers && !timeSynced; i++) {
      Serial.printf("Trying NTP server: %s\n", ntpServers[i]);
      lv_timer_handler();  // Keep UI responsive

      // Configure time with this server
      configTime(timezoneOffsetSeconds, 3600, ntpServers[i]);

      // Wait for sync with timeout
      struct tm timeinfo;
      if (getLocalTime(&timeinfo, 5000)) {  // 5 second timeout
        char timeStr[64];
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
        Serial.printf("✓ Time synced from %s: %s\n", ntpServers[i], timeStr);
        timeSynced = true;
      } else {
        Serial.printf("✗ Failed to sync from %s\n", ntpServers[i]);
      }
      lv_timer_handler();  // Keep UI responsive
    }
    
    if (!timeSynced) {
      Serial.println("! Failed to sync time from all NTP servers");
      Serial.println("Timestamps will use uptime format");
    }
  }
  
  delay(1000);

  // Only fetch data if WiFi is connected
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n--- Initial data fetch on startup ---");

    // Update UI to show loading state
    if (priceLabel) lv_label_set_text(priceLabel, "Loading...");
    if (changeLabel) lv_label_set_text(changeLabel, "Fetching data...");
    lv_timer_handler();  // Force UI update

    #if ENABLE_PRICE_HISTORY
    fetchHistoricalPriceData();    // Fetch 24h historical price data first
    lv_timer_handler();  // Keep UI responsive
    #endif

    fetch_data();                  // Then get current price (overlays on historical data)
    lv_timer_handler();  // Keep UI responsive

    #if ENABLE_NETWORK_HASHRATE_CHART
    loadHashrateHistory();         // Load old data AND populate chart
    lv_timer_handler();  // Keep UI responsive

    fetchNetworkHashrate();        // Get fresh network data (sets networkHashrate variable)
    lv_timer_handler();  // Keep UI responsive

    updateHashrateChart();         // Add fresh data point to history and chart
    lv_timer_handler();  // Keep UI responsive

    saveHashrateHistory();         // Save updated history with fresh point
    #else
    fetchNetworkHashrate();        // Still fetch network hashrate for display
    #endif

    Serial.println("✓ Price history populated with 24h historical data from CoinGecko");
    Serial.println("  Chart will update with live data every 5 minutes");
  } else {
    Serial.println("\n--- WiFi not connected - skipping data fetch ---");
    Serial.println("! Connect to WiFi network 'KASDeck' (password: kaspa123)");
    Serial.println("  Then use Settings menu to configure WiFi");

    // Set UI to show "No WiFi" state
    if (priceLabel) {
      lv_label_set_text(priceLabel, "No WiFi");
    }
    if (changeLabel) {
      lv_label_set_text(changeLabel, "Connect to KASDeck");
    }
  }

  // Load miner configuration from preferences
  Serial.println("\n=== Loading Miner Configuration ===");
  Preferences prefs4;
  prefs4.begin("kaspa", true);  // Read-only
  minerWalletAddress = prefs4.getString("wallet", "");
  minerPoolUrl = prefs4.getString("pool", "stratum+tcp://pool.proofofprints.com:5555");
  miningEnabled = prefs4.getBool("mining", false);
  timezoneOffsetHours = prefs4.getInt("timezone", -6);  // Default Central Time
  prefs4.end();
  
  Serial.printf("Timezone: UTC%+d\n", timezoneOffsetHours);
  
  // Force mining OFF if no wallet configured (first boot scenario)
  if (minerWalletAddress.length() == 0) {
    miningEnabled = false;
    Serial.println("No wallet configured - mining disabled");
  }
  
  Serial.printf("Wallet: %s\n", minerWalletAddress.length() > 0 ? maskWallet(minerWalletAddress).c_str() : "NOT CONFIGURED");
  Serial.printf("Pool: %s\n", minerPoolUrl.c_str());
  Serial.printf("Mining Enabled: %s\n", miningEnabled ? "YES" : "NO");

  // Set mining started timestamp if mining is enabled on boot
  if (miningEnabled) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      char timeStr[32];
      strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M", &timeinfo);
      miningStartedTimestamp = String(timeStr);
    } else {
      miningStartedTimestamp = "Started";  // Fallback if NTP not synced
    }
  }

  Serial.println("===================================\n");
  // ============================================

  // Initialize mutex for protecting shared mining state between cores
  miningStateMutex = xSemaphoreCreateMutex();
  if (miningStateMutex == NULL) {
    Serial.println("Failed to create mining state mutex!");
  } else {
    Serial.println("Mining state mutex created successfully");
  }

  // Only start mining task if mining was enabled (saves 48KB RAM when disabled)
  Serial.println("✓ KHeavyHash ready (matrix will generate on first job)");
  if (miningEnabled) {
    Serial.println("Mining was enabled - starting mining task...");
    startMiningTask();
    miningStartTime = millis();
  } else {
    Serial.println("Mining disabled - task not started (48KB RAM saved)");
  }
  
  // Initialize mining time tracking
  lastMiningStateChange = millis();
  totalMiningTime = 0;

  // Start mDNS with unique name based on MAC address (for multiple devices on same network)
  String mac = WiFi.macAddress();
  mac.replace(":", "");  // Remove colons
  String mdnsName = "kaspa-" + mac.substring(8);  // Use last 4 chars of MAC (e.g., kaspa-a1b2c3d4)
  
  if (MDNS.begin(mdnsName.c_str())) {
    Serial.println("mDNS responder started: http://" + mdnsName + ".local");
    Serial.println("You can also use the IP address: http://" + WiFi.localIP().toString());
  }

  // Collect headers for authentication (X-Auth-Token and Cookie)
  const char* headerKeys[] = {"X-Auth-Token", "Cookie"};
  server.collectHeaders(headerKeys, 2);

  // Authentication routes (no auth required)
  server.on("/login", HTTP_GET, handleLogin);
  server.on("/setup-password", HTTP_GET, handleSetupPassword);
  server.on("/api/auth", HTTP_POST, handleApiAuth);
  server.on("/api/auth/status", HTTP_GET, handleApiAuthStatus);
  server.on("/api/auth/setup", HTTP_POST, handleApiAuthSetup);
  server.on("/api/auth/logout", HTTP_POST, handleApiAuthLogout);
  server.on("/api/auth/change-password", HTTP_POST, handleApiAuthChangePassword);
  server.on("/api/auth/reset-password", HTTP_POST, handleApiAuthResetPassword);

  // Define web routes
  server.on("/", handleRoot);
  server.on("/kaspa_logo.bin", handleLogo);  // Serve logo from SD card
  server.on("/debug.txt", handleDebugLog);  // Serve debug log from SD card
  server.on("/api/stats", handleApiStats);  // JSON API for dashboard (read-only, no auth)
  server.on("/api/config", HTTP_GET, handleApiGetConfig);  // Get configuration (read-only, no auth)
  server.on("/api/config", HTTP_POST, handleApiPostConfig);  // Save configuration (auth required)
  server.on("/api/logs", HTTP_GET, handleApiLogs);  // Get system logs (read-only, no auth)
  server.on("/save", HTTP_POST, handleSave);  // Save settings (auth required)
  server.on("/toggle", HTTP_POST, handleToggle);  // Toggle mining (auth required)

  // OTA update page
  server.on("/check-update", HTTP_GET, handleOTAPage);

  // File upload routes
  server.on("/upload", HTTP_GET, handleFileUpload);
  server.on("/upload", HTTP_POST, handleFileUploadComplete, handleFileUploadPost);

  // SD card firmware update routes
  server.on("/api/sd-update/check", HTTP_GET, handleCheckSDUpdate);
  server.on("/api/sd-update/apply", HTTP_POST, handleApplySDUpdate);
  server.on("/api/update-status", HTTP_GET, handleUpdateStatus);

  // Initialize OTA update system (ElegantOTA handles all flash writes)
  setupOTA();

  server.begin();
  Serial.println("HTTP server started");

  // Initialize system logs
  addLog("System initialized", "info");
  addLog("WiFi connected: " + String(WiFi.SSID()), "success");
  addLog("IP address: " + WiFi.localIP().toString(), "info");
  addLog("Web dashboard ready", "success");

  Serial.println("=== Setup Complete ===");
}

void loop() {
  // DIAGNOSTIC: DISABLED - SD logging from loop() causes crashes during mining
  // static int loop_count = 0;
  // if (loop_count < 5) {
  //   char msg[80];
  //   sprintf(msg, "loop() iteration %d on Core %d, uptime=%lu", loop_count, xPortGetCoreID(), millis());
  //   logToSD(msg);
  //   loop_count++;
  // }

  // DIAGNOSTIC: DISABLED - SD logging from loop() causes crashes during mining
  // static unsigned long last_loop_log = 0;
  // unsigned long now = millis();
  // if (now - last_loop_log > 30000 && loop_count >= 5) {
  //   char msg[80];
  //   sprintf(msg, "loop() running on Core %d, uptime=%lu", xPortGetCoreID(), now);
  //   logToSD(msg);
  //   last_loop_log = now;
  // }

  // Handle web server requests (including ElegantOTA)
  server.handleClient();

  // DIAGNOSTIC: DISABLED - SD logging causes crashes
  // if (loop_count < 5) {
  //   logToSD("After server.handleClient()");
  // }

  // TEMPORARILY DISABLED ElegantOTA FOR TESTING
  // ElegantOTA.loop();  // Process ElegantOTA updates

  // Skip all other processing during firmware update
  if (firmwareUpdateInProgress) {
    delay(10);  // Small delay to prevent tight loop
    return;  // Exit loop early - no LVGL, no stratum, nothing
  }

  if (stratumClient.connected()) {
    // DIAGNOSTIC: DISABLED - SD logging causes crashes
    // if (loop_count < 5) {
    //   logToSD("About to call handleStratumMessages()");
    // }
    handleStratumMessages();
    // if (loop_count < 5) {
    //   logToSD("Returned from handleStratumMessages()");
    // }
  }

  // Check if WiFi scan is complete (non-blocking)
  checkWiFiScanComplete();

  // Handle WiFi connection state machine (non-blocking)
  if (wifiState == WIFI_CONNECTING) {
    // Check connection status
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nConnected!");
      wifiState = WIFI_CONNECTED;

      // Update overlay
      if (connectingStatusLabel) {
        lv_label_set_text(connectingStatusLabel, "Connected!");
      }

      // Save credentials to app preferences (for reference)
      Serial.println("Saving credentials to app preferences...");
      preferences.begin("kaspa", false);
      preferences.putString("ssid", selectedSSID);
      preferences.putString("password", pendingPassword);
      preferences.end();

      // Check if WiFi library saved credentials
      Serial.println("Checking WiFi.SSID() after connection...");
      String savedSSID = WiFi.SSID();
      Serial.printf("WiFi.SSID() reports: '%s' (length: %d)\n",
                    savedSSID.c_str(), savedSSID.length());

      // Wait a moment to show success
      delay(1000);

      // Clean up overlay
      if (connectingOverlay) {
        lv_obj_del(connectingOverlay);
        connectingOverlay = nullptr;
      }

      // Show success
      showSaveConfirmation();
      delay(1500);

      // Clean up WiFi menu
      selectedWifiButton = nullptr;
      ssidDisplayLabel = nullptr;
      passwordTextarea = nullptr;
      scanLabel = nullptr;
      wifiList = nullptr;

      if (wifiScanInProgress) {
        WiFi.scanDelete();
        wifiScanInProgress = false;
        wifiScanComplete = false;
      }

      if (wifiConfigMenu) {
        lv_obj_del(wifiConfigMenu);
        wifiConfigMenu = nullptr;
      }

      // Handle wizard vs settings mode
      if (wizardStep == 2) {
        wizardStep = 3;
        show_wizard_mining_setup();
      } else {
        if (priceLabel) lv_label_set_text(priceLabel, "Loading...");
        if (changeLabel) lv_label_set_text(changeLabel, "Fetching data...");

        #if ENABLE_PRICE_HISTORY
        fetchHistoricalPriceData();
        #endif
        fetch_data();
        #if ENABLE_NETWORK_HASHRATE_CHART
        loadHashrateHistory();
        #endif
        fetchNetworkHashrate();
        #if ENABLE_NETWORK_HASHRATE_CHART
        updateHashrateChart();
        saveHashrateHistory();
        #endif

        showSuccessNotification("WiFi Connected!\nData loading...");
      }

      wifiState = WIFI_IDLE;
    } else if (millis() - wifiConnectStartTime > 10000) {
      // Timeout after 10 seconds
      Serial.println("\nConnection failed!");
      wifiState = WIFI_FAILED;

      if (connectingOverlay) {
        lv_obj_del(connectingOverlay);
        connectingOverlay = nullptr;
      }

      showErrorNotification("Connection failed!\nCheck password and try again");
      wifiState = WIFI_IDLE;
    }
  }

  // FULL UI ENABLED: All features restored
  // PERFORMANCE: Throttle LVGL updates to 20 FPS instead of ~200 FPS for +5% hashrate
  static unsigned long lastLvglUpdate = 0;
  if (millis() - lastLvglUpdate > 50) {  // 50ms = 20 FPS (smooth enough for UI)
    lv_timer_handler();
    lastLvglUpdate = millis();
  }

  delay(5);

  // Check for OTA updates every 24 hours
  loopOTA();

  // Update miner display every 10 seconds (was 5s - optimized for hashrate)
  static unsigned long lastUIUpdate = 0;
  if (millis() - lastUIUpdate > 10000) {
    updateMinerDisplay();
    lastUIUpdate = millis();
  }
  
  // Update miner hashrate history every 5 minutes
  if (millis() - lastMinerHashrateUpdate > 300000) {  // 300000ms = 5 minutes
    minerHashrateHistory[minerHashrateIndex] = currentHashrate;
    minerHashrateTimestamps[minerHashrateIndex] = millis();
    minerHashrateIndex = (minerHashrateIndex + 1) % MINER_HASHRATE_POINTS;
    lastMinerHashrateUpdate = millis();
  }

  // Check pool connection every 15 seconds ONLY if mining is enabled
  static unsigned long lastPoolCheck = 0;
  static unsigned long lastConnectionAttempt = 0;
  static int reconnectDelay = 5000;  // Start with 5 second delay
  
  if (miningEnabled && (millis() - lastPoolCheck > 15000)) {
    static bool wasConnected = false;
    bool isConnected = stratumClient.connected();

    if (!isConnected) {
      // Log disconnection only once
      if (wasConnected) {
        Serial.println("Pool disconnected!");
        addMiningLog("POOL DISCONNECTED", "Connection lost");
        addLog("POOL DISCONNECTED: Connection lost", "error");
        wasConnected = false;

        // Stop mining activity when disconnected (like real ASICs)
        hasJob = false;
        currentHashrate = 0;
        poolConnected = false;
      }

      // Only attempt reconnect if enough time has passed (backoff)
      if (millis() - lastConnectionAttempt > reconnectDelay) {
        Serial.println("Attempting reconnect...");
        connectToPool();
        lastConnectionAttempt = millis();

        // Increase backoff delay up to 60 seconds
        reconnectDelay = min(reconnectDelay + 5000, 60000);
      }
    } else {
      // Log connection only once
      if (!wasConnected) {
        wasConnected = true;
      }
      // Connected successfully, reset backoff delay
      reconnectDelay = 5000;
    }
    lastPoolCheck = millis();
  }

  // Update every 5 minutes (300000 ms) for testing, change to 1800000 for 30min
  if (millis() - last_update > 300000) {  // 5 minutes for testing
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\n--- Fetching new data ---");
      Serial.printf("Free heap before fetch: %d bytes\n", ESP.getFreeHeap());

      fetch_data();
      delay(5000);
      fetchNetworkHashrate();
      #if ENABLE_NETWORK_HASHRATE_CHART
      updateHashrateChart();
      saveHashrateHistory();
      #endif

      Serial.printf("Free heap after fetch: %d bytes\n", ESP.getFreeHeap());
      // Show time until next update
      Serial.printf("Next update in 5 minutes at: %lu ms\n", millis() + 300000);
    } else {
      Serial.println("\n--- Skipping data fetch - WiFi not connected ---");
      Serial.println("! Connect to 'KASDeck' WiFi (password: kaspa123) to configure network");
    }
    last_update = millis();
  }

  // Periodic memory status report every 30 minutes (no cleanup - causes crashes)
  static unsigned long lastMemoryReport = 0;
  if (millis() - lastMemoryReport > 1800000) {  // 30 minutes
    Serial.println("\n=== Memory Status Report ===");
    Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());

    // Report LVGL memory usage
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    Serial.printf("LVGL memory - Used: %d, Free: %d, Frag: %d%%\n",
                  mon.used_cnt, mon.free_cnt, mon.frag_pct);

    lastMemoryReport = millis();
  }
}