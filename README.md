# sensor-w1therm

A C implementation of the w1_therm temperature sensor reader for WildlifeSystems.

## Description

This program reads temperature data from 1-Wire temperature sensors connected to
the system via the Linux kernel `w1_therm` driver interface. It outputs readings in JSON
format compatible with the WildlifeSystems sensor-control framework.

Supported sensors:
- DS18S20 (family code 0x10)
- DS1822 (family code 0x22)
- DS18B20 (family code 0x28)
- DS1825 (family code 0x3B)
- DS28EA00 (family code 0x42)

## Building

```bash
make
```

## Installing

```bash
sudo make install
```

This installs the binary to `/usr/bin/sensor-w1therm` and the man page to
`/usr/share/man/man1/sensor-w1therm.1`.

## Uninstalling

```bash
sudo make uninstall
```

## Usage

### Enable 1-Wire interface (first-time setup)

```bash
sudo sensor-w1therm enable
```

This adds the w1-gpio overlay to `/boot/firmware/config.txt`. A reboot is required.

### Read all connected sensors

```bash
sensor-w1therm
```

The output is a JSON array in the WildlifeSystems format with one reading per
sensor found on the bus. The `node_id` and `deployment_id` fields are filled in
by `sr`.

```json
[{"sensor":"ds18b20","device":"ds18b20","measures":"temperature","value":23.500,"unit":"Celsius","node_id":null,"sensor_id":"28-0123456789ab","sensor_name":null,"location":null,"deployment_id":null,"timestamp":1789225958,"config":null,"internal":false,"error":null}]
```

### Filter by location

```bash
# Read only internal sensors
sensor-w1therm internal

# Read only external sensors
sensor-w1therm external

# Read all sensors explicitly
sensor-w1therm all
```

### Configure sensors for fast reading

```bash
sensor-w1therm setup
```

This is run automatically on boot by the systemd service.

### List supported measurements

```bash
sensor-w1therm list
```

### Identify sensor type (for sensor-control integration)

```bash
sensor-w1therm identify
```

Returns exit code 60.

### Show version

```bash
sensor-w1therm version
```

### Output mock data for testing

```bash
sensor-w1therm mock
```

## Configuration

Configuration is optional and is read from `/etc/ws/sensors/w1therm.json`. Every
sensor found on the bus is reported whether or not it has an entry; an entry
matches a sensor by its 1-Wire hardware identifier and overrides the fields
below.

```json
[
  {
    "hw_id": "28-00000a1b2c3d",
    "sensor_name": "Enclosure",
    "internal": true,
    "location": "{{node}}"
  },
  {
    "hw_id": "28-00000e4f5g6h",
    "sensor_id": "pond_temp",
    "sensor_name": "Pond",
    "location": { "latitude": 51.4967, "longitude": -0.1764, "accuracy": 3.0 }
  }
]
```

- `hw_id`: the 1-Wire hardware identifier to match, as it appears under
  `/sys/bus/w1/devices`.
- `sensor_id`: a custom sensor identifier. If omitted, the hardware identifier
  is used.
- `sensor_name`: a human-readable name, reported in the `sensor_name` field.
- `internal`: whether the sensor is inside the enclosure. The default is
  false.
- `location`: where the sensor is. Either `"{{node}}"` for the position of the
  node, `"{{none}}"` for a sensor that has no position, or an object with
  `latitude` and `longitude` in decimal degrees and, optionally, `altitude`
  and `accuracy` in metres. If omitted, the `location` field of the reading is
  null.

An example showing every option in use is installed as
`/usr/share/doc/sensor-w1therm/examples/w1therm.json`; it is valid JSON and can
be copied into place and edited.

## Hardware Configuration

1. Enable 1-Wire interface:
   ```bash
   sudo sensor-w1therm enable
   ```

2. Reboot the Raspberry Pi

## Sysfs Interface

The program uses the Linux kernel `w1_therm` driver sysfs interface:

- `/sys/bus/w1/devices/*/w1_slave` - Traditional interface with CRC check
- `/sys/bus/w1/devices/*/temperature` - Direct temperature reading in millidegrees
- `/sys/bus/w1/devices/w1_bus_master1/therm_bulk_read` - Bulk conversion trigger

For more information, see the [kernel documentation](https://docs.kernel.org/w1/slaves/w1_therm.html).

## Error Handling

Every sensor found on the bus is reported. A sensor that cannot be read is
still emitted, with `value` null and the reason in `error`, as the other
WildlifeSystems drivers do; a sensor absent from the output was not found on
the bus at all. The reported error conditions are:

- **Failed to read sensor**: neither the `temperature` nor the `w1_slave` sysfs file could be read, typically a permissions problem or a probe that dropped off the bus mid-read
- **Startup value (85.0°C)**: Indicates the sensor has not completed a conversion yet
- **Insufficient power (127.937°C)**: Indicates the kernel driver detected a power or bus error (not that the sensor is completely unpowered)

## Building the Debian Package

From within the sensor-w1therm directory:

```bash
debuild -us -uc -b
```

## Dependencies

- Linux kernel with `w1_therm` driver
- POSIX threads library (pthread)
- GCC compiler

## License

GPL-2.0-or-later

## Documentation

[Comparison with older Bash version](https://reports.ebaker.me.uk/WS-sensor-w1therm.html)

## Author

Ed Baker <ed@ebaker.me.uk>

## Project

Part of the WildlifeSystems project. For more information, visit:
- https://wildlife.systems
- https://docs.wildlife.systems
