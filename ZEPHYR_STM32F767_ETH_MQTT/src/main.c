/*
 * Copyright (c) 2025 Blynk
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(mqtt_blynk, LOG_LEVEL_INF);

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/mqtt.h>

#include <zephyr/sys/printk.h>
#include <zephyr/random/random.h>
#include <string.h>
#include <errno.h>

#include <math.h>
#include <stdio.h>

#include "test_certs.h"

#if defined(CONFIG_SOCKS)
#define SOCKS5_PROXY_ADDR	CONFIG_SOCKS_ADDR
#define SOCKS5_PROXY_PORT	CONFIG_SOCKS_PORT
#endif

#define MQTT_CLIENTID	"1"

#define APP_SLEEP_MSECS		8000

#define APP_MQTT_BUFFER_SIZE	1024


/* Buffers for MQTT client. */
static uint8_t rx_buffer[APP_MQTT_BUFFER_SIZE];
static uint8_t tx_buffer[APP_MQTT_BUFFER_SIZE];

/* The mqtt client struct */
static struct mqtt_client client_ctx;

/* MQTT Broker details. */
static struct sockaddr_storage broker;

#if defined(CONFIG_SOCKS)
static struct sockaddr socks5_proxy;
#endif

/* Socket Poll */
static struct pollfd fds[1];
static int nfds;

static bool mqtt_connected;

static struct k_work_delayable pub_message;
#if defined(CONFIG_NET_DHCPV4)
static struct k_work_delayable check_network_conn;

/* Network Management events */
#define L4_EVENT_MASK (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)

static struct net_mgmt_event_callback l4_mgmt_cb;
#endif

#if defined(CONFIG_DNS_RESOLVER)
static struct addrinfo hints;
static struct addrinfo *haddr;
#endif

static K_SEM_DEFINE(mqtt_start, 0, 1);

/* Application TLS configuration details */
#define TLS_SNI_HOSTNAME CONFIG_CLOUD_BLYNK_SERVER_ADDR
#define APP_CA_CERT_TAG 1

static int timeout_for_publish(void);
static const sec_tag_t m_sec_tags[] = {
	APP_CA_CERT_TAG,
};

static uint8_t devbound_topic[] = "downlink/#";
static struct mqtt_topic subs_topic;
static struct mqtt_subscription_list subs_list;

static char broker_host[128] = CONFIG_CLOUD_BLYNK_SERVER_ADDR;
static uint16_t broker_port = CONFIG_CLOUD_BLYNK_SERVER_PORT;

static bool power_on = false;
static float target_temp = 23.0f;
static float current_temp = 15.0f;

static void mqtt_event_handler(struct mqtt_client *const client,
			       const struct mqtt_evt *evt);

static int tls_init(void)
{
	int err;

	err = tls_credential_add(APP_CA_CERT_TAG, TLS_CREDENTIAL_CA_CERTIFICATE,
				 ca_certificate, sizeof(ca_certificate));
    if (err < 0) {
		LOG_ERR("Failed to register public certificate: %d", err);
		return err;
	}

	return err;
}

static void prepare_fds(struct mqtt_client *client)
{
	if (client->transport.type == MQTT_TRANSPORT_SECURE) {
		fds[0].fd = client->transport.tls.sock;
	}

	fds[0].events = POLLIN;
	nfds = 1;
}

static void clear_fds(void)
{
	nfds = 0;
}

static int wait(int timeout)
{
	int rc = -EINVAL;

	if (nfds <= 0) {
		return rc;
	}

	rc = poll(fds, nfds, timeout);
	if (rc < 0) {
		return -errno;
	}

	return rc;
}

static bool parse_url(const char *url, char *host_out, size_t host_len, uint16_t *port_out)
{
    const char *p = strstr(url, "://");
    if (!p) return false;

    p += 3;  // skip past "://"

    const char *colon = strchr(p, ':');
    const char *slash = strchr(p, '/');

    if (colon && (!slash || colon < slash)) {
        size_t len = colon - p;
        if (len >= host_len) return false;
        strncpy(host_out, p, len);
        host_out[len] = '\0';

        *port_out = (uint16_t)atoi(colon + 1);
    } else {
        // No port specified
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        if (len >= host_len) return false;
        strncpy(host_out, p, len);
        host_out[len] = '\0';

        *port_out = broker_port;  // fallback default
    }

    return true;
}

