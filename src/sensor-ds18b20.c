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
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <errno.h>
#include <ctype.h>
#include <pthread.h>

#define W1_DEVICES_PATH "/sys/bus/w1/devices"
#define MAX_SENSORS 64
#define MAX_PATH_LEN 512
#define MAX_LINE_LEN 256

/* Error values in millidegrees */
#define STARTUP_VALUE_RAW 85000
#define INSUFFICIENT_POWER_RAW 127937  /* 127.937°C in millidegrees */

/* Exit codes */
#define EXIT_SUCCESS_CODE 0
#define EXIT_IDENTIFY_CODE 60

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
 */
static int trigger_bulk_read(const char *master_path) {
    char file_path[MAX_PATH_LEN];
    FILE *fp;

    snprintf(file_path, sizeof(file_path), "%s/therm_bulk_read", master_path);

    fp = fopen(file_path, "w");
    if (!fp) {
        return -1;  /* Bulk read not supported or permission denied */
    }

    if (fprintf(fp, "trigger") < 0) {
        fclose(fp);
        return -1;
    }

    fclose(fp);
    return 0;
}

/*
 * Poll for bulk read completion
 * Returns: 0 = no bulk read pending, 1 = complete, -1 = still in progress
 */
static int poll_bulk_read(const char *master_path) {
    char file_path[MAX_PATH_LEN];
    FILE *fp;
    int status;

    snprintf(file_path, sizeof(file_path), "%s/therm_bulk_read", master_path);

    fp = fopen(file_path, "r");
    if (!fp) {
        return 0;  /* No bulk read support */
    }

    if (fscanf(fp, "%d", &status) != 1) {
        fclose(fp);
        return 0;
    }

    fclose(fp);
    return status;
}

/*
 * Wait for bulk read to complete with timeout
 * Returns 0 on success, -1 on timeout
 */
static int wait_for_bulk_read(const char *master_path, int timeout_ms) {
    int elapsed = 0;
    int poll_interval_us = 10000;  /* 10ms poll interval */
    int status;

    while (elapsed < timeout_ms) {
        status = poll_bulk_read(master_path);
        if (status >= 0) {
            return 0;  /* Conversion complete or no pending */
        }
        usleep(poll_interval_us);
        elapsed += poll_interval_us / 1000;
    }

    return -1;  /* Timeout */
}

/*
 * Enable poll-for-conversion feature on a sensor
 * This makes the driver poll the bus for conversion completion
 * instead of waiting the full default conversion time.
 * Feature bit 1: Check for conversion errors (85.00 or 127.94)
 * Feature bit 2: Poll for conversion completion (faster reads)
 */
