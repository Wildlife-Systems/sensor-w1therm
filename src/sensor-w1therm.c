/*
 * sensor-w1therm - Read w1_therm 1-Wire temperature sensors
 *
 * Part of the WildlifeSystems project.
 * https://wildlife.systems
 *
 * Author: Ed Baker <ed@ebaker.me.uk>
 * License: MIT
 *
 * This program reads temperature data from 1-Wire temperature sensors
 * supported by the Linux kernel w1_therm driver interface.
 *
 * Supported sensors:
 *   - DS18S20 (family code 0x10)
 *   - DS1822  (family code 0x22)
 *   - DS18B20 (family code 0x28)
 *   - DS1825  (family code 0x3B)
 *   - DS28EA00 (family code 0x42)
 *
 * References:
 *   https://docs.kernel.org/w1/slaves/w1_therm.html
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <ctype.h>
#include <pthread.h>
#include <ws_utils.h>

#define W1_DEVICES_PATH "/sys/bus/w1/devices"
#define MAX_SENSORS 64
#define MAX_PATH_LEN 512
#define MAX_LINE_LEN 256

/* Config file path */
#define CONFIG_PATH "/etc/ws/sensors/w1therm.json"

/* Boot config paths (Raspberry Pi) */
#define BOOT_CONFIG_PATH "/boot/firmware/config.txt"
#define BOOT_CONFIG_PATH_LEGACY "/boot/config.txt"

/* w1-gpio overlay configuration */
#define W1_OVERLAY_LINE "dtoverlay=w1-gpio,gpiopin=17,pullup=1"

/* Version information - passed via -DVERSION from Makefile (extracted from debian/changelog) */
#ifndef VERSION
#define VERSION "unknown"
#endif

/* Error values in millidegrees */
#define STARTUP_VALUE_RAW 85000  /* 85.000°C startup/error value */
#define INSUFFICIENT_POWER_RAW 127937  /* 127.937°C in millidegrees */

/* Sensor reading result */
typedef struct {
    char sensor_id[64];
    char sensor_type[16];
    double temperature;
    int has_error;
    char error_msg[128];
    int valid;
} sensor_result_t;

/* Thread arguments */
typedef struct {
    char folder_path[MAX_PATH_LEN];
    sensor_result_t *result;
} thread_args_t;

/* Sensor configuration from config file */
typedef struct {
    char *hw_id;        /* Hardware ID to match (e.g., "28-00000a1b2c3d") */
    char *sensor_id;    /* Override sensor_id in output */
    char *sensor_name;  /* Human-readable name */
    int internal;       /* 1 if internal, 0 if external */
} sensor_config_t;

/* Supported w1_therm family codes */
static const char *W1_THERM_FAMILY_CODES[] = {
    "10-",  /* W1_THERM_DS18S20 */
    "22-",  /* W1_THERM_DS1822 */
    "28-",  /* W1_THERM_DS18B20 */
    "3b-",  /* W1_THERM_DS1825 */
    "42-",  /* W1_THERM_DS28EA00 */
    NULL
};

/* Sensor type names corresponding to family codes */
static const char *W1_THERM_SENSOR_NAMES[] = {
    "ds18s20",
    "ds1822",
    "ds18b20",
    "ds1825",
    "ds28ea00",
    "unknown"
};

/*
 * Get sensor type name from sensor ID
 */
static const char *get_sensor_type(const char *sensor_id) {
    int i;
    for (i = 0; W1_THERM_FAMILY_CODES[i] != NULL; i++) {
        if (strncmp(sensor_id, W1_THERM_FAMILY_CODES[i], 3) == 0) {
            return W1_THERM_SENSOR_NAMES[i];
        }
    }
    return "unknown";
}

/*
 * Parse a simple JSON config file - returns dynamically allocated array
 */
