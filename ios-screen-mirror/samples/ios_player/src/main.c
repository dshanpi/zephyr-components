/*
 * Copyright (c) 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Shows the screen of an iPhone: joins the WiFi, announces itself as an AirPlay receiver and
 * decodes the H.264 stream of the mirroring phone with the video engine onto the video plane.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <ff.h>
#include <lvgl.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/display/display_sunxi.h>
#include <zephyr/drivers/vdec.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/igmp.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/sys/printk.h>
#include <ios_zephyr/airplay.h>

#define REPORT_MS	5000

static FATFS fat_fs;
static struct fs_mount_t mp = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
	.mnt_point = "/SD:",
};

/* ---- status text -------------------------------------------------------------------- */

static lv_obj_t *status_label;
static K_MUTEX_DEFINE(ui_lock);

static void status(const char *text)
{
	printk("ios: %s\n", text);
	k_mutex_lock(&ui_lock, K_FOREVER);
	if (text[0] == '\0') {
		lv_obj_add_flag(lv_obj_get_parent(status_label), LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_label_set_text(status_label, text);
		lv_obj_remove_flag(lv_obj_get_parent(status_label), LV_OBJ_FLAG_HIDDEN);
	}
	k_mutex_unlock(&ui_lock);
}

static void ui_build(void)
{
	lv_obj_t *scr = lv_screen_active();
	lv_obj_t *card = lv_obj_create(scr);

	lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
	lv_obj_set_size(card, 470, 110);
	lv_obj_center(card);
	lv_obj_set_style_bg_color(card, lv_color_make(250, 250, 252), 0);
	lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(card, 16, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	status_label = lv_label_create(card);
	lv_obj_set_style_text_font(status_label, &lv_font_montserrat_20, 0);
	lv_obj_set_style_text_color(status_label, lv_color_make(30, 32, 40), 0);
	lv_obj_set_style_bg_color(status_label, lv_color_make(250, 250, 252), 0);
	lv_obj_set_style_bg_opa(status_label, LV_OPA_COVER, 0);
	lv_label_set_text(status_label, "");
	lv_obj_center(status_label);
}

/* ---- WiFi ---------------------------------------------------------------------------- */

static struct net_mgmt_event_callback wifi_cb, ipv4_cb;
static K_SEM_DEFINE(connect_done, 0, 1);
static K_SEM_DEFINE(got_ip, 0, 1);
static volatile int connect_status;

static void wifi_event(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *iface)
{
	if (event == NET_EVENT_WIFI_CONNECT_RESULT) {
		const struct wifi_status *s = cb->info;

		connect_status = s->status;
		k_sem_give(&connect_done);
	}
}

static void ipv4_event(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *iface)
{
	char buf[NET_IPV4_ADDR_LEN];

	printk("ios: address %s\n",
	       net_addr_ntop(AF_INET, &iface->config.ip.ipv4->unicast[0].ipv4.address.in_addr, buf,
			     sizeof(buf)));
	k_sem_give(&got_ip);
}

/* The network of the Android mirror player: the name on the first line, the password next */
static void wifi_load(char *ssid, char *psk)
{
	struct fs_file_t f;
	char buf[128] = {0};
	ssize_t n = 0;

	ssid[0] = psk[0] = '\0';
	fs_file_t_init(&f);
	if (fs_open(&f, "/SD:/wifi.cfg", FS_O_READ) == 0) {
		n = fs_read(&f, buf, sizeof(buf) - 1);
		fs_close(&f);
	}
	if (n > 0) {
		char *nl = strchr(buf, '\n');

		if (nl != NULL) {
			*nl = '\0';
			strncpy(ssid, buf, 32);
			strncpy(psk, nl + 1, 64);
			nl = strchr(psk, '\n');
			if (nl != NULL) {
				*nl = '\0';
			}
			size_t sl = strlen(ssid);
			if (sl > 0 && ssid[sl - 1] == '\r') ssid[sl - 1] = '\0';
			size_t pl = strlen(psk);
			if (pl > 0 && psk[pl - 1] == '\r') psk[pl - 1] = '\0';
		}
	}
	if (ssid[0] == '\0') {
		strncpy(ssid, CONFIG_SAMPLE_IOS_WIFI_SSID, 32);
		strncpy(psk, CONFIG_SAMPLE_IOS_WIFI_PSK, 64);
	}
}

static bool wifi_join(struct net_if *iface)
{
	char ssid[33] = {0}, psk[65] = {0};
	struct wifi_connect_req_params p = {
		.channel = WIFI_CHANNEL_ANY,
		.band = WIFI_FREQ_BAND_UNKNOWN,
	};

	wifi_load(ssid, psk);
	if (ssid[0] == '\0') {
		status("No WiFi: put wifi.cfg on the card");
		return false;
	}
	p.ssid = ssid;
	p.ssid_length = strlen(ssid);
	p.psk = psk;
	p.psk_length = strlen(psk);
	p.security = p.psk_length > 0U ? WIFI_SECURITY_TYPE_PSK : WIFI_SECURITY_TYPE_NONE;

	status("Joining the WiFi");
	net_mgmt_init_event_callback(&wifi_cb, wifi_event, NET_EVENT_WIFI_CONNECT_RESULT);
	net_mgmt_add_event_callback(&wifi_cb);
	net_mgmt_init_event_callback(&ipv4_cb, ipv4_event, NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ipv4_cb);
	connect_status = -1;
	if (net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &p, sizeof(p)) != 0 ||
	    k_sem_take(&connect_done, K_SECONDS(30)) != 0 || connect_status != 0) {
		status("Cannot join the WiFi");
		return false;
	}

	return k_sem_take(&got_ip, K_SECONDS(30)) == 0;
}

/* ---- video ---------------------------------------------------------------------------- */

static const struct device *const vdec_dev = DEVICE_DT_GET(DT_NODELABEL(ve));
static const struct device *const disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

static struct vdec_stream *stream;
static uint32_t shown_count, packets, bytes, t_decode_us;
static int64_t last_report;
static K_MUTEX_DEFINE(video_lock);
static bool too_big;
static int active_conns;
static uint8_t last_config[256];
static size_t last_config_len;

static K_MUTEX_DEFINE(disp_lock);
static K_SEM_DEFINE(disp_sem, 0, 1);
static K_SEM_DEFINE(disp_flushed, 0, 1);
K_MSGQ_DEFINE(disp_done_q, sizeof(struct vdec_frame), 8, 4);
static struct vdec_frame disp_pending;
static bool disp_has_pending, disp_flush_req;
static uint32_t mirror_src_w, mirror_src_h, mirror_dst_w, mirror_dst_h;

static void disp_thread_fn(void *a, void *b, void *c)
{
	struct vdec_frame shown = {0}, older = {0};

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	while (true) {
		struct vdec_frame f;
		bool do_flush = false, have_frame = false;

		int wret = k_sem_take(&disp_sem, older.priv != NULL ? K_MSEC(20) : K_FOREVER);
		k_mutex_lock(&disp_lock, K_FOREVER);
		if (disp_flush_req) {
			if (disp_has_pending) {
				k_msgq_put(&disp_done_q, &disp_pending, K_FOREVER);
				memset(&disp_pending, 0, sizeof(disp_pending));
				disp_has_pending = false;
			}
			disp_flush_req = false;
			do_flush = true;
		} else if (disp_has_pending) {
			f = disp_pending;
			memset(&disp_pending, 0, sizeof(disp_pending));
			disp_has_pending = false;
			have_frame = true;
		}
		k_mutex_unlock(&disp_lock);

		if (do_flush) {
			display_sunxi_hide_yuv(disp);
			k_msleep(35);
			if (older.priv != NULL) {
				k_msgq_put(&disp_done_q, &older, K_FOREVER);
				memset(&older, 0, sizeof(older));
			}
			if (shown.priv != NULL) {
				k_msgq_put(&disp_done_q, &shown, K_FOREVER);
				memset(&shown, 0, sizeof(shown));
			}
			k_sem_give(&disp_flushed);
			continue;
		}
		if (!have_frame) {
			if (wret == -EAGAIN && older.priv != NULL) {
				k_msgq_put(&disp_done_q, &older, K_FOREVER);
				memset(&older, 0, sizeof(older));
			}
			continue;
		}
		uint32_t cw = 0, ch = 0;

		k_mutex_lock(&disp_lock, K_FOREVER);
		if (mirror_dst_w >= 160U && mirror_dst_h >= 160U &&
		    mirror_dst_w <= f.width && mirror_dst_h <= f.height &&
		    (mirror_dst_w < f.width || mirror_dst_h < f.height)) {
			cw = mirror_dst_w;
			ch = mirror_dst_h;
		} else if (mirror_src_w >= 160U && mirror_src_h >= 160U &&
			   mirror_src_w <= f.width && mirror_src_h <= f.height &&
			   (mirror_src_w < f.width || mirror_src_h < f.height)) {
			cw = mirror_src_w;
			ch = mirror_src_h;
		}
		k_mutex_unlock(&disp_lock);
		struct display_sunxi_yuv yuv = {
			.y = f.plane[0],
			.uv = f.plane[1],
			.width = f.width,
			.height = f.height,
			.crop_width = cw,
			.crop_height = ch,
			.stride_y = f.stride[0],
			.stride_uv = f.stride[1],
			.bt709 = true,
			.nonblock = true,
		};

		display_sunxi_show_yuv(disp, &yuv);
		if (older.priv != NULL) {
			k_msgq_put(&disp_done_q, &older, K_FOREVER);
		}
		older = shown;
		shown = f;
	}
}

K_THREAD_DEFINE(ios_disp_thread, 8192, disp_thread_fn, NULL, NULL, NULL, 7, 0, 0);

static void reclaim_done(void)
{
	struct vdec_frame done;

	while (k_msgq_get(&disp_done_q, &done, K_NO_WAIT) == 0) {
		vdec_frame_release(vdec_dev, &done);
	}
}

static int stream_open(void)
{
	const struct vdec_stream_config cfg = {
		.codec = VDEC_CODEC_H264,
		.format = VDEC_FORMAT_NV12,
		.buffer_size = 256 * 1024,
		.no_cache_ops = true,
		/* the phone's stream keeps several reference pictures; the pictures are small */
		.holding_frames = CONFIG_SAMPLE_IOS_HOLD_FRAMES,
	};

	return vdec_stream_open(vdec_dev, &cfg, &stream);
}

static void stream_close(void)
{
	if (stream != NULL) {
		k_mutex_lock(&disp_lock, K_FOREVER);
		disp_flush_req = true;
		k_mutex_unlock(&disp_lock);
		k_sem_give(&disp_sem);
		k_sem_take(&disp_flushed, K_FOREVER);
		reclaim_done();
		vdec_stream_close(vdec_dev, stream);
		stream = NULL;
	}
	/* the next stream shows its first picture by taking the status text down */
	shown_count = 0;
}

static bool drop_pending(void)
{
	struct vdec_frame dropped = {0};

	k_mutex_lock(&disp_lock, K_FOREVER);
	if (disp_has_pending) {
		dropped = disp_pending;
		memset(&disp_pending, 0, sizeof(disp_pending));
		disp_has_pending = false;
	}
	k_mutex_unlock(&disp_lock);
	if (dropped.priv != NULL) {
		vdec_frame_release(vdec_dev, &dropped);
		return true;
	}

	return false;
}

static void drain(void)
{
	struct vdec_frame f;
	int ret;

	reclaim_done();
	while (true) {
		ret = vdec_stream_get_frame(vdec_dev, stream, &f);
		if (ret == -EBUSY) {
			reclaim_done();
			if (drop_pending()) {
				continue;
			}
			break;
		}
		if (ret != 0) {
			break;
		}
		struct vdec_frame dropped = {0};

		k_mutex_lock(&disp_lock, K_FOREVER);
		if (disp_has_pending) {
			dropped = disp_pending;
		}
		disp_pending = f;
		disp_has_pending = true;
		k_mutex_unlock(&disp_lock);
		if (dropped.priv != NULL) {
			vdec_frame_release(vdec_dev, &dropped);
		}
		k_sem_give(&disp_sem);
		if (shown_count++ == 0U) {
			printk("ios: first picture %ux%u\n", f.width, f.height);
			status("");
		}
		reclaim_done();
	}
	if (ret == -EBUSY) {
		static int64_t last;

		if (k_uptime_get() - last > 1000) {
			printk("ios: decoder has no free picture buffer\n");
			last = k_uptime_get();
		}
	}
}


/* ---- what the SPS says ---------------------------------------------------------------- */

struct bits {
	const uint8_t *p;
	size_t n, pos;
};

static uint32_t get_bits(struct bits *b, unsigned int count)
{
	uint32_t v = 0;

	while (count-- > 0U) {
		size_t byte = b->pos >> 3;

		v = (v << 1) | (byte < b->n ? (b->p[byte] >> (7 - (b->pos & 7U))) & 1U : 0U);
		b->pos++;
	}

	return v;
}

static uint32_t get_ue(struct bits *b)
{
	unsigned int zeros = 0;

	while (get_bits(b, 1) == 0U && zeros < 32U) {
		zeros++;
	}

	return ((1U << zeros) - 1U) + get_bits(b, zeros);
}

/* logs the size and the number of reference pictures of the first SPS in an Annex B buffer */
static bool read_sps(const uint8_t *d, size_t len, unsigned int *width, unsigned int *height,
		     unsigned int *refs_out)
{
	size_t i = 0;

	while (i + 5 < len && !(d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1 && (d[i + 3] & 0x1f) == 7)) {
		i++;
	}
	if (i + 5 >= len) {
		return false;
	}

	uint8_t rbsp[64];
	size_t n = MIN(len - (i + 4), sizeof(rbsp)), o = 0;

	for (size_t k = 0; k < n; k++) {
		const uint8_t c = d[i + 4 + k];

		if (o >= 2 && rbsp[o - 1] == 0 && rbsp[o - 2] == 0 && c == 3) {
			continue;	/* emulation prevention byte */
		}
		rbsp[o++] = c;
	}

	struct bits b = {.p = rbsp, .n = o, .pos = 0};
	unsigned int profile = get_bits(&b, 8);
	unsigned int level;
	uint32_t refs, w_mbs, h_mbs, frame_mbs_only;

	get_bits(&b, 8);
	level = get_bits(&b, 8);
	get_ue(&b);
	if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44) {
		uint32_t chroma = get_ue(&b);

		if (chroma == 3U) {
			get_bits(&b, 1);
		}
		get_ue(&b);
		get_ue(&b);
		get_bits(&b, 1);
		if (get_bits(&b, 1)) {
			printk("ios: SPS with scaling lists, size not read\n");
			return false;
		}
	}
	get_ue(&b);			/* log2_max_frame_num - 4 */
	if (get_ue(&b) == 0U) {		/* pic_order_cnt_type */
		get_ue(&b);
	}
	refs = get_ue(&b);
	get_bits(&b, 1);
	w_mbs = get_ue(&b) + 1U;
	h_mbs = get_ue(&b) + 1U;
	frame_mbs_only = get_bits(&b, 1);
	*width = w_mbs * 16U;
	*height = h_mbs * 16U * (frame_mbs_only ? 1U : 2U);
	*refs_out = refs;
	printk("ios: stream profile %u level %u, %u reference frames, %ux%u\n", profile, level, refs,
	       *width, *height);

	return true;
}

