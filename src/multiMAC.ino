#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

#define MOUNT_POINT "/sdcard"

// Pins from Waveshare demo firmware
#define SDMMC_CLK  14
#define SDMMC_CMD  15
#define SDMMC_D0   16
#define SDMMC_D1   18
#define SDMMC_D2   17
#define SDMMC_D3   21

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Initializing SD_MMC from demo pinout...");

  // SDMMC host config
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.flags = SDMMC_HOST_FLAG_4BIT;
  host.max_freq_khz = SDMMC_FREQ_DEFAULT;

  // Slot config with correct types
  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  slot_config.width = 4;
  slot_config.clk = (gpio_num_t)SDMMC_CLK;
  slot_config.cmd = (gpio_num_t)SDMMC_CMD;
  slot_config.d0  = (gpio_num_t)SDMMC_D0;
  slot_config.d1  = (gpio_num_t)SDMMC_D1;
  slot_config.d2  = (gpio_num_t)SDMMC_D2;
  slot_config.d3  = (gpio_num_t)SDMMC_D3;
  slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  // VFS config
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
    .format_if_mount_failed = false,
    .max_files = 5,
    .allocation_unit_size = 16 * 1024
  };

  sdmmc_card_t* card;
  esp_err_t ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);

  if (ret != ESP_OK) {
    Serial.printf("❌ Mount failed: %s\n", esp_err_to_name(ret));
    return;
  }

  Serial.println("✅ SD card mounted.");
  sdmmc_card_print_info(stdout, card);

  // Write test file
  FILE* f = fopen(MOUNT_POINT "/demo_test.txt", "w");
  if (!f) {
    Serial.println("❌ Failed to open file.");
    return;
  }

  fprintf(f, "Hello from Waveshare demo SD config\n");
  fclose(f);
  Serial.println("✅ Write success.");
}

void loop() {}