static int enable_poll_feature(const char *sensor_path) {
    char file_path[MAX_PATH_LEN];
    FILE *fp;

    snprintf(file_path, sizeof(file_path), "%s/features", sensor_path);

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
static int auto_set_conv_time(const char *sensor_path) {
    char file_path[MAX_PATH_LEN];
    FILE *fp;

    snprintf(file_path, sizeof(file_path), "%s/conv_time", sensor_path);

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
    struct timeval tv_start, tv_end;

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

    gettimeofday(&tv_start, NULL);

    /* Try reading from temperature file first (simpler interface) */
    if (read_temperature_file(args->folder_path, &temp_raw) != 0) {
        /* Fall back to w1_slave file */
        fprintf(stderr, "Debug: temperature file failed for %s, trying w1_slave\n", sensor_id);
        if (read_w1_slave(args->folder_path, &temp_raw) != 0) {
            result->has_error = 1;
            snprintf(result->error_msg, sizeof(result->error_msg), "Failed to read sensor");
            result->valid = 0;
            return NULL;
        }
    }

    gettimeofday(&tv_end, NULL);
    long read_ms = (tv_end.tv_sec - tv_start.tv_sec) * 1000 + 
                   (tv_end.tv_usec - tv_start.tv_usec) / 1000;
    fprintf(stderr, "Debug: Read %s took %ldms\n", sensor_id, read_ms);

    result->valid = 1;

    /* Check for startup value (85.000°C = 85000 millidegrees) */
    if (temp_raw == STARTUP_VALUE_RAW) {
        result->has_error = 1;
        snprintf(result->error_msg, sizeof(result->error_msg), "Sensor has startup value.");
        result->temperature = 85.0;
        return NULL;
    }

    /* Check for insufficient power value (~127.94°C) */
    /* The raw value is approximately 127937 millidegrees */
    if (temp_raw >= 127000 && temp_raw <= 128000) {
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
 * Escape a string for JSON output
 */
static void json_escape_string(const char *src, char *dest, size_t dest_size) {
    size_t i = 0, j = 0;
    while (src[i] && j < dest_size - 2) {
        if (src[i] == '"' || src[i] == '\\') {
            dest[j++] = '\\';
        }
        dest[j++] = src[i++];
    }
    dest[j] = '\0';
}

/*
 * Print a single sensor result as JSON
 */
static void print_sensor_json(const sensor_result_t *result, int is_first) {
    char escaped_id[128];
    char escaped_error[256];

    if (!is_first) {
        printf(",");
    }

    json_escape_string(result->sensor_id, escaped_id, sizeof(escaped_id));

    printf("{\"sensor\":\"%s\",\"measures\":\"temperature\",\"unit\":\"Celsius\",\"sensor_id\":\"%s\"",
           result->sensor_type, escaped_id);

    if (result->has_error) {
        json_escape_string(result->error_msg, escaped_error, sizeof(escaped_error));
        printf(",\"error\":\"%s\"", escaped_error);
    } else {
        printf(",\"value\":%.3f", result->temperature);
    }

    printf("}");
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
    int bulk_read_triggered = 0;

    /* Handle 'identify' command */
    if (argc > 1 && strcmp(argv[1], "identify") == 0) {
        return EXIT_IDENTIFY_CODE;
    }

    /* Handle 'list' command */
    if (argc > 1 && strcmp(argv[1], "list") == 0) {
        printf("temperature\n");
        return EXIT_SUCCESS_CODE;
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
        return EXIT_SUCCESS_CODE;
    }

    if (sensor_count == 0) {
        printf("[]\n");
        fprintf(stderr, "Warning: No w1_therm sensors detected. Please check your wiring and ensure 1-Wire is enabled.\n");
        return EXIT_SUCCESS_CODE;
    }

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
     */
    master_count = find_masters(masters, 8);

    if (master_count == 0) {
        fprintf(stderr, "Warning: No w1_bus_master found, falling back to sequential reads\n");
    }

    struct timeval tv_start, tv_end;
    gettimeofday(&tv_start, NULL);
    
    if (master_count > 0) {
        /* Trigger bulk conversion on all masters */
        for (i = 0; i < master_count; i++) {
            if (trigger_bulk_read(masters[i]) == 0) {
                bulk_read_triggered = 1;
                fprintf(stderr, "Debug: Bulk read triggered on %s\n", masters[i]);
            } else {
                fprintf(stderr, "Warning: Failed to trigger bulk read on %s\n", masters[i]);
            }
        }

        if (bulk_read_triggered) {
            /* Wait for all conversions to complete (max 1000ms for 12-bit resolution) */
            for (i = 0; i < master_count; i++) {
                int wait_result = wait_for_bulk_read(masters[i], 1000);
                gettimeofday(&tv_end, NULL);
                long elapsed_ms = (tv_end.tv_sec - tv_start.tv_sec) * 1000 + 
                                  (tv_end.tv_usec - tv_start.tv_usec) / 1000;
                fprintf(stderr, "Debug: wait_for_bulk_read returned %d after %ldms\n", 
                        wait_result, elapsed_ms);
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
        snprintf(thread_args[i].folder_path, sizeof(thread_args[i].folder_path), "%s", folders[i]);

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
            print_sensor_json(&results[i], is_first);
            is_first = 0;
            output_count++;
        } else if (results[i].sensor_id[0] != '\0') {
            fprintf(stderr, "Warning: Failed to read sensor at %s\n", folders[i]);
        }
    }
    printf("]\n");

    if (output_count == 0) {
        fprintf(stderr, "Warning: No w1_therm sensors detected. Please check your wiring and ensure 1-Wire is enabled.\n");
    }

    return EXIT_SUCCESS_CODE;
}
