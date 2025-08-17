/*
 * SPDX-FileCopyrightText: 2025 Khrystyna Olkhovetska for Blynk Technologies Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(net_mgr, LOG_LEVEL_INF);

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/wifi_mgmt.h>
#include "network_manager.h"
#include "mqtt_client.h"

/* WiFi Configuration - Update these with your network credentials */
#define WIFI_SSID "BlackCatHome311"
#define WIFI_PSK  "ThereIsNoSpoon"

static bool network_connected = false;
static struct net_if *wifi_iface = NULL;
static struct wifi_connect_req_params wifi_config;

/* Work item for checking network connection */
static struct k_work_delayable check_network_conn;

/* WiFi event callback */
static struct net_mgmt_event_callback wifi_mgmt_cb;

bool is_network_connected(void)
{
    return network_connected;
}

static void wifi_event_handler(struct net_mgmt_event_callback *cb,
                              uint32_t mgmt_event,
                              struct net_if *iface)
{
    switch (mgmt_event) {
    case NET_EVENT_WIFI_CONNECT_RESULT: {
        LOG_INF("Connected to WiFi: %s", WIFI_SSID);
        /* Don't set network_connected here - wait for DHCP */
        k_work_reschedule(&check_network_conn, K_SECONDS(1));
        break;
    }
    case NET_EVENT_WIFI_DISCONNECT_RESULT: {
        LOG_INF("Disconnected from WiFi: %s", WIFI_SSID);
        network_connected = false;
        abort_mqtt_connection();
        k_work_cancel_delayable(&check_network_conn);
        /* Try to reconnect */
        k_work_reschedule(&check_network_conn, K_SECONDS(5));
        break;
    }
    default:
        break;
    }
}

static int connect_to_wifi(void)
{
    if (!wifi_iface) {
        LOG_ERR("WiFi interface not initialized");
        return -EIO;
    }

    /* Check if already connected */
    if (net_if_is_up(wifi_iface) && network_connected) {
        LOG_INF("WiFi already connected");
        return 0;
    }

    wifi_config.ssid = (const uint8_t *)WIFI_SSID;
    wifi_config.ssid_length = strlen(WIFI_SSID);
    wifi_config.psk = (const uint8_t *)WIFI_PSK;
    wifi_config.psk_length = strlen(WIFI_PSK);
    wifi_config.security = WIFI_SECURITY_TYPE_PSK;
    wifi_config.channel = WIFI_CHANNEL_ANY;
    wifi_config.band = WIFI_FREQ_BAND_2_4_GHZ;

    LOG_INF("Connecting to WiFi SSID: %s", WIFI_SSID);

    int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, wifi_iface, &wifi_config,
                      sizeof(struct wifi_connect_req_params));
    if (ret) {
        LOG_ERR("Failed to connect to WiFi (%s), err: %d", WIFI_SSID, ret);
    }

    return ret;
}

static void check_network_connection_work(struct k_work *work)
{
    struct net_if *iface;

    if (mqtt_connected) {
        return;
    }

    /* Get WiFi interface */
    if (!wifi_iface) {
        wifi_iface = net_if_get_wifi_sta();
        if (!wifi_iface) {
            LOG_ERR("WiFi STA interface not found");
            goto retry;
        }
    }

    iface = wifi_iface;

    /* Check if WiFi interface is up */
    if (!net_if_is_up(iface)) {
        LOG_DBG("WiFi interface is down, attempting connection");
        connect_to_wifi();
        goto retry;
    }

#if defined(CONFIG_NET_DHCPV4)
    /* Check DHCP state */
    if (iface->config.dhcpv4.state == NET_DHCPV4_BOUND) {
        if (!network_connected) {
            LOG_INF("Network connected - WiFi + DHCP ready");
            network_connected = true;
            k_sem_give(&mqtt_start);
        }
        return;
    } else {
        LOG_DBG("DHCP state: %d", iface->config.dhcpv4.state);
    }
#else
    /* For static IP, check if interface has IP */
    struct net_if_ipv4 *ipv4 = iface->config.ip.ipv4;
    if (ipv4 && ipv4->unicast[0].addr_state == NET_ADDR_PREFERRED) {
        if (!network_connected) {
            LOG_INF("Network connected - WiFi + Static IP ready");
            network_connected = true;
            k_sem_give(&mqtt_start);
        }
        return;
    }
#endif

retry:
    k_work_reschedule(&check_network_conn, K_SECONDS(3));
}

void network_manager_init(void)
{
    /* Initialize work item */
    k_work_init_delayable(&check_network_conn, check_network_connection_work);

    /* Setup WiFi event callback */
    net_mgmt_init_event_callback(&wifi_mgmt_cb,
                                wifi_event_handler,
                                NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT);
    net_mgmt_add_event_callback(&wifi_mgmt_cb);

    /* Get WiFi interface */
    wifi_iface = net_if_get_wifi_sta();
    if (!wifi_iface) {
        LOG_ERR("WiFi STA interface not found");
        return;
    }

    LOG_INF("WiFi Network Manager initialized");

    /* Start connection process */
    k_work_schedule(&check_network_conn, K_SECONDS(1));
}