static sensor_config_t *load_config(const char *path, int *count) {
    *count = 0;
    
    char *buffer = ws_read_file(path, NULL);
    if (!buffer) return NULL;
    
    int sensor_count = ws_json_count_objects(buffer);
    if (sensor_count == 0) { free(buffer); return NULL; }
    
    sensor_config_t *configs = malloc(sensor_count * sizeof(sensor_config_t));
    if (!configs) { free(buffer); return NULL; }
    
    char *ptr = buffer;
    int sensor_idx = 0;
    while ((ptr = strchr(ptr, '{')) != NULL && sensor_idx < sensor_count) {
        char *end = strchr(ptr, '}');
        if (!end) break;
        
        configs[sensor_idx].internal = ws_json_parse_bool(ptr, end, "internal", 0);
        configs[sensor_idx].hw_id = ws_json_parse_string(ptr, end, "hw_id");
        configs[sensor_idx].sensor_id = ws_json_parse_string(ptr, end, "sensor_id");
        configs[sensor_idx].sensor_name = ws_json_parse_string(ptr, end, "sensor_name");
        
        sensor_idx++;
        ptr = end + 1;
    }
    
    free(buffer);
    *count = sensor_idx;
    return configs;
}

/*
 * Free config array
 */
static void free_config(sensor_config_t *configs, int count) {
    if (configs) {
        for (int i = 0; i < count; i++) {
            free(configs[i].hw_id);
            free(configs[i].sensor_id);
            free(configs[i].sensor_name);
        }
        free(configs);
    }
}

/*
 * Find config for a sensor by hardware ID
 */
static sensor_config_t *find_sensor_config(sensor_config_t *configs, int count, const char *hw_id) {
    if (!configs || !hw_id) return NULL;
    for (int i = 0; i < count; i++) {
        if (configs[i].hw_id && strcmp(configs[i].hw_id, hw_id) == 0) {
            return &configs[i];
        }
    }
    return NULL;
}

/*
 * Check if a directory entry is a w1_bus_master folder
 */
static int is_w1_master_folder(const struct dirent *entry) {
    return (entry->d_type == DT_LNK || entry->d_type == DT_DIR) &&
           strncmp(entry->d_name, "w1_bus_master", 13) == 0;
}

/*
 * Check if a directory entry is a w1_therm sensor folder
 */
static int is_w1therm_folder(const struct dirent *entry) {
    const char **family;
    
    if (entry->d_type != DT_LNK && entry->d_type != DT_DIR) {
        return 0;
    }
    
    for (family = W1_THERM_FAMILY_CODES; *family != NULL; family++) {
        if (strncmp(entry->d_name, *family, 3) == 0) {
            return 1;
        }
    }
    return 0;
}

/*
 * Trigger bulk read on a w1_bus_master
 * This sends the convert command to ALL sensors on the bus simultaneously,
 * which is much faster than reading each sensor individually.
 * 
 * Note: The write blocks until conversion is complete (~750ms for 12-bit).
 */
static int trigger_bulk_read(const char master_path[MAX_PATH_LEN]) {
    char file_path[MAX_PATH_LEN];
    int fd;
    ssize_t written;

    snprintf(file_path, sizeof(file_path), "%.480s/therm_bulk_read", master_path);

    fd = open(file_path, O_WRONLY | O_SYNC);
    if (fd < 0) {
        return -1;  /* Bulk read not supported or permission denied */
    }

    /* Write "trigger\n" - the newline is required */
    written = write(fd, "trigger\n", 8);
    fsync(fd);
    close(fd);

    if (written != 8) {
        return -1;
    }

    return 0;
}

/*
 * Enable poll-for-conversion feature on a sensor
 * This makes the driver poll the bus for conversion completion
 * instead of waiting the full default conversion time.
 * Feature bit 1: Check for conversion errors (85.00 or 127.94)
 * Feature bit 2: Poll for conversion completion (faster reads)
 */
static int enable_poll_feature(const char sensor_path[MAX_PATH_LEN]) {
    char file_path[MAX_PATH_LEN];
    FILE *fp;

    snprintf(file_path, sizeof(file_path), "%.500s/features", sensor_path);

    fp = fopen(file_path, "w");
    if (!fp) {
        return -1;  /* Feature not supported or permission denied */
    }

    /* Enable both features: error checking (1) + poll for completion (2) = 3 */
    if (fprintf(fp, "3") < 0) {
        fclose(fp);
        return -1;
    }

    fclose(fp);
    return 0;
}

/*
 * Auto-measure and set conversion time for a sensor
 * Writing '1' to conv_time tells the driver to measure actual conversion time
 */