static void broker_init(void)
{
	struct sockaddr_in *broker4 = (struct sockaddr_in *)&broker;

	broker4->sin_family = AF_INET;
	broker4->sin_port = htons(broker_port);

#if defined(CONFIG_DNS_RESOLVER)
	net_ipaddr_copy(&broker4->sin_addr,
			&net_sin(haddr->ai_addr)->sin_addr);
#else
	inet_pton(AF_INET, broker_host, &broker4->sin_addr);
#endif

#if defined(CONFIG_SOCKS)
	struct sockaddr_in *proxy4 = (struct sockaddr_in *)&socks5_proxy;

	proxy4->sin_family = AF_INET;
	proxy4->sin_port = htons(SOCKS5_PROXY_PORT);
	inet_pton(AF_INET, SOCKS5_PROXY_ADDR, &proxy4->sin_addr);
#endif
}

static void client_init(struct mqtt_client *client)
{
	static struct mqtt_utf8 password;
	static struct mqtt_utf8 username;
	struct mqtt_sec_config *tls_config;

	mqtt_client_init(client);

	broker_init();

	/* MQTT client configuration */
	client->broker = &broker;
	client->evt_cb = mqtt_event_handler;

	client->client_id.utf8 = (uint8_t *)MQTT_CLIENTID;
	client->client_id.size = strlen(MQTT_CLIENTID);

	password.utf8 = (uint8_t *)CONFIG_CLOUD_BLYNK_AUTH_TOKEN;
	password.size = strlen(CONFIG_CLOUD_BLYNK_AUTH_TOKEN);

	client->password = &password;

	username.utf8 = (uint8_t *)CONFIG_CLOUD_BLYNK_USERNAME;
	username.size = strlen(CONFIG_CLOUD_BLYNK_USERNAME);

	client->user_name = &username;

	client->protocol_version = MQTT_VERSION_3_1_1;

	/* MQTT buffers configuration */
	client->rx_buf = rx_buffer;
	client->rx_buf_size = sizeof(rx_buffer);
	client->tx_buf = tx_buffer;
	client->tx_buf_size = sizeof(tx_buffer);

	/* MQTT transport configuration */
	client->transport.type = MQTT_TRANSPORT_SECURE;

	tls_config = &client->transport.tls.config;

	tls_config->peer_verify = TLS_PEER_VERIFY_REQUIRED;
	tls_config->cipher_list = NULL;
	tls_config->sec_tag_list = m_sec_tags;
	tls_config->sec_tag_count = ARRAY_SIZE(m_sec_tags);
	tls_config->hostname = TLS_SNI_HOSTNAME;

#if defined(CONFIG_SOCKS)
	mqtt_client_set_proxy(client, &socks5_proxy,
			      socks5_proxy.sa_family == AF_INET ?
			      sizeof(struct sockaddr_in) :
			      sizeof(struct sockaddr_in6));
#endif
}

static void publish_str(const char *topic, const char *value)
{
    if (!mqtt_connected) {
        LOG_WRN("Publish skipped (MQTT not connected): %s -> %s", topic, value);
        return;
    }

    struct mqtt_publish_param param = {
        .message.topic.qos = MQTT_QOS_1_AT_LEAST_ONCE,
        .message.topic.topic.utf8 = (uint8_t *)topic,
        .message.topic.topic.size = strlen(topic),
        .message.payload.data = (void *)value,
        .message.payload.len = strlen(value),
        .message_id = sys_rand16_get(),
        .dup_flag = 0U,
        .retain_flag = 0U,
    };

    int err = mqtt_publish(&client_ctx, &param);
    if (err) {
        LOG_ERR("Failed to publish to %s (err %d)", topic, err);
    } else {
        LOG_DBG("Published: %s -> %s", topic, value);
    }
}


