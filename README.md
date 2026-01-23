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

Output example:
```json
[{"sensor":"ds18b20","measures":"temperature","unit":"Celsius","sensor_id":"28-0123456789ab","value":23.500}]
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

## Hardware Configuration

1. Enable 1-Wire interface:
   ```bash
   sudo sensor-w1therm enable
   ```

2. Reboot the Raspberry Pi

3. The 1-Wire kernel modules are loaded automatically.

4. Connected sensors will appear in `/sys/bus/w1/devices/` with their family code prefix.

## Sysfs Interface

The program uses the Linux kernel `w1_therm` driver sysfs interface:

- `/sys/bus/w1/devices/*/w1_slave` - Traditional interface with CRC check
- `/sys/bus/w1/devices/*/temperature` - Direct temperature reading in millidegrees
- `/sys/bus/w1/devices/w1_bus_master1/therm_bulk_read` - Bulk conversion trigger

For more information, see the [kernel documentation](https://docs.kernel.org/w1/slaves/w1_therm.html).

## Error Handling

The program detects and reports the following error conditions:

- **Startup value (85.0°C)**: Indicates the sensor has not completed a conversion yet
- **Insufficient power (~127.94°C)**: Indicates the sensor is not receiving adequate power
- **CRC failure**: Indicates a communication error with the sensor

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

MIT License

## Author

Ed Baker <ed@ebaker.me.uk>

## Project

Part of the WildlifeSystems project. For more information, visit:
- https://wildlife.systems
- https://docs.wildlife.systems