static int auto_set_conv_time(const char sensor_path[MAX_PATH_LEN]) {
    char file_path[MAX_PATH_LEN];
    FILE *fp;

    snprintf(file_path, sizeof(file_path), "%.500s/conv_time", sensor_path);

    fp = fopen(file_path, "w");
    if (!fp) {
        return -1;  /* Not supported or permission denied */
    }

    /* Write '1' to trigger auto-measurement of conversion time */
    if (fprintf(fp, "1") < 0) {
        fclose(fp);
        return -1;
    }

    fclose(fp);
    return 0;
}

/*
 * Find all w1_bus_master paths
 */
static int find_masters(char masters[][MAX_PATH_LEN], int max_count) {
    DIR *dir;
    struct dirent *entry;
    int count = 0;

    dir = opendir(W1_DEVICES_PATH);
    if (!dir) {
        return 0;
    }

    while ((entry = readdir(dir)) != NULL && count < max_count) {
        if (is_w1_master_folder(entry)) {
            snprintf(masters[count], MAX_PATH_LEN, "%s/%s", W1_DEVICES_PATH, entry->d_name);
            count++;
        }
    }

    closedir(dir);
    return count;
}

/*
 * Read temperature from a sensor's w1_slave file
 * Returns the temperature in millidegrees, or sets error on failure
 */
static int read_w1_slave(const char *folder_path, long *temp_raw) {
    char file_path[MAX_PATH_LEN];
    FILE *fp;
    char line1[MAX_LINE_LEN];
    char line2[MAX_LINE_LEN];
    char *temp_pos;

    snprintf(file_path, sizeof(file_path), "%s/w1_slave", folder_path);

    fp = fopen(file_path, "r");
    if (!fp) {
        return -1;
    }

    /* Read first line - contains CRC check */
    if (!fgets(line1, sizeof(line1), fp)) {
        fclose(fp);
        return -1;
    }

    /* Check for CRC YES/NO */
    if (!strstr(line1, "YES")) {
        fclose(fp);
        return -1;  /* CRC check failed */
    }

    /* Read second line - contains temperature */
    if (!fgets(line2, sizeof(line2), fp)) {
        fclose(fp);
        return -1;
    }

    fclose(fp);

    /* Find t= in the second line */
    temp_pos = strstr(line2, "t=");
    if (!temp_pos) {
        return -1;
    }

    /* Parse temperature value */
    *temp_raw = strtol(temp_pos + 2, NULL, 10);
    return 0;
}

/*
 * Alternative: read temperature directly from 'temperature' sysfs entry
 * This is simpler and returns temperature in millidegrees directly
 */
static int read_temperature_file(const char *folder_path, long *temp_raw) {
    char file_path[MAX_PATH_LEN];
    FILE *fp;
    char line[MAX_LINE_LEN];

    snprintf(file_path, sizeof(file_path), "%s/temperature", folder_path);

    fp = fopen(file_path, "r");
    if (!fp) {
        return -1;
    }

    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return -1;
    }

    fclose(fp);

    *temp_raw = strtol(line, NULL, 10);
    return 0;
}

/*
 * Thread worker function to read a sensor
 */
static void *read_sensor_thread(void *arg) {
    thread_args_t *args = (thread_args_t *)arg;
    sensor_result_t *result = args->result;
    long temp_raw = 0;
    char *sensor_id;

    /* Extract sensor ID from folder path */
    sensor_id = strrchr(args->folder_path, '/');
    if (sensor_id) {
        sensor_id++;  /* Skip the '/' */
    } else {
        sensor_id = args->folder_path;
    }
    strncpy(result->sensor_id, sensor_id, sizeof(result->sensor_id) - 1);
    result->sensor_id[sizeof(result->sensor_id) - 1] = '\0';

    /* Determine sensor type from family code */
    strncpy(result->sensor_type, get_sensor_type(sensor_id), sizeof(result->sensor_type) - 1);
    result->sensor_type[sizeof(result->sensor_type) - 1] = '\0';

    /* Try reading from temperature file first (simpler interface) */
    if (read_temperature_file(args->folder_path, &temp_raw) != 0) {
        /* Fall back to w1_slave file */
        if (read_w1_slave(args->folder_path, &temp_raw) != 0) {
            result->has_error = 1;
            snprintf(result->error_msg, sizeof(result->error_msg), "Failed to read sensor");
            result->valid = 0;
            return NULL;
        }
    }

    result->valid = 1;

    /* Check for startup value (85.000°C = 85000 millidegrees) */
    if (temp_raw == STARTUP_VALUE_RAW) {
        result->has_error = 1;
        snprintf(result->error_msg, sizeof(result->error_msg), "Sensor has startup value.");
        result->temperature = 85.0;
        return NULL;
    }

    /* Check for insufficient power value (127.937°C) */
    if (temp_raw == INSUFFICIENT_POWER_RAW) {
        result->has_error = 1;
        snprintf(result->error_msg, sizeof(result->error_msg), "Insufficient power");
        result->temperature = (double)temp_raw / 1000.0;
        return NULL;
    }

    /* Convert millidegrees to degrees */
    result->temperature = (double)temp_raw / 1000.0;
    result->has_error = 0;

    return NULL;
}