static void on_video(const uint8_t *data, size_t size, uint64_t pts_us, bool config, void *arg)
{
	size_t off = 0;
	uint32_t c0;

	k_mutex_lock(&video_lock, K_FOREVER);
	if (config) {
		unsigned int w = 0, h = 0, refs = 0;

		if (size <= sizeof(last_config)) {
			memcpy(last_config, data, size);
			last_config_len = size;
		}

		too_big = false;
		if (read_sps(data, size, &w, &h, &refs)) {
			/* the engine allocates two pictures plus the held ones; one more for safety */
			size_t need = (size_t)w * h * 3 / 2 * (2U + CONFIG_SAMPLE_IOS_HOLD_FRAMES);

			if (need > (CONFIG_VDEC_SUNXI_MAX_MEMORY_KB - 512U) * 1024U) {
				printk("ios: %ux%u does not fit the decoder (%zu KB)\n", w, h,
				       need / 1024U);
				too_big = true;
			}
		}
		if (too_big) {
			char text[64];

			stream_close();
			snprintk(text, sizeof(text), "Unsupported resolution %ux%u", w, h);
			status(text);
		}
	}
	if (too_big) {
		goto out;
	}
	if (config && stream != NULL) {
		/* a new SPS: the picture size may have changed */
		stream_close();
	}
	if (stream == NULL) {
		if (stream_open() != 0) {
			printk("ios: cannot open the decoder\n");
			goto out;
		}
		if (!config && last_config_len > 0U) {
			size_t coff = 0;

			while (coff < last_config_len) {
				size_t cused = 0;
				int cret = vdec_stream_feed(vdec_dev, stream, last_config + coff,
							    last_config_len - coff, -1, &cused);

				if (cret == 0) {
					coff += cused;
				} else if (cret == -EAGAIN) {
					drain();
					k_msleep(1);
				} else {
					break;
				}
			}
		}
	}
	c0 = k_cycle_get_32();
	int stuck = 0;

	while (off < size) {
		size_t used = 0;
		int ret = vdec_stream_feed(vdec_dev, stream, data + off, size - off,
					   config ? -1 : (int64_t)pts_us, &used);

		if (ret == 0) {
			off += used;
		} else if (ret == -EAGAIN) {
			drain();
			k_msleep(1);
			if (++stuck % 1000 == 0) {
				printk("ios: decoder not taking data (%d ms), %zu of %zu fed, shown %u\n",
				       stuck, off, size, shown_count);
			}
		} else {
			printk("ios: feed error %d\n", ret);
			break;
		}
	}
	if (!config && off == size) {
		static const uint8_t aud[6] = {0x00, 0x00, 0x00, 0x01, 0x09, 0x10};
		size_t aoff = 0;

		while (aoff < sizeof(aud)) {
			size_t used = 0;
			int ret = vdec_stream_feed(vdec_dev, stream, aud + aoff, sizeof(aud) - aoff,
						   (int64_t)pts_us, &used);

			if (ret == 0) {
				aoff += used;
			} else if (ret == -EAGAIN) {
				drain();
				k_msleep(1);
			} else {
				break;
			}
		}
	}
	drain();
	t_decode_us += (uint32_t)k_cyc_to_us_floor64(k_cycle_get_32() - c0);
	packets++;
	bytes += size;
	if (k_uptime_get() - last_report >= REPORT_MS) {
		int ms = (int)(k_uptime_get() - last_report);

		printk("ios: %u packets (%u/s), %u shown, %u KB/s, decode %u ms per %d ms\n",
		       packets, packets * 1000 / ms, shown_count, bytes / ms,
		       t_decode_us / 1000, ms);
		packets = bytes = t_decode_us = 0;
		last_report = k_uptime_get();
	}
out:
	k_mutex_unlock(&video_lock);
}

