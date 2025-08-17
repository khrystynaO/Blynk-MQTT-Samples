# Blynk MQTT client for SIWX917y_EK (WIFI + Zephyr)

This project demonstrates how to use **SIWX917y_EK** (e.g., Nucleo-F767ZI) with **Zephyr RTOS** and **WIFI** to connect securely to **Blynk.Cloud** using the MQTT protocol. It uses the built-in Zephyr networking stack, mbedTLS for encryption, and reconnects automatically on network failures or internet loss.

---

This example was verified to work with **WIFI on SIWX917y_EK**, but should work with other STM32 boards that have WIFI support and enough RAM.

#FIXME: all WIFI function tested using sample **shell** and build for siwx917y_ek.

## Setup Zephyr SDK

Follow the official [Zephyr Getting Started Guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html) to set up your environment.

---

## WIFI Configuration

This example uses native WIFI via the on-board RMII interface on the Nucleo-F767ZI board. No external modem or Wi-Fi is needed.

WIFI port is configured with DHCP by default.

---

## Blynk Configuration

To successfully connect to the Blynk we need to specify template id, template name, token and server. It can be done by adding appropriate configurations to **prj.conf** file. for example use next command (with your data):

````
echo 'CONFIG_CLOUD_BLYNK_TEMPLATE_ID="TMPLxxx"' >> prj.conf
echo 'CONFIG_CLOUD_BLYNK_AUTH_TOKEN="***"' >> prj.conf
echo 'CONFIG_CLOUD_BLYNK_TEMPLATE_NAME="sample"' >> prj.conf
echo 'CONFIG_CLOUD_BLYNK_SERVER_ADDR="blynk.cloud"' >> prj.conf
echo 'CONFIG_CLOUD_BLYNK_SERVER_PORT=8883' >> prj.conf

````

> Find this information in your **Blynk Device Info** screen.
> Read more: [https://bit.ly/BlynkSimpleAuth](https://bit.ly/BlynkSimpleAuth)

---

## Build and Run

This project uses the standard **Zephyr CMake+West** build system. But also you need intsall Simpicity Commander, use the **Open Shell** button on the botom of UI and in this shellgo to the folder with project using command **cd** and after that you can flash firmware in chip.

```sh
# Build the firmware
west build -b siwx917y_ek

# Flash firmware (ST-Link is built into the board)
west flash
```

You can also flash using STM32CubeProgrammer or OpenOCD if preferred.

If you can't build for this board please move folder siwx917y_ek from dir boards in sample to zephyr os boards: **zephyr/boards/silabs/dev_kits/**
---

## Logs and Debugging

Connect to the board via USB and open a terminal:

```sh
minicom -D /dev/ttyACM0 -b 115200
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
* OTA updates
---

## Further Reading

* [Blynk MQTT API documentation](https://docs.blynk.io/en/blynk.cloud-mqtt-api/device-mqtt-api)
* [Blynk Troubleshooting Guide](https://docs.blynk.io/en/troubleshooting/general-issues)
* [Zephyr Networking API Reference](https://docs.zephyrproject.org/latest/connectivity/networking/index.html)
* [Zephyr Logging System](https://docs.zephyrproject.org/latest/services/logging/index.html)

> Important: The mode for MCUboot must be set in the sysbuild.conf file (like SB_CONFIG_MCUBOOT_MODE_SWAP_SCRATCH etc.); if it is set elsewhere, it will be overwritten.

