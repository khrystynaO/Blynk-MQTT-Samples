# Blynk MQTT client for STM32F767ZI (Ethernet + Zephyr)

This project demonstrates how to use **STM32F767ZI** (e.g., Nucleo-F767ZI) with **Zephyr RTOS** and **Ethernet** to connect securely to **Blynk.Cloud** using the MQTT protocol. It uses the built-in Zephyr networking stack, mbedTLS for encryption, and reconnects automatically on network failures or internet loss.

---

This example was verified to work with **Ethernet on STM32F767ZI**, but should work with other STM32 boards that have Ethernet support and enough RAM.

Important note: The microcontroller must have at least **128KB RAM** and more than **256KB Flash**.

## Setup Zephyr SDK

Follow the official [Zephyr Getting Started Guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html) to set up your environment.

Clone Zephyr and install Python tools:

```sh
west init -m https://github.com/zephyrproject-rtos/zephyr
cd zephyr
west update
pip install -r zephyr/scripts/requirements.txt
```

## Install Toolchain

Install the official Zephyr SDK:
[https://docs.zephyrproject.org/latest/develop/toolchains/zephyr\_sdk.html](https://docs.zephyrproject.org/latest/develop/toolchains/zephyr_sdk.html)

Or use a compatible `arm-none-eabi` toolchain (if not using the official SDK).

---

## Ethernet Configuration

This example uses native Ethernet via the on-board RMII interface on the Nucleo-F767ZI board. No external modem or Wi-Fi is needed.

Ethernet port is configured with DHCP by default.

---

## Blynk Configuration

Fill in your Blynk.Cloud credentials in `prj.conf`:

```
# Enter your auth token here
CONFIG_CLOUD_BLYNK_AUTH_TOKEN="ZcIf4acV2dO6YCUIIy8TRsUXSpWWXlb4"

```

> Find this information in your **Blynk Device Info** screen.
> Read more: [https://bit.ly/BlynkSimpleAuth](https://bit.ly/BlynkSimpleAuth)

---

## Build and Run

This project uses the standard **Zephyr CMake+West** build system.

```sh
# Build the firmware
west build -b nucleo_f767zi

# Flash firmware (ST-Link is built into the board)
west flash
```

You can also flash using STM32CubeProgrammer or OpenOCD if preferred.

---

## Logs and Debugging

Connect to the board via USB and open a terminal:

```sh
minicom -D /dev/ttyACM0 -b 115200
```

You will see logs such as:

```log
[00:00:14.116,000] <inf> net_mqtt: Connect completed
[00:00:14.326,000] <inf> mqtt_blynk: Received topic: downlink/ds/Power -> 1
[00:00:14.328,000] <inf> mqtt_blynk: Received topic: downlink/ds/Set Temperature -> 23
[00:00:14.330,000] <inf> mqtt_blynk: Temp: 15.00°C, Target: 23.0°C
```

---

## Reconnect Handling

If internet or network is lost (e.g. router restarts), the system automatically:

* Detects disconnection via Zephyr network events
* Reconnects to the MQTT broker after internet is restored
* Resubscribes to Blynk topics

Example disconnection and recovery logs:

```log
<err> net_sock_tls: TLS recv error: -4e
<err> net_mqtt_rx: Transport read error: -5
<inf> mqtt_blynk: MQTT client disconnected -5
<inf> mqtt_blynk: Reconnecting in 5 seconds...
```

---

## Device Behavior

This firmware:

* Subscribes to `downlink/ds/Power` and `downlink/ds/Set Temperature`
* Publishes current state regularly
* Controls internal "thermostat" logic using received parameters
* Logs device state and network status

---

## Further Reading

* [Blynk MQTT API documentation](https://docs.blynk.io/en/blynk.cloud-mqtt-api/device-mqtt-api)
* [Blynk Troubleshooting Guide](https://docs.blynk.io/en/troubleshooting/general-issues)
* [Zephyr Networking API Reference](https://docs.zephyrproject.org/latest/connectivity/networking/index.html)
* [Zephyr Logging System](https://docs.zephyrproject.org/latest/services/logging/index.html)

