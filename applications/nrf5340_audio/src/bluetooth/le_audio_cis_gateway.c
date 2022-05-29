/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#if (CONFIG_AUDIO_DEV == GATEWAY)

#include <bluetooth/bluetooth.h>
#include <bluetooth/audio/audio.h>

#include "le_audio.h"
#include "macros_common.h"

#include <logging/log.h>
LOG_MODULE_REGISTER(cis_gateway, CONFIG_LOG_BLE_LEVEL);

static struct bt_conn *default_conn;
static struct bt_audio_stream audio_stream;
static struct bt_audio_unicast_group *unicast_group;

static struct k_work_delayable audio_send_work;
static struct bt_codec *remote_codecs[CONFIG_BT_AUDIO_UNICAST_CLIENT_PAC_COUNT];
static struct bt_audio_ep *sinks[CONFIG_BT_AUDIO_UNICAST_CLIENT_ASE_SNK_COUNT];

#define BT_AUDIO_LC3_UNICAST_PRESET_NRF5340_AUDIO \
	BT_AUDIO_LC3_PRESET( \
		BT_CODEC_LC3_CONFIG_48_4, \
		BT_CODEC_LC3_QOS_10_UNFRAMED(120u, 2u, 20u, 10000u) \
	)

static struct bt_audio_lc3_preset unicast_present_nrf5340 = BT_AUDIO_LC3_UNICAST_PRESET_NRF5340_AUDIO;

#define HCI_ISO_BUF_ALLOC_PER_CHAN 2
#define NET_BUF_POOL_ITERATE(i, _)                                                                 \
	NET_BUF_POOL_FIXED_DEFINE(iso_tx_pool_##i, HCI_ISO_BUF_ALLOC_PER_CHAN,                     \
				  BT_ISO_SDU_BUF_SIZE(CONFIG_BT_ISO_TX_MTU), 8, NULL);
#define NET_BUF_POOL_PTR_ITERATE(i, ...) IDENTITY(&iso_tx_pool_##i)

LISTIFY(CONFIG_BT_ISO_MAX_CHAN, NET_BUF_POOL_ITERATE, (;))

static struct net_buf_pool *iso_tx_pools[] = { LISTIFY(CONFIG_BT_ISO_MAX_CHAN,
						       NET_BUF_POOL_PTR_ITERATE,
						       (,)) };


static void start_scan(void);
static int group_create(struct bt_audio_stream *stream);
static int stream_config(struct bt_audio_stream *stream);
static int stream_qos_set(void);
static int stream_enable(struct bt_audio_stream *stream);
static int stream_start(struct bt_audio_stream *stream);

static void audio_timer_timeout(struct k_work *work)
{
	int ret;
	static uint8_t buf_data[CONFIG_BT_ISO_TX_MTU];
	static bool data_initialized;
	struct net_buf *buf;
	static size_t len_to_send = 1;

	if (!data_initialized) {
		/* TODO: Actually encode some audio data */
		for (int i = 0; i < ARRAY_SIZE(buf_data); i++) {
			buf_data[i] = (uint8_t)i;
		}

		data_initialized = true;
	}

	//buf = net_buf_alloc(&tx_pool, K_FOREVER);
	net_buf_reserve(buf, BT_ISO_CHAN_SEND_RESERVE);

	net_buf_add_mem(buf, buf_data, len_to_send);

	ret = bt_audio_stream_send(&audio_stream, buf);
	if (ret < 0) {
		LOG_INF("Failed to send audio data (%d)\n", ret);
		net_buf_unref(buf);
	} else {
		LOG_INF("Sending mock data with len %zu\n", len_to_send);
	}

	k_work_schedule(&audio_send_work, K_MSEC(1000));

	len_to_send++;
	if (len_to_send > ARRAY_SIZE(buf_data)) {
		len_to_send = 1;
	}
}

static int group_create(struct bt_audio_stream *stream)
{
	int err;
	LOG_INF("%s", __func__);
	err = bt_audio_unicast_group_create(stream, 1, &unicast_group);
	if (err != 0) {
		LOG_INF("Could not create unicast group (err %d)\n", err);
		return err;
	}

	return 0;
}

static int stream_config(struct bt_audio_stream *stream)
{
	int err;
	LOG_INF("%s", __func__);
	err = bt_audio_stream_config(default_conn, stream, sinks[0], &unicast_present_nrf5340.codec);
	if (err != 0) {
		LOG_INF("Could not configure stream\n");
		return err;
	}

	return 0;
}

static int stream_qos_set(void)
{
	int err;
	LOG_INF("%s", __func__);
	err = bt_audio_stream_qos(default_conn, unicast_group, &unicast_present_nrf5340.qos);
	if (err != 0) {
		LOG_INF("Unable to setup QoS: %d", err);
		return err;
	}
	stream_enable(&audio_stream);
	return 0;
}