/*
 * Print a single sensor result as JSON using library helper
 */
static void print_sensor_json(const sensor_result_t *result, int is_first, sensor_config_t *config) {
    char json[2048];
    const char *sensor_id_to_use;
    const char *sensor_name = NULL;
    bool internal = false;
    time_t now = time(NULL);

    if (!is_first) {
        printf(",");
    }

    /* Determine sensor_id: use config override or default to hardware ID */
    sensor_id_to_use = (config && config->sensor_id) ? config->sensor_id : result->sensor_id;
    
    if (config) {
        sensor_name = config->sensor_name;
        internal = config->internal;
    }
    
    /* Build base JSON with common fields */
    if (ws_build_sensor_json_base(json, sizeof(json),
                                   result->sensor_type, "temperature", "Celsius",
                                   sensor_id_to_use, sensor_name,
                                   internal, now) != 0) {
        return;
    }

    /* Add value or error */
    if (result->has_error) {
        ws_sensor_json_set_error(json, result->error_msg);
    }
    ws_sensor_json_set_value(json, result->temperature, 3);

    printf("%s", json);
}

/*
 * Check if w1-gpio overlay is already enabled in config.txt
 * Returns: 1 if found, 0 if not found, -1 on error
 */
static int check_w1_overlay_enabled(const char *config_path) {
    FILE *fp;
    char line[MAX_LINE_LEN];

    fp = fopen(config_path, "r");
    if (!fp) {
        return -1;
    }

    while (fgets(line, sizeof(line), fp)) {
        /* Skip comments and whitespace */
        char *p = line;
        while (*p && isspace(*p)) p++;
        if (*p == '#' || *p == '\0' || *p == '\n') continue;

        /* Check for w1-gpio overlay */
        if (strstr(p, "dtoverlay=w1-gpio") != NULL) {
            fclose(fp);
            return 1;
        }
    }

    fclose(fp);
    return 0;
}

/*
 * Enable 1-Wire interface by adding overlay to config.txt
 * Returns: 0 on success, 1 on error
 */
static int enable_w1_interface(void) {
    const char *config_path = NULL;
    FILE *fp;
    int has_all_section = 0;
    char line[MAX_LINE_LEN];

    /* Check which config file exists */
    if (access(BOOT_CONFIG_PATH, F_OK) == 0) {
        config_path = BOOT_CONFIG_PATH;
    } else if (access(BOOT_CONFIG_PATH_LEGACY, F_OK) == 0) {
        config_path = BOOT_CONFIG_PATH_LEGACY;
    } else {
        fprintf(stderr, "Error: Could not find config.txt at %s or %s\n",
                BOOT_CONFIG_PATH, BOOT_CONFIG_PATH_LEGACY);
        return 1;
    }

    /* Check if already enabled */
    int status = check_w1_overlay_enabled(config_path);
    if (status == 1) {
        printf("1-Wire interface is already enabled in %s\n", config_path);
        printf("If sensors are not detected, please reboot the system.\n");
        return 0;
    }
    if (status == -1) {
        fprintf(stderr, "Error: Could not read %s (permission denied?)\n", config_path);
        return 1;
    }

    /* Check for [all] section */
    fp = fopen(config_path, "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            char *p = line;
            while (*p && isspace(*p)) p++;
            if (strncmp(p, "[all]", 5) == 0) {
                has_all_section = 1;
                break;
            }
        }
        fclose(fp);
    }

    /* Append the overlay configuration */
    fp = fopen(config_path, "a");
    if (!fp) {
        fprintf(stderr, "Error: Could not write to %s (need root?)\n", config_path);
        return 1;
    }

    if (!has_all_section) {
        fprintf(fp, "\n[all]\n");
    }
    fprintf(fp, "# 1-Wire interface for temperature sensors (added by sensor-w1therm)\n");
    fprintf(fp, "%s\n", W1_OVERLAY_LINE);
    fclose(fp);

    printf("1-Wire interface enabled in %s\n", config_path);
    printf("\n*** REBOOT REQUIRED ***\n");
    printf("Please reboot the system for changes to take effect:\n");
    printf("  sudo reboot\n\n");

    return 0;
}