static void on_connection(bool up, void *arg)
{
	bool all_closed = false;

	k_mutex_lock(&video_lock, K_FOREVER);
	if (up) {
		active_conns++;
	} else if (active_conns > 0) {
		active_conns--;
		if (active_conns == 0) {
			stream_close();
			last_config_len = 0;
			shown_count = 0;
			k_mutex_lock(&disp_lock, K_FOREVER);
			mirror_src_w = mirror_src_h = mirror_dst_w = mirror_dst_h = 0;
			k_mutex_unlock(&disp_lock);
			all_closed = true;
		}
	}
	printk("ios: phone %s (conns=%d)\n", up ? "connected" : "disconnected", active_conns);
	k_mutex_unlock(&video_lock);
	if (all_closed) {
		status("Waiting for the phone");
	}
}

static void on_geometry(uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h,
			void *arg)
{
	ARG_UNUSED(arg);
	k_mutex_lock(&disp_lock, K_FOREVER);
	mirror_src_w = src_w;
	mirror_src_h = src_h;
	mirror_dst_w = dst_w;
	mirror_dst_h = dst_h;
	k_mutex_unlock(&disp_lock);
}

static void on_flush(void *arg)
{
	k_mutex_lock(&video_lock, K_FOREVER);
	stream_close();
	last_config_len = 0;
	shown_count = 0;
	k_mutex_lock(&disp_lock, K_FOREVER);
	mirror_src_w = mirror_src_h = mirror_dst_w = mirror_dst_h = 0;
	k_mutex_unlock(&disp_lock);
	k_mutex_unlock(&video_lock);
	status("Waiting for the phone");
}