static int stream_enable(struct bt_audio_stream *stream)
{
	int err;

	err = bt_audio_stream_enable(stream, unicast_present_nrf5340.codec.meta,
				     unicast_present_nrf5340.codec.meta_count);
	if (err != 0) {
		LOG_INF("Unable to enable stream: %d", err);
		return err;
	}

	return 0;
}

static int stream_start(struct bt_audio_stream *stream)
{
	int err;

	err = bt_audio_stream_start(stream);
	if (err != 0) {
		LOG_INF("Unable to start stream: %d\n", err);
		return err;
	}

	return 0;
}

static void stream_configured_cb(struct bt_audio_stream *stream,
			      const struct bt_codec_qos_pref *pref)
{
	LOG_INF("%s", __func__);
	LOG_INF("Audio Stream %p configured\n", (void *)stream);
	// create group
	stream_qos_set();
}

static void stream_qos_set_cb(struct bt_audio_stream *stream)
{
	LOG_INF("Audio Stream %p QoS set\n", (void *)stream);
}

static void stream_enabled_cb(struct bt_audio_stream *stream)
{
	LOG_INF("Audio Stream %p enabled\n", (void *)stream);
	stream_start(&audio_stream);
}
#include "audio_system.h"
#include "streamctrl.h"
static void stream_started_cb(struct bt_audio_stream *stream)
{
	LOG_INF("Audio Stream %p started\n", (void *)stream);
	/* Start send timer */
	//k_work_schedule(&audio_send_work, K_MSEC(0));
	audio_system_start();
	stream_state_set(STATE_STREAMING);
}

static void stream_metadata_updated_cb(struct bt_audio_stream *stream)
{
	LOG_INF("Audio Stream %p metadata updated\n", (void *)stream);
}

static void stream_disabled_cb(struct bt_audio_stream *stream)
{
	LOG_INF("Audio Stream %p disabled\n", (void *)stream);
}

static void stream_stopped_cb(struct bt_audio_stream *stream)
{
	LOG_INF("Audio Stream %p stopped\n", (void *)stream);

	/* Stop send timer */
	//k_work_cancel_delayable(&audio_send_work);
}

static void stream_released_cb(struct bt_audio_stream *stream)
{
	LOG_INF("Audio Stream %p released\n", (void *)stream);
}


static struct bt_audio_stream_ops stream_ops = {
	.configured = stream_configured_cb,
	.qos_set = stream_qos_set_cb,
	.enabled = stream_enabled_cb,
	.started = stream_started_cb,
	.metadata_updated = stream_metadata_updated_cb,
	.disabled = stream_disabled_cb,
	.stopped = stream_stopped_cb,
	.released = stream_released_cb,
};

static void add_remote_sink(struct bt_audio_ep *ep, uint8_t index)
{
	LOG_INF("%s", __func__);
	LOG_INF("Sink #%u: ep %p\n", index, (void *)ep);
	sinks[index] = ep;
}

static void add_remote_codec(struct bt_codec *codec, int index, uint8_t type)
{
	LOG_INF("%s", __func__);
	LOG_INF("#%u: codec %p type 0x%02x\n", index, (void *)codec, type);

	//print_codec(codec);

	if (type != BT_AUDIO_SINK && type != BT_AUDIO_SOURCE) {
		return;
	}

	if (index < CONFIG_BT_AUDIO_UNICAST_CLIENT_PAC_COUNT) {
		remote_codecs[index] = codec;
	}
}

static void discover_sink_cb(struct bt_conn *conn, struct bt_codec *codec, struct bt_audio_ep *ep,
			     struct bt_audio_discover_params *params)
{
	if (params->err != 0) {
		LOG_INF("Discovery failed: %d\n", params->err);
		return;
	}

	if (codec != NULL) {
		add_remote_codec(codec, params->num_caps, params->type);
		return;
	}

	if (ep != NULL) {
		if (params->type == BT_AUDIO_SINK) {
			add_remote_sink(ep, params->num_eps);
		} else {
			LOG_INF("Invalid param type: %u\n", params->type);
		}

		return;
	}

	LOG_INF("Discover complete: err %d\n", params->err);

	(void)memset(params, 0, sizeof(*params));
	stream_config(&audio_stream);
}
#define BT_LE_CONN_PARAM_MULTI \
	BT_LE_CONN_PARAM(CONFIG_BLE_ACL_CONN_INTERVAL, CONFIG_BLE_ACL_CONN_INTERVAL, \
			 CONFIG_BLE_ACL_SLAVE_LATENCY, CONFIG_BLE_ACL_SUP_TIMEOUT)