/*
 * Find all w1_therm sensor folders
 */
static int find_sensors(char folders[][MAX_PATH_LEN], int max_count) {
    DIR *dir;
    struct dirent *entry;
    int count = 0;

    dir = opendir(W1_DEVICES_PATH);
    if (!dir) {
        return 0;
    }

    while ((entry = readdir(dir)) != NULL && count < max_count) {
        if (is_w1therm_folder(entry)) {
            snprintf(folders[count], MAX_PATH_LEN, "%s/%s", W1_DEVICES_PATH, entry->d_name);
            count++;
        }
    }

    closedir(dir);
    return count;
}

/*
 * Main entry point
 */
int main(int argc, char *argv[]) {
    char folders[MAX_SENSORS][MAX_PATH_LEN];
    char masters[8][MAX_PATH_LEN];  /* w1_bus_master paths */
    sensor_result_t results[MAX_SENSORS];
    thread_args_t thread_args[MAX_SENSORS];
    pthread_t threads[MAX_SENSORS];
    int sensor_count;
    int master_count;
    int i;
    int output_count = 0;
    int is_first = 1;
    ws_location_filter_t location_filter = WS_LOCATION_ALL;
    sensor_config_t *configs = NULL;
    int config_count = 0;

    /* Handle command-line arguments */
    if (argc > 1) {
        if (strcmp(argv[1], "identify") == 0) {
            ws_cmd_identify();
        } else if (strcmp(argv[1], "list") == 0) {
            ws_cmd_list_single("temperature");
        } else if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0 ||
                   strcmp(argv[1], "version") == 0) {
            ws_print_version("sensor-w1therm", VERSION);
            return WS_EXIT_SUCCESS;
        } else if (strcmp(argv[1], "enable") == 0) {
            return enable_w1_interface();
        } else if (strcmp(argv[1], "internal") == 0) {
            location_filter = WS_LOCATION_INTERNAL;
        } else if (strcmp(argv[1], "external") == 0) {
            location_filter = WS_LOCATION_EXTERNAL;
        } else if (strcmp(argv[1], "setup") == 0) {
            /* Handled below after finding sensors */
        } else if (strcmp(argv[1], "mock") == 0) {
            /* Output mock data for testing without hardware */
            char *serial = ws_get_serial_with_suffix("w1therm_mock");
            time_t now = time(NULL);
            char json[2048];
            if (ws_build_sensor_json_base(json, sizeof(json), "ds18b20", "temperature", "Celsius",
                                          serial, "Mock DS18B20", false, now) == 0) {
                ws_sensor_json_set_value(json, 21.375, 3);
                printf("[%s]\n", json);
            }
            free(serial);
            return WS_EXIT_SUCCESS;
        } else if (strcmp(argv[1], "all") != 0) {
            fprintf(stderr, "Unknown command: %s\n", argv[1]);
            fprintf(stderr, "Usage: sensor-w1therm [--version|identify|list|setup|enable|mock|internal|external|all]\n");
            return WS_EXIT_INVALID_ARG;
        }
    }

    /* Find all w1_therm sensors */
    sensor_count = find_sensors(folders, MAX_SENSORS);

    /*
     * Handle 'setup' command - configure sensors for fastest possible readings
     * This enables poll-for-completion and auto-measures conversion time.
     * Run once after boot or when sensors are added.
     */
    if (argc > 1 && strcmp(argv[1], "setup") == 0) {
        if (sensor_count == 0) {
            fprintf(stderr, "No sensors found to configure.\n");
            return 1;
        }
        printf("Configuring %d sensor(s) for optimized reading...\n", sensor_count);
        for (i = 0; i < sensor_count; i++) {
            char *sensor_id = strrchr(folders[i], '/');
            sensor_id = sensor_id ? sensor_id + 1 : folders[i];
            
            if (enable_poll_feature(folders[i]) == 0) {
                printf("  %s: enabled poll-for-completion\n", sensor_id);
            } else {
                printf("  %s: poll feature not available (may need root)\n", sensor_id);
            }
            
            if (auto_set_conv_time(folders[i]) == 0) {
                printf("  %s: auto-measuring conversion time...\n", sensor_id);
            }
        }
        printf("Setup complete. Sensors are now optimized for fast reading.\n");
        return WS_EXIT_SUCCESS;
    }

    if (sensor_count == 0) {
        printf("[]\n");
        fprintf(stderr, "Warning: No w1_therm sensors detected. Please check your wiring and ensure 1-Wire is enabled.\n");
        return WS_EXIT_SUCCESS;
    }

    /* Load config file if it exists */
    configs = load_config(CONFIG_PATH, &config_count);

    /* Initialize results */
    memset(results, 0, sizeof(results));

    /*
     * OPTIMIZATION: Use bulk read for fastest possible reading
     * 
     * Traditional approach: Each sensor read triggers a conversion (~750ms for 12-bit)
     * Reading N sensors sequentially = N * 750ms
     * 
     * Bulk read approach: Trigger conversion on ALL sensors simultaneously,
     * wait once for completion, then read cached values.
     * Reading N sensors = 750ms + N * (fast read time)
     * 
     * This is dramatically faster for multiple sensors!
     * 
     * Note: The trigger write blocks until conversion is complete, so no
     * additional polling is needed.
     */
    master_count = find_masters(masters, 8);
    
    if (master_count > 0) {
        /* Trigger bulk conversion on all masters - this blocks until complete */
        for (i = 0; i < master_count; i++) {
            if (trigger_bulk_read(masters[i]) != 0) {
                fprintf(stderr, "Warning: Failed to trigger bulk read on %s (may need root)\n", masters[i]);
            }
        }
    }

    /*
     * After bulk read, reading the 'temperature' file returns the cached
     * converted value without triggering a new conversion - this is very fast.
     * 
     * We read in parallel for even more speed when there are many sensors.
     */
    for (i = 0; i < sensor_count; i++) {
        thread_args[i].result = &results[i];
        snprintf(thread_args[i].folder_path, sizeof(thread_args[i].folder_path), "%.511s", folders[i]);

        if (pthread_create(&threads[i], NULL, read_sensor_thread, &thread_args[i]) != 0) {
            /* If thread creation fails, read synchronously */
            read_sensor_thread(&thread_args[i]);
        }
    }

    /* Wait for all threads to complete */
    for (i = 0; i < sensor_count; i++) {
        pthread_join(threads[i], NULL);
    }

    /* Output JSON array */
    printf("[");
    for (i = 0; i < sensor_count; i++) {
        if (results[i].valid) {
            /* Find config for this sensor */
            sensor_config_t *sensor_cfg = find_sensor_config(configs, config_count, results[i].sensor_id);
            
            /* Apply location filter */
            if (location_filter == WS_LOCATION_INTERNAL && (!sensor_cfg || !sensor_cfg->internal)) {
                continue;  /* Skip non-internal sensors */
            }
            if (location_filter == WS_LOCATION_EXTERNAL && sensor_cfg && sensor_cfg->internal) {
                continue;  /* Skip internal sensors */
            }
            
            print_sensor_json(&results[i], is_first, sensor_cfg);
            is_first = 0;
            output_count++;
        } else if (results[i].sensor_id[0] != '\0') {
            fprintf(stderr, "Warning: Failed to read sensor at %s\n", folders[i]);
        }
    }
    printf("]\n");

    /* Free config memory */
    free_config(configs, config_count);

    if (output_count == 0) {
        fprintf(stderr, "Warning: No w1_therm sensors detected. Please check your wiring and ensure 1-Wire is enabled.\n");
    }

    return WS_EXIT_SUCCESS;
}
