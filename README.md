# sensor-ds18b20-c

A C implementation of the DS18B20 temperature sensor reader for WildlifeSystems.

## Description

This program reads temperature data from DS18B20 1-Wire temperature sensors connected to
the system via the Linux kernel `w1_therm` driver interface. It outputs readings in JSON
format compatible with the WildlifeSystems sensor-control framework.

## Building

```bash
make
```

## Installing

```bash
sudo make install
```

This installs the binary to `/usr/bin/sensor-ds18b20` and the man page to
`/usr/share/man/man1/sensor-ds18b20.1`.

## Uninstalling

```bash
sudo make uninstall
```

## Usage

### Read all connected DS18B20 sensors

```bash
sensor-ds18b20
```

Output example:
```json
[{"sensor":"ds18b20","measures":"temperature","unit":"Celsius","sensor_id":"28-0123456789ab","value":23.500}]
```

### List supported measurements

```bash
sensor-ds18b20 list
```

### Identify sensor type (for sensor-control integration)

```bash
sensor-ds18b20 identify
```

Returns exit code 60.

## Hardware Configuration

1. Enable 1-Wire interface on Raspberry Pi by adding to `/boot/config.txt` or `/boot/firmware/config.txt`:
   ```
   dtoverlay=w1-gpio
   ```

2. Reboot the Raspberry Pi

3. Load the 1-Wire kernel modules (usually loaded automatically):
   ```bash
   sudo modprobe w1-gpio
   sudo modprobe w1-therm
   ```

4. Connected DS18B20 sensors will appear in `/sys/bus/w1/devices/28-*/`

## Sysfs Interface

The program uses the Linux kernel `w1_therm` driver sysfs interface:

- `/sys/bus/w1/devices/28-*/w1_slave` - Traditional interface with CRC check
- `/sys/bus/w1/devices/28-*/temperature` - Direct temperature reading in millidegrees

For more information, see the [kernel documentation](https://docs.kernel.org/w1/slaves/w1_therm.html).

## Error Handling

The program detects and reports the following error conditions:

- **Startup value (85.0°C)**: Indicates the sensor has not completed a conversion yet
- **Insufficient power (~127.94°C)**: Indicates the sensor is not receiving adequate power
- **CRC failure**: Indicates a communication error with the sensor

## Building the Debian Package

From within the sensor-ds18b20-c directory:

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