#define DEVICE_NAME_PEER "NRF5340_AUDIO"
#define DEVICE_NAME_PEER_LEN (sizeof(DEVICE_NAME_PEER) - 1)
static int device_found(uint8_t type, const uint8_t *data, uint8_t data_len,
			const bt_addr_le_t *addr)
{
	int ret;
	struct bt_conn *conn;
	char addr_str[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
	if ((data_len == DEVICE_NAME_PEER_LEN) &&
	    (strncmp(DEVICE_NAME_PEER, data, DEVICE_NAME_PEER_LEN) == 0)) {
		LOG_INF("device found\n");
		bt_le_scan_stop();
		ret = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN, BT_LE_CONN_PARAM_MULTI,
					&default_conn);
		if (ret != 0) {
			LOG_INF("Create conn to failed (%u)\n", ret);
			start_scan();
		}
	}

	return -ENOENT;
}

/** @brief  BLE parse advertisement package.
 */
static void ad_parse(struct net_buf_simple *p_ad, const bt_addr_le_t *addr)
{
	while (p_ad->len > 1) {
		uint8_t len = net_buf_simple_pull_u8(p_ad);
		uint8_t type;

		/* Check for early termination */
		if (len == 0) {
			return;
		}

		if (len > p_ad->len) {
			LOG_INF("AD malformed");
			return;
		}

		type = net_buf_simple_pull_u8(p_ad);

		if (device_found(type, p_ad->data, len - 1, addr) == 0) {
			return;
		}

		(void)net_buf_simple_pull(p_ad, len - 1);
	}
}



static void on_device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
			    struct net_buf_simple *p_ad)
{
	/* Note: May lead to connection creation */
	ad_parse(p_ad, addr);
}

static void start_scan(void)
{
	int err;

	/* This demo doesn't require active scan */
	err = bt_le_scan_start(BT_LE_SCAN_ACTIVE, on_device_found);
	if (err != 0) {
		LOG_INF("Scanning failed to start (err %d)\n", err);
		return;
	}

	LOG_INF("Scanning successfully started\n");
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	(void)bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (err != 0) {
		LOG_INF("Failed to connect to %s (%u)\n", addr, err);

		bt_conn_unref(default_conn);
		default_conn = NULL;

		start_scan();
		return;
	}

	if (conn != default_conn) {
		return;
	}

	LOG_INF("Connected: %s\n", addr);
	bt_conn_set_security(conn, BT_SECURITY_L2);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	if (conn != default_conn) {
		return;
	}

	(void)bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Disconnected: %s (reason 0x%02x)\n", addr, reason);

	bt_conn_unref(default_conn);
	default_conn = NULL;

	start_scan();
}

static void security_changed_cb(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
	int ret;

	if (err) {
		LOG_ERR("Security failed: level %u err %d", level, err);
		ret = bt_conn_disconnect(conn, err);
		if (ret) {
			LOG_ERR("Failed to disconnect %d", ret);
		}
	} else {
		LOG_INF("Security changed: level %u", level);
		static struct bt_audio_discover_params dis_params;

		dis_params.func = discover_sink_cb;
		dis_params.type = BT_AUDIO_SINK;

		err = bt_audio_discover(default_conn, &dis_params);
		if (err != 0) {
			LOG_INF("Failed to discover sink: %d\n", err);
		}


		
	}
}


BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed_cb,
};


int le_audio_config_get(void)
{
	return 0;
}

int le_audio_volume_up(void)
{
	return 0;
}

int le_audio_volume_down(void)
{
	return 0;
}

int le_audio_volume_mute(void)
{
	return 0;
}

int le_audio_play(void)
{
	return 0;
}

int le_audio_pause(void)
{
	return 0;
}

int le_audio_send(uint8_t const *const data, size_t size)
{
	int ret;
	struct net_buf *buf;
	buf = net_buf_alloc(iso_tx_pools[0], K_NO_WAIT);
	net_buf_reserve(buf, BT_ISO_CHAN_SEND_RESERVE);

	net_buf_add_mem(buf, data, 120);

	ret = bt_audio_stream_send(&audio_stream, buf);
	if (ret < 0) {
		//LOG_INF("Failed to send audio data (%d)\n", ret);
		net_buf_unref(buf);
	} else {
		//LOG_INF("Sending mock data with len %zu\n", size);
	}
	return 0;
}

void le_audio_enable(le_audio_receive_cb recv_cb)
{
	k_work_init_delayable(&audio_send_work, audio_timer_timeout);
	audio_stream.ops = &stream_ops;
	group_create(&audio_stream);
	start_scan();
}

void le_audio_disable(void)
{
	// TODO: Stopping functionality is in broadcast_audio_source sample
}

#endif /* (CONFIG_AUDIO_DEV == GATEWAY) */
