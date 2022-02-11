# Custom firmware for Vindriktning sensor with SCD41, DH22 and BMP280

Custom firmware for IKEA Vindriktning sensor with few more sensors
- SCD41 for CO2 measurement
- DHT22 for Temperature and Humidity measurement (mounted outside)
- BMP280 for pressure measurement
- (original) PM1006 for PM2.5 particle measurement

based on aftermarket board from LaskaKit https://www.laskakit.cz/laskakit-esp-vindriktning-esp-32-i2c/ 

Firmware is based on platformio and uses standard arduino framework for ESP32. 
Measurements are shown via built-in RGB leds and published regularly to InfluxDB.
Supports basic OTA (no password!)
Turns LEDs off between 21:30 and 7:30

**Sources:** 
- https://github.com/LaskaKit/ESP-Vindriktning
- examples from used libraries

**TODO:**
- Publish data to MQTT

## Images

![Front view of upgraded sensor](https://github.com/35mmHunter/vindriktning/blob/master/img/front.jpg)
![Rear view of upgraded sensor](https://github.com/35mmHunter/vindriktning/blob/master/img/back.jpg)