static void terminal_print(const char *msg)
{
    char payload[128];
    snprintf(payload, sizeof(payload), "%s\n", msg);
    struct mqtt_publish_param param = {
        .message.topic.qos = MQTT_QOS_1_AT_LEAST_ONCE,
        .message.topic.topic.utf8 = (uint8_t *)"ds/Terminal",
        .message.topic.topic.size = strlen("ds/Terminal"),
        .message.payload.data = payload,
        .message.payload.len = strlen(payload),
        .message_id = sys_rand16_get(),
        .dup_flag = 0U,
        .retain_flag = 0U,
    };
    mqtt_publish(&client_ctx, &param);
}

static void publish_timeout(struct k_work *work)
{
    float target = power_on ? target_temp : 10.0f;
    current_temp += (target - current_temp) * 0.05f;
    current_temp += ((0.5f - ((float)(sys_rand32_get() % 1000) / 1000.0f)) * 0.3f);
    current_temp = fminf(fmaxf(current_temp, 10.0f), 35.0f);

    char temp_str[16];
    snprintf(temp_str, sizeof(temp_str), "%.2f", (double)current_temp);
    publish_str("ds/Current Temperature", temp_str);

    int state = 1; // OFF
    if (power_on) {
        if (fabsf(current_temp - target_temp) < 1.0f) {
            state = 2; // Idle
        } else if (target_temp > current_temp) {
            state = 3; // Heating
        } else {
            state = 4; // Cooling
        }
    }

    const char *colors[] = { NULL, "E4F6F7", "E6F7E4", "F7EAE4", "E4EDF7" };

    char state_str[8];
    snprintf(state_str, sizeof(state_str), "%d", state);
    publish_str("ds/Status", state_str);

    if (colors[state]) {
        publish_str("ds/Status/prop/color", colors[state]);
    }

    // Reschedule next publish
    k_work_reschedule(&pub_message, K_SECONDS(timeout_for_publish()));
}


static void handle_terminal_command(const char *payload)
{
    char temp_buf[64];
    strncpy(temp_buf, payload, sizeof(temp_buf) - 1);
    temp_buf[sizeof(temp_buf) - 1] = '\0';

    char *cmd = strtok(temp_buf, " \r\n");
    if (!cmd) return;

    if (strcmp(cmd, "set") == 0) {
        char *arg = strtok(NULL, " \r\n");
        if (arg) {
            target_temp = atof(arg);
            terminal_print("Temperature set");
            char val[16];
            snprintf(val, sizeof(val), "%.1f", (double)target_temp);
            publish_str("ds/Set Temperature", val);

        }
    } else if (strcmp(cmd, "on") == 0) {
        power_on = true;
        publish_str("ds/Power", "1");
        terminal_print("Turned ON");

    } else if (strcmp(cmd, "off") == 0) {
        power_on = false;
        publish_str("ds/Power", "0");
        terminal_print("Turned OFF");

    } else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        terminal_print("Available commands:");
        terminal_print("  set N    - set target temperature");
        terminal_print("  on       - turn on");
        terminal_print("  off      - turn off");

    } else {
        terminal_print("Unknown command");
    }
}

static void handle_incoming_topic(const char *topic, const char *payload)
{
    LOG_INF("Received topic: %s -> %s", topic, payload);

    if (strcmp(topic, "downlink/ds/Power") == 0) {
        power_on = atoi(payload);
        publish_str("ds/Set Temperature/prop/isDisabled", power_on ? "0" : "1");
        publish_timeout(NULL);
    } else if (strcmp(topic, "downlink/ds/Set Temperature") == 0) {
        target_temp = atof(payload);
        publish_timeout(NULL);
    } else if (strcmp(topic, "downlink/ds/Terminal") == 0) {
        handle_terminal_command(payload);
        publish_timeout(NULL);
    } else if (strcmp(topic, "downlink/ping") == 0) {
        LOG_INF("Ping, MQTT client library automagically sends the QOS1 response");
    } else if (strcmp(topic, "downlink/reboot") == 0) {
        LOG_INF("Reboot");
        sys_reboot(SYS_REBOOT_COLD);
    } else if (strcmp(topic, "downlink/redirect") == 0) {
        char new_host[128];
        uint16_t new_port;

        if (parse_url(payload, new_host, sizeof(new_host), &new_port)) {
            strncpy(broker_host, new_host, sizeof(broker_host));
            broker_port = new_port;
            LOG_INF("Redirecting to new broker: %s:%d", broker_host, broker_port);

            mqtt_abort(&client_ctx);  // force reconnect
            mqtt_connected = false;
            clear_fds();
            k_work_cancel_delayable(&pub_message);
            k_sem_give(&mqtt_start);  // restart connection flow
        } else {
            LOG_ERR("Failed to parse redirect URL: %s", payload);
        }
    }
}