int main(void)
{
	struct airplay_config cfg = {
		.name = CONFIG_IOS_MIRROR_NAME,
		.port = CONFIG_IOS_MIRROR_PORT,
		.video = on_video,
		.geometry = on_geometry,
		.connection = on_connection,
		.flush = on_flush,
	};
	struct net_if *iface = net_if_get_first_wifi();
	int ret;

	ui_build();
	if (!device_is_ready(vdec_dev) || !device_is_ready(disp) || iface == NULL) {
		printk("a device is not ready\n");
		return 0;
	}
	for (int attempt = 0; attempt < 5; attempt++) {
		ret = fs_mount(&mp);
		if (ret == 0) {
			break;
		}
		k_sleep(K_MSEC(1000));
	}
	if (ret != 0) {
		printk("cannot mount the SD card: %d\n", ret);
	}
	if (!wifi_join(iface)) {
		return 0;
	}
	/* the responder joins the group when an interface comes up, which was before it started */
	{
		struct in_addr mdns_group = {{{224, 0, 0, 251}}};

		ret = net_ipv4_igmp_join(iface, &mdns_group, NULL);
		printk("ios: mDNS group joined: %d\n", ret);
	}
	memcpy(cfg.hw_addr, net_if_get_link_addr(iface)->addr, sizeof(cfg.hw_addr));
	ret = airplay_start(&cfg);
	if (ret != 0) {
		printk("ios: cannot start AirPlay: %d\n", ret);
		status("Cannot start AirPlay");
		return 0;
	}
	last_report = k_uptime_get();
	{
		char text[96];

		snprintk(text, sizeof(text), "Screen Mirroring: choose \"%s\"", CONFIG_IOS_MIRROR_NAME);
		status(text);
	}
	while (true) {
		lv_timer_handler();
		k_msleep(50);
	}
}
