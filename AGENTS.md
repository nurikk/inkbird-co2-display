# AGENTS.md - CO2 Display Project

This file provides guidelines for AI coding agents working in this repository.

## Project Overview

- **Platform**: ESP32-C3 (ESP32-C3-DevKitM-1 board)
- **Framework**: ESP-IDF v5.5.0 via PlatformIO
- **Language**: C (embedded)
- **Purpose**: IoT CO2 sensor display device

## Project Structure

```
co2_display/
├── CMakeLists.txt          # ESP-IDF CMake configuration
├── platformio.ini          # PlatformIO project configuration
├── sdkconfig.nodemcu-32s   # ESP-IDF SDK configuration (auto-generated, do not edit manually)
├── src/
│   ├── main.c              # Main application entry point (app_main)
│   └── CMakeLists.txt      # Component CMake config
├── include/                # Project header files
├── lib/                    # Private libraries
└── test/                   # PlatformIO unit tests
```

## Build Commands

| Command | Description |
|---------|-------------|
| `pio run` | Build the project |
| `pio run -t upload` | Build and upload to device |
| `pio run -t clean` | Clean build artifacts |
| `pio run -t fullclean` | Full clean including dependencies |
| `pio device monitor` | Open serial monitor (115200 baud) |
| `pio run -t upload && pio device monitor` | Build, upload, and monitor |
| `pio run -t menuconfig` | Open ESP-IDF configuration menu |

## Testing Commands

| Command | Description |
|---------|-------------|
| `pio test` | Run all unit tests |
| `pio test -f test_<name>` | Run a specific test file |
| `pio test -e nodemcu-32s` | Run tests in specific environment |
| `pio test --verbose` | Run tests with verbose output |

Tests should be placed in the `test/` directory with names prefixed by `test_`.

## Static Analysis

| Command | Description |
|---------|-------------|
| `pio check` | Run static code analysis (cppcheck, clangtidy) |
| `pio check --skip-packages` | Check only project source |
| `pio check -f src/main.c` | Check specific file |

## Code Style Guidelines

### Include Order

Organize includes in this order, separated by blank lines:

```c
// 1. Standard C library headers
#include <stdio.h>
#include <string.h>

// 2. SDK configuration (always include before ESP-IDF headers)
#include "sdkconfig.h"

// 3. FreeRTOS headers
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

// 4. ESP-IDF component headers
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"

// 5. Project-specific headers
#include "my_component.h"
```

### Naming Conventions

| Element | Convention | Example |
|---------|------------|---------|
| Functions | `snake_case` | `read_sensor_data()` |
| Variables | `snake_case` | `sensor_value` |
| Constants/Macros | `UPPER_SNAKE_CASE` | `MAX_RETRY_COUNT` |
| Typedefs | `snake_case_t` suffix | `sensor_config_t` |
| Static globals | `s_` prefix | `s_sensor_handle` |
| Global tags | module name | `static const char *TAG = "main";` |

### Formatting

- **Brace style**: K&R (opening brace on same line)
- **Indentation**: 4 spaces (no tabs)
- **Line length**: Max 120 characters
- **Pointer style**: `type *name` (space before asterisk)

```c
void example_function(int param)
{
    if (condition) {
        // code
    } else {
        // code
    }

    for (int i = 0; i < count; i++) {
        // code
    }
}
```

### Error Handling

Always check return values from ESP-IDF functions:

```c
// Use esp_err_t for return values
esp_err_t init_sensor(void)
{
    esp_err_t ret = some_esp_function();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize: %s", esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}

// Use ESP_ERROR_CHECK for critical failures (will abort on error)
ESP_ERROR_CHECK(nvs_flash_init());

// Use ESP_RETURN_ON_ERROR for cleaner error propagation
ESP_RETURN_ON_ERROR(gpio_set_direction(pin, GPIO_MODE_OUTPUT), TAG, "GPIO setup failed");
```

### Logging

Use ESP-IDF logging macros with a TAG:

```c
static const char *TAG = "module_name";

ESP_LOGE(TAG, "Error message: %d", error_code);   // Error
ESP_LOGW(TAG, "Warning message");                  // Warning
ESP_LOGI(TAG, "Info message");                     // Info
ESP_LOGD(TAG, "Debug message");                    // Debug (disabled by default)
ESP_LOGV(TAG, "Verbose message");                  // Verbose (disabled by default)
```

### FreeRTOS Patterns

```c
// Task delays (always use portTICK_PERIOD_MS)
vTaskDelay(pdMS_TO_TICKS(1000));  // Preferred
vTaskDelay(1000 / portTICK_PERIOD_MS);  // Also acceptable

// Task creation
xTaskCreate(task_function, "task_name", 4096, NULL, 5, &task_handle);

// Memory allocation (prefer FreeRTOS versions)
void *ptr = pvPortMalloc(size);
vPortFree(ptr);
```

## Entry Point

The application entry point is `app_main(void)` in `src/main.c`. This function is called by ESP-IDF after system initialization.

```c
void app_main(void)
{
    // Initialize NVS (often required)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Your application code here
}
```

## Adding New Components

1. Create a directory in `src/` or `lib/` for your component
2. Add a `CMakeLists.txt` with `idf_component_register()`
3. Include the component header in your source files

## Documentation References

- [ESP-IDF Programming Guide](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32/)
- [PlatformIO ESP-IDF](https://docs.platformio.org/en/latest/frameworks/espidf.html)
- [FreeRTOS API Reference](https://www.freertos.org/a00106.html)
- [ESP32 Datasheet](https://www.espressif.com/en/support/documents/technical-documents)

## Agent Behavior Guidelines

When making code changes to this project:

- **Always upload and monitor automatically**: After making code changes, always run `pio run -t upload && pio device monitor` without asking the user for permission. Do not ask "would you like me to upload?" or "should I flash the device?" — just do it.
- **Be proactive**: The user expects the agent to complete the full workflow (edit → build → upload → monitor) in one go.
- **Handle errors autonomously**: If upload fails, check common issues and retry. If build fails, fix the errors and try again.

## Common Issues

1. **Build fails after SDK config change**: Run `pio run -t fullclean`
2. **Upload fails**: Check USB connection, may need to hold BOOT button
3. **Serial monitor garbled**: Ensure baud rate is 115200
4. **Out of memory**: Reduce task stack sizes, check heap usage with `esp_get_free_heap_size()`