static void mqtt_event_handler(struct mqtt_client *const client,
                               const struct mqtt_evt *evt)
{
    struct mqtt_puback_param puback;
    uint8_t data[128];
    int len;
    int bytes_read;

    switch (evt->type) {
    case MQTT_EVT_CONNACK:
        if (evt->result) {
            break;
        }

        mqtt_connected = true;
        terminal_print("      ___  __          __");
        terminal_print("     / _ )/ /_ _____  / /__");
        terminal_print("    / _  / / // / _ \\/  '_/");
        terminal_print("   /____/_/\\_, /_//_/_/\\_\\");
        terminal_print("          /___/");
        terminal_print("Type \"help\" for the list of available commands");

        break;

    case MQTT_EVT_PUBLISH:
        len = evt->param.publish.message.payload.len;
        if (len >= sizeof(data)) len = sizeof(data) - 1;
        bytes_read = mqtt_read_publish_payload(&client_ctx, data, len);
        if (bytes_read >= 0) {
            data[bytes_read] = '\0';

            char topic_str[128];
            int topic_len = evt->param.publish.message.topic.topic.size;
            memcpy(topic_str, evt->param.publish.message.topic.topic.utf8, topic_len);
            topic_str[topic_len] = '\0';

            handle_incoming_topic(topic_str, (char *)data);

            puback.message_id = evt->param.publish.message_id;
            mqtt_publish_qos1_ack(&client_ctx, &puback);
        }
        break;

    case MQTT_EVT_DISCONNECT:
        mqtt_connected = false;
        LOG_INF("MQTT client disconnected %d", evt->result);
        clear_fds();
        break;
    case MQTT_EVT_SUBACK:
        publish_str("get/ds", "Power,Set Temperature");  // <-- Correct value request
        break;

    case MQTT_EVT_PUBACK:
    case MQTT_EVT_PUBREC:
    case MQTT_EVT_PUBREL:
    case MQTT_EVT_PUBCOMP:
    case MQTT_EVT_UNSUBACK:
    case MQTT_EVT_PINGRESP:
        LOG_DBG("Unhandled MQTT event type %d", evt->type);
        break;
    }
}


static void subscribe(struct mqtt_client *client)
{
	int err;

	subs_topic.topic.utf8 = devbound_topic;
	subs_topic.topic.size = strlen(devbound_topic);
	subs_list.list = &subs_topic;
	subs_list.list_count = 1U;
	subs_list.message_id = 1U;

	err = mqtt_subscribe(client, &subs_list);
	if (err) {
		LOG_ERR("Failed on topic %s", devbound_topic);
	}
}

static void poll_mqtt(void)
{
	int rc;
	while (mqtt_connected) {
		rc = wait(SYS_FOREVER_MS);
		if (rc > 0) {
            if (mqtt_input(&client_ctx) != 0) {
                LOG_WRN("mqtt_input failed, dropping connection");
                mqtt_connected = false;
                break;
            }
        }
	}
}

/* Random time between 10 - 15 seconds
 * If you prefer to have this value more than CONFIG_MQTT_KEEPALIVE,
 * then keep the application connection live by calling mqtt_live()
 * in regular intervals.
 */

static int timeout_for_publish(void)
{
    return 10 + sys_rand8_get() % 5;
}


static int try_to_connect(struct mqtt_client *client)
{
	uint8_t retries = 3U;
	int rc;

    LOG_INF("Attempting to connect to %s:%d", broker_host, broker_port);

	while (retries--) {
		client_init(client);

		rc = mqtt_connect(client);
		if (rc) {
            LOG_ERR("MQTT connect retry %d failed (rc=%d)", 3 - retries, rc);
            if (rc == 4 || rc == 5)
                LOG_ERR("Invalid BLYNK_AUTH_TOKEN");
			continue;
		}

		prepare_fds(client);

		rc = wait(APP_SLEEP_MSECS);
		if (rc < 0) {
			mqtt_abort(client);
			return rc;
		}

		mqtt_input(client);

		if (mqtt_connected) {
            subscribe(client);
            k_work_reschedule(&pub_message,
					  K_SECONDS(timeout_for_publish()));
			return 0;
		}

		mqtt_abort(client);

		wait(10 * MSEC_PER_SEC);
	}

	return -EINVAL;
}

#if defined(CONFIG_DNS_RESOLVER)
static int get_mqtt_broker_addrinfo(void)
{
	int retries = 3;
	int rc = -EINVAL;

	while (retries--) {
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = 0;
		char port_str[6];
        snprintf(port_str, sizeof(port_str), "%u", broker_port);
        rc = getaddrinfo(broker_host, port_str, &hints, &haddr);
		if (rc == 0) {
			return 0;
		}

	}

	return rc;
}
#endif

static void connect_to_cloud_and_publish(void)
{
	int rc = -EINVAL;

#if defined(CONFIG_DNS_RESOLVER)
		rc = get_mqtt_broker_addrinfo();
		if (rc) {
			return;
		}
#endif
		rc = try_to_connect(&client_ctx);
		if (rc) {
            return;
		}

		poll_mqtt();
}

/* DHCP tries to renew the address after interface is down and up.
 * If DHCPv4 address renewal is success, then it doesn't generate
 * any event. We have to monitor this way.
 * If DHCPv4 attempts exceeds maximum number, it will delete iface
 * address and attempts for new request. In this case we can rely
 * on IPV4_ADDR_ADD event.
 */
#if defined(CONFIG_NET_DHCPV4)
static void check_network_connection(struct k_work *work)
{
	struct net_if *iface;
	if (mqtt_connected) {
		return;
	}

	iface = net_if_get_default();
	if (!iface) {
		goto end;
	}

	if (iface->config.dhcpv4.state == NET_DHCPV4_BOUND) {
		k_sem_give(&mqtt_start);
		return;
	}

end:
	k_work_reschedule(&check_network_conn, K_SECONDS(3));
}
#endif

#if defined(CONFIG_NET_DHCPV4)
static void abort_mqtt_connection(void)
{
	if (mqtt_connected) {
		mqtt_connected = false;
		mqtt_abort(&client_ctx);
		k_work_cancel_delayable(&pub_message);
	}
}

static void l4_event_handler(struct net_mgmt_event_callback *cb,
			     uint32_t mgmt_event, struct net_if *iface)
{
	if ((mgmt_event & L4_EVENT_MASK) != mgmt_event) {
		return;
	}

	if (mgmt_event == NET_EVENT_L4_CONNECTED) {
		/* Wait for DHCP to be back in BOUND state */
		k_work_reschedule(&check_network_conn, K_SECONDS(3));

		return;
	}

	if (mgmt_event == NET_EVENT_L4_DISCONNECTED) {
		abort_mqtt_connection();
		k_work_cancel_delayable(&check_network_conn);

		return;
	}
}
#endif


int main(void)
{
    int rc;

	LOG_INF("Waiting for network to setup...");
    if (strlen(CONFIG_CLOUD_BLYNK_USERNAME) == 0 || strlen(CONFIG_CLOUD_BLYNK_AUTH_TOKEN) == 0) {
        LOG_ERR("Blynk username or auth token is empty. Check your configuration.");
        return -EINVAL;
    }

	rc = tls_init();
	if (rc) {
		return 0;
	}

	k_work_init_delayable(&pub_message, publish_timeout);

#if defined(CONFIG_NET_DHCPV4)
	k_work_init_delayable(&check_network_conn, check_network_connection);

	net_mgmt_init_event_callback(&l4_mgmt_cb, l4_event_handler, L4_EVENT_MASK);
	net_mgmt_add_event_callback(&l4_mgmt_cb);

	// Start initial DHCP watcher
	k_work_schedule(&check_network_conn, K_NO_WAIT);
#endif

	// Loop forever waiting for network and reconnecting
	while (1) {
		k_sem_take(&mqtt_start, K_FOREVER);
		connect_to_cloud_and_publish();
	}

	return 0;
}

