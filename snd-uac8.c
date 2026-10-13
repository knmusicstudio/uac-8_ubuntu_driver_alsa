// SPDX-License-Identifier: GPL-2.0
/*
 * snd-uac8 - ALSA USB オーディオドライバ (ZOOM UAC-8, USB 1686:f02b)
 *
 * 44.1 / 48 / 88.2 / 96 / 176.4 / 192 kHz サンプリングレート切り替え対応
 * 32, 64, 128, 256, 512, 1024, 2048 サンプル/周期 (frames/period) 完全対応版
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/bitops.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

MODULE_AUTHOR("converted from main.c");
MODULE_DESCRIPTION("ALSA driver for ZOOM UAC-8 (1686:f02b)");
MODULE_LICENSE("GPL");

/* ------------------------------------------------------------------ */
/* デバイス定数                                                       */
/* ------------------------------------------------------------------ */
#define UAC8_VENDOR_ID		0x1686
#define UAC8_PRODUCT_ID		0xf02b

#define UAC8_IF_CTRL		0
#define UAC8_IF_OUT		1
#define UAC8_IF_IN		2

#define UAC8_EP_OUT		0x01
#define UAC8_EP_IN		0x82

#define UAC8_PHYS_CHANNELS	32
#define UAC8_SAMPLE_BYTES	4	/* S32_LE */
#define UAC8_FRAME_BYTES	(UAC8_PHYS_CHANNELS * UAC8_SAMPLE_BYTES)	/* 128 */

#define UAC8_URB_BYTES		1024
#define UAC8_URB_FRAMES		(UAC8_URB_BYTES / UAC8_FRAME_BYTES)	/* 8 */
#define UAC8_MAX_URBS		128

#define UAC8_PLAY_CHANNELS	20
#define UAC8_CAP_CHANNELS	18

#define UAC8_BUFFER_FRAMES_MAX	32768
#define UAC8_PERIODS_MIN	2
#define UAC8_PERIODS_MAX	64

#define UAC8_CTRL_TIMEOUT_MS	1000
#define UAC8_MAX_ERRORS		32

#define UAC8_COMPANION		((void *)-1L)

static_assert(UAC8_URB_BYTES % UAC8_FRAME_BYTES == 0);
static_assert(UAC8_PLAY_CHANNELS <= UAC8_PHYS_CHANNELS);
static_assert(UAC8_CAP_CHANNELS <= UAC8_PHYS_CHANNELS);

/* ------------------------------------------------------------------ */
/* モジュールパラメータ                                               */
/* ------------------------------------------------------------------ */
static int card_index = SNDRV_DEFAULT_IDX1;
static char *card_id = SNDRV_DEFAULT_STR1;

module_param_named(index, card_index, int, 0444);
MODULE_PARM_DESC(index, "Index value for the UAC-8 sound card.");
module_param_named(id, card_id, charp, 0444);
MODULE_PARM_DESC(id, "ID string for the UAC-8 sound card.");

/* ------------------------------------------------------------------ */
/* 構造体                                                             */
/* ------------------------------------------------------------------ */
struct uac8;
struct uac8_stream;

struct uac8_urb_ctx {
	struct urb *urb;
	u8 *buf;
	struct uac8_stream *stream;
};

struct uac8_stream {
	struct uac8 *chip;
	bool playback;
	unsigned int max_ch;
	unsigned int active_urbs;	/* 周期サイズに応じて動的計算 */
	struct uac8_urb_ctx ctx[UAC8_MAX_URBS];

	/* chip->lock で保護 */
	struct snd_pcm_substream *substream;
	bool running;
	unsigned int ch;
	unsigned int hwptr;
	unsigned int period_pos;
	unsigned int buffer_size;
	unsigned int period_size;

	/* chip->mutex で保護 */
	unsigned int hw_rate;

	atomic_t notify_busy;
	atomic_t errors;
};

enum {
	UAC8_FLAG_FAILED,
};

struct uac8 {
	struct usb_device *udev;
	struct snd_card *card;
	struct snd_pcm *pcm;

	struct mutex mutex;
	spinlock_t lock;

	struct uac8_stream stream[2];

	bool engine_running;
	bool hw_configured;
	unsigned int rate;

	bool disconnected;
	bool stopping;
	unsigned long flags;
	struct work_struct fail_work;
};

static struct usb_driver uac8_driver;

static inline struct uac8_stream *uac8_stream_of(struct uac8 *chip,
						 struct snd_pcm_substream *ss)
{
	return &chip->stream[ss->stream];
}

/* ------------------------------------------------------------------ */
/* ハードウェア制御                                                   */
/* ------------------------------------------------------------------ */
static int uac8_rate_to_alt(unsigned int rate)
{
	if (rate >= 176400)
		return 1;
	if (rate >= 88200)
		return 2;
	return 3;
}

static int uac8_ctrl_out(struct uac8 *chip, u8 reqtype, u8 req, u16 value,
			 u16 index, const void *data, u16 len)
{
	struct usb_device *udev = chip->udev;
	void *buf;
	int ret;

	if (len) {
		buf = kmemdup(data, len, GFP_KERNEL);
		if (!buf)
			return -ENOMEM;
	} else {
		buf = kmalloc(1, GFP_KERNEL);
		if (!buf)
			return -ENOMEM;
	}

	ret = usb_control_msg(udev, usb_sndctrlpipe(udev, 0), req, reqtype,
			      value, index, buf, len, UAC8_CTRL_TIMEOUT_MS);
	kfree(buf);
	if (ret < 0)
		dev_dbg(&udev->dev,
			"ctrl req=0x%02x type=0x%02x val=0x%04x idx=0x%04x: %d\n",
			req, reqtype, value, index, ret);
	return ret;
}

static bool uac8_dev_gone(struct uac8 *chip)
{
	return READ_ONCE(chip->disconnected) ||
	       chip->udev->state == USB_STATE_NOTATTACHED;
}

static int uac8_configure_hw(struct uac8 *chip, unsigned int rate, int alt)
{
	struct usb_device *udev = chip->udev;
	static const u8 zero2[2] = { 0x00, 0x00 };
	__le32 rate_le = cpu_to_le32(rate);
	u8 clk_sel = 1;
	int err;

	chip->hw_configured = true;

	err = usb_set_interface(udev, UAC8_IF_OUT, 0);
	if (err < 0)
		goto err_if;
	err = usb_set_interface(udev, UAC8_IF_IN, 0);
	if (err < 0)
		goto err_if;
	msleep(20);

	uac8_ctrl_out(chip, 0x21, 0x01, 0x0100, 0x2900, &clk_sel, 1);
	uac8_ctrl_out(chip, 0x21, 0x01, 0x0100, 0x2500, &rate_le, 4);
	uac8_ctrl_out(chip, 0x21, 0x01, 0x0100, 0x2800, &rate_le, 4);

	uac8_ctrl_out(chip, 0x21, 0x01, 0x0100, 0x2900, zero2, 2);
	uac8_ctrl_out(chip, 0x21, 0x01, 0x0200, 0x2900, zero2, 2);
	uac8_ctrl_out(chip, 0x21, 0x01, 0x0201, 0x2900, zero2, 2);
	uac8_ctrl_out(chip, 0x21, 0x01, 0x0202, 0x2900, zero2, 2);

	uac8_ctrl_out(chip, 0x01, 0x0B, alt, UAC8_IF_OUT, NULL, 0);
	uac8_ctrl_out(chip, 0x01, 0x0B, alt, UAC8_IF_IN, NULL, 0);
	uac8_ctrl_out(chip, 0x02, 0x01, 0x0000, UAC8_EP_OUT, NULL, 0);
	uac8_ctrl_out(chip, 0x02, 0x01, 0x0000, UAC8_EP_IN, NULL, 0);
	uac8_ctrl_out(chip, 0x01, 0x0B, alt, UAC8_IF_OUT, NULL, 0);
	uac8_ctrl_out(chip, 0x01, 0x0B, alt, UAC8_IF_IN, NULL, 0);

	if (uac8_dev_gone(chip))
		return -ENODEV;

	err = usb_set_interface(udev, UAC8_IF_OUT, alt);
	if (err < 0)
		goto err_if;
	err = usb_set_interface(udev, UAC8_IF_IN, alt);
	if (err < 0)
		goto err_if;

	return 0;

err_if:
	dev_err(&udev->dev, "usb_set_interface failed: %d\n", err);
	return err;
}

/* ------------------------------------------------------------------ */
/* URB / ストリーミング                                               */
/* ------------------------------------------------------------------ */
static void uac8_fail(struct uac8 *chip)
{
	if (!test_and_set_bit(UAC8_FLAG_FAILED, &chip->flags))
		schedule_work(&chip->fail_work);
}

static bool uac8_stream_active(const struct uac8_stream *s)
{
	const struct snd_pcm_runtime *rt;

	if (!s->running || !s->substream)
		return false;
	rt = s->substream->runtime;
	if (!rt || !rt->dma_area)
		return false;
	if (!s->ch || s->ch > s->max_ch || s->ch * UAC8_SAMPLE_BYTES > UAC8_FRAME_BYTES)
		return false;
	if (!s->buffer_size || !s->period_size)
		return false;
	if ((size_t)s->buffer_size * s->ch * UAC8_SAMPLE_BYTES > rt->dma_bytes)
		return false;
	return true;
}

static inline bool uac8_advance(struct uac8_stream *s)
{
	bool elapsed = false;

	if (++s->hwptr >= s->buffer_size)
		s->hwptr = 0;
	if (++s->period_pos >= s->period_size) {
		s->period_pos = 0;
		elapsed = true;
	}
	return elapsed;
}

static bool uac8_fill_playback(struct uac8_stream *s, u8 *buf)
{
	const struct snd_pcm_runtime *rt = s->substream->runtime;
	const unsigned int cb = s->ch * UAC8_SAMPLE_BYTES;
	const unsigned int pad = UAC8_FRAME_BYTES - cb;
	bool elapsed = false;
	unsigned int i;

	for (i = 0; i < UAC8_URB_FRAMES; i++) {
		u8 *dst = buf + i * UAC8_FRAME_BYTES;

		memcpy(dst, rt->dma_area + (size_t)s->hwptr * cb, cb);
		if (pad)
			memset(dst + cb, 0, pad);
		if (uac8_advance(s))
			elapsed = true;
	}
	return elapsed;
}

static bool uac8_drain_capture(struct uac8_stream *s, const u8 *buf,
			       unsigned int frames)
{
	const struct snd_pcm_runtime *rt = s->substream->runtime;
	const unsigned int cb = s->ch * UAC8_SAMPLE_BYTES;
	bool elapsed = false;
	unsigned int i;

	for (i = 0; i < frames; i++) {
		memcpy(rt->dma_area + (size_t)s->hwptr * cb,
		       buf + i * UAC8_FRAME_BYTES, cb);
		if (uac8_advance(s))
			elapsed = true;
	}
	return elapsed;
}

static bool uac8_urb_ok(struct uac8_stream *s, struct urb *urb)
{
	struct uac8 *chip = s->chip;

	if (READ_ONCE(chip->stopping) ||
	    test_bit(UAC8_FLAG_FAILED, &chip->flags))
		return false;

	switch (urb->status) {
	case 0:
		atomic_set(&s->errors, 0);
		return true;
	case -ESHUTDOWN:
	case -ENODEV:
		return false;
	case -ENOENT:
	case -ECONNRESET:
	case -EPIPE:
		uac8_fail(chip);
		return false;
	default:
		break;
	}

	if (atomic_inc_return(&s->errors) >= UAC8_MAX_ERRORS) {
		uac8_fail(chip);
		return false;
	}
	dev_warn_ratelimited(&chip->udev->dev, "%s URB error %d\n",
			     s->playback ? "playback" : "capture", urb->status);
	return true;
}

static void uac8_resubmit(struct uac8_stream *s, struct urb *urb)
{
	int err;

	if (READ_ONCE(s->chip->stopping))
		return;
	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err && err != -ENODEV && err != -ESHUTDOWN && err != -EPERM) {
		dev_err_ratelimited(&s->chip->udev->dev,
				    "resubmit failed: %d\n", err);
		uac8_fail(s->chip);
	}
}

static void uac8_play_complete(struct urb *urb)
{
	struct uac8_urb_ctx *c = urb->context;
	struct uac8_stream *s = c->stream;
	struct uac8 *chip = s->chip;
	struct snd_pcm_substream *notify = NULL;
	unsigned long flags;

	if (!uac8_urb_ok(s, urb))
		return;

	spin_lock_irqsave(&chip->lock, flags);
	if (uac8_stream_active(s)) {
		if (uac8_fill_playback(s, c->buf)) {
			notify = s->substream;
			atomic_inc(&s->notify_busy);
		}
	} else {
		memset(c->buf, 0, UAC8_URB_BYTES);
	}
	spin_unlock_irqrestore(&chip->lock, flags);

	uac8_resubmit(s, urb);

	if (notify) {
		snd_pcm_period_elapsed(notify);
		smp_mb__before_atomic();
		atomic_dec(&s->notify_busy);
	}
}

static void uac8_capture_complete(struct urb *urb)
{
	struct uac8_urb_ctx *c = urb->context;
	struct uac8_stream *s = c->stream;
	struct uac8 *chip = s->chip;
	struct snd_pcm_substream *notify = NULL;
	unsigned int frames;
	unsigned long flags;

	if (!uac8_urb_ok(s, urb))
		return;

	frames = min_t(unsigned int, urb->actual_length / UAC8_FRAME_BYTES,
		       UAC8_URB_FRAMES);
	if (urb->actual_length % UAC8_FRAME_BYTES)
		dev_warn_ratelimited(&chip->udev->dev,
				     "capture: partial frame (%u bytes)\n",
				     urb->actual_length);

	spin_lock_irqsave(&chip->lock, flags);
	if (frames && uac8_stream_active(s)) {
		if (uac8_drain_capture(s, c->buf, frames)) {
			notify = s->substream;
			atomic_inc(&s->notify_busy);
		}
	}
	spin_unlock_irqrestore(&chip->lock, flags);

	uac8_resubmit(s, urb);

	if (notify) {
		snd_pcm_period_elapsed(notify);
		smp_mb__before_atomic();
		atomic_dec(&s->notify_busy);
	}
}

static void uac8_free_stream(struct uac8_stream *s)
{
	struct usb_device *udev = s->chip->udev;
	int i;

	for (i = 0; i < UAC8_MAX_URBS; i++) {
		struct uac8_urb_ctx *c = &s->ctx[i];

		if (c->buf)
			usb_free_coherent(udev, UAC8_URB_BYTES, c->buf,
					  c->urb->transfer_dma);
		usb_free_urb(c->urb);
		c->buf = NULL;
		c->urb = NULL;
	}
	s->active_urbs = 0;
}

static int uac8_alloc_stream(struct uac8_stream *s, unsigned int pipe,
			     usb_complete_t complete, unsigned int num_urbs)
{
	struct usb_device *udev = s->chip->udev;
	int i;

	s->active_urbs = num_urbs;
	for (i = 0; i < num_urbs; i++) {
		struct uac8_urb_ctx *c = &s->ctx[i];

		c->stream = s;
		c->urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!c->urb)
			return -ENOMEM;
		c->buf = usb_alloc_coherent(udev, UAC8_URB_BYTES, GFP_KERNEL,
					    &c->urb->transfer_dma);
		if (!c->buf)
			return -ENOMEM;
		memset(c->buf, 0, UAC8_URB_BYTES);
		usb_fill_bulk_urb(c->urb, udev, pipe, c->buf, UAC8_URB_BYTES,
				  complete, c);
		c->urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	}
	return 0;
}

static void uac8_kill_stream(struct uac8_stream *s)
{
	int i;

	for (i = 0; i < s->active_urbs; i++)
		if (s->ctx[i].urb)
			usb_kill_urb(s->ctx[i].urb);
}

static int uac8_submit_stream(struct uac8_stream *s)
{
	int i, err;

	for (i = 0; i < s->active_urbs; i++) {
		err = usb_submit_urb(s->ctx[i].urb, GFP_KERNEL);
		if (err) {
			dev_err(&s->chip->udev->dev,
				"initial URB submit failed: %d\n", err);
			return err;
		}
	}
	return 0;
}

static bool uac8_ep_is_bulk(struct usb_device *udev, unsigned int pipe)
{
	struct usb_host_endpoint *ep = usb_pipe_endpoint(udev, pipe);

	return ep && usb_endpoint_xfer_bulk(&ep->desc);
}

static void uac8_stop_engine(struct uac8 *chip)
{
	lockdep_assert_held(&chip->mutex);

	WRITE_ONCE(chip->stopping, true);
	uac8_kill_stream(&chip->stream[SNDRV_PCM_STREAM_CAPTURE]);
	uac8_kill_stream(&chip->stream[SNDRV_PCM_STREAM_PLAYBACK]);
	uac8_free_stream(&chip->stream[SNDRV_PCM_STREAM_CAPTURE]);
	uac8_free_stream(&chip->stream[SNDRV_PCM_STREAM_PLAYBACK]);
	chip->engine_running = false;

	if (chip->hw_configured) {
		chip->hw_configured = false;
		if (!uac8_dev_gone(chip)) {
			usb_set_interface(chip->udev, UAC8_IF_OUT, 0);
			usb_set_interface(chip->udev, UAC8_IF_IN, 0);
		}
	}
}

/*
 * 周期サイズ (period_size) に応じて投入する URB 本数を動的に決定する。
 * 1 URB = 8 フレーム。
 */
static unsigned int uac8_calc_urbs(unsigned int period_size)
{
	unsigned int urbs;

	if (period_size <= 32)
		urbs = 8;
	else if (period_size <= 64)
		urbs = 16;
	else if (period_size <= 128)
		urbs = 32;
	else if (period_size <= 512)
		urbs = 64;
	else
		urbs = UAC8_MAX_URBS;

	return clamp_val(urbs, 8U, (unsigned int)UAC8_MAX_URBS);
}

static int uac8_start_engine(struct uac8 *chip, unsigned int rate,
			     unsigned int period_size)
{
	struct usb_device *udev = chip->udev;
	struct uac8_stream *play = &chip->stream[SNDRV_PCM_STREAM_PLAYBACK];
	struct uac8_stream *cap = &chip->stream[SNDRV_PCM_STREAM_CAPTURE];
	unsigned int pipe_out, pipe_in;
	unsigned int num_urbs;
	int err;

	lockdep_assert_held(&chip->mutex);

	if (READ_ONCE(chip->disconnected))
		return -ENODEV;

	num_urbs = uac8_calc_urbs(period_size);

	err = uac8_configure_hw(chip, rate, uac8_rate_to_alt(rate));
	if (err)
		goto err_stop;

	pipe_out = usb_sndbulkpipe(udev, UAC8_EP_OUT & USB_ENDPOINT_NUMBER_MASK);
	pipe_in = usb_rcvbulkpipe(udev, UAC8_EP_IN & USB_ENDPOINT_NUMBER_MASK);
	if (!uac8_ep_is_bulk(udev, pipe_out) || !uac8_ep_is_bulk(udev, pipe_in)) {
		dev_err(&udev->dev, "bulk endpoints not present in alt %d\n",
			uac8_rate_to_alt(rate));
		err = -ENODEV;
		goto err_stop;
	}

	err = uac8_alloc_stream(cap, pipe_in, uac8_capture_complete, num_urbs);
	if (err)
		goto err_stop;
	err = uac8_alloc_stream(play, pipe_out, uac8_play_complete, num_urbs);
	if (err)
		goto err_stop;

	clear_bit(UAC8_FLAG_FAILED, &chip->flags);
	atomic_set(&cap->errors, 0);
	atomic_set(&play->errors, 0);
	chip->rate = rate;
	WRITE_ONCE(chip->stopping, false);

	err = uac8_submit_stream(cap);
	if (err)
		goto err_stop;
	err = uac8_submit_stream(play);
	if (err)
		goto err_stop;

	chip->engine_running = true;
	dev_info(&udev->dev, "streaming started: %u Hz, alt %d, queue %u URBs (%u frames ahead)\n",
		 rate, uac8_rate_to_alt(rate), num_urbs, num_urbs * UAC8_URB_FRAMES);
	return 0;

err_stop:
	uac8_stop_engine(chip);
	return err;
}

static void uac8_fail_work(struct work_struct *work)
{
	struct uac8 *chip = container_of(work, struct uac8, fail_work);
	int i;

	mutex_lock(&chip->mutex);
	if (!READ_ONCE(chip->disconnected) &&
	    test_bit(UAC8_FLAG_FAILED, &chip->flags)) {
		dev_err(&chip->udev->dev,
			"USB streaming error: stopping PCM streams\n");
		for (i = 0; i < 2; i++) {
			if (chip->stream[i].substream)
				snd_pcm_stop_xrun(chip->stream[i].substream);
		}
	}
	mutex_unlock(&chip->mutex);
}

/* ------------------------------------------------------------------ */
/* PCM                                                                */
/* ------------------------------------------------------------------ */
static const unsigned int uac8_rates[] = {
	44100, 48000, 88200, 96000, 176400, 192000
};
static const struct snd_pcm_hw_constraint_list uac8_rate_list = {
	.list = uac8_rates,
	.count = ARRAY_SIZE(uac8_rates),
};

/* 32 から 2048 までの 2 の累乗ステップ一覧 */
static const unsigned int uac8_periods_list[] = {
	32, 64, 128, 256, 512, 1024, 2048
};
static const struct snd_pcm_hw_constraint_list uac8_period_sizes = {
	.list = uac8_periods_list,
	.count = ARRAY_SIZE(uac8_periods_list),
};

static const struct snd_pcm_hardware uac8_pcm_hw = {
	.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_BATCH | SNDRV_PCM_INFO_JOINT_DUPLEX,
	.formats = SNDRV_PCM_FMTBIT_S32_LE,
	.rates = SNDRV_PCM_RATE_KNOT,
	.rate_min = 44100,
	.rate_max = 192000,
	.channels_min = 1,
	.channels_max = UAC8_PLAY_CHANNELS,
	.buffer_bytes_max = UAC8_BUFFER_FRAMES_MAX * UAC8_SAMPLE_BYTES *
			    UAC8_PHYS_CHANNELS,
	.period_bytes_min = 32 * UAC8_SAMPLE_BYTES,
	.period_bytes_max = 2048 * UAC8_SAMPLE_BYTES * UAC8_PHYS_CHANNELS,
	.periods_min = UAC8_PERIODS_MIN,
	.periods_max = UAC8_PERIODS_MAX,
};

static void uac8_wait_idle(struct uac8_stream *s)
{
	int i;

	for (i = 0; i < 2000 && atomic_read_acquire(&s->notify_busy); i++)
		usleep_range(50, 100);
}

static int uac8_pcm_open(struct snd_pcm_substream *ss)
{
	struct uac8 *chip = snd_pcm_substream_chip(ss);
	struct snd_pcm_runtime *rt = ss->runtime;
	struct uac8_stream *s = uac8_stream_of(chip, ss);
	struct uac8_stream *other = &chip->stream[!ss->stream];
	unsigned long flags;
	int err;

	mutex_lock(&chip->mutex);
	if (READ_ONCE(chip->disconnected)) {
		err = -ENODEV;
		goto out;
	}

	rt->hw = uac8_pcm_hw;
	rt->hw.channels_max = s->max_ch;
	rt->hw.buffer_bytes_max = UAC8_BUFFER_FRAMES_MAX * UAC8_SAMPLE_BYTES *
				  s->max_ch;
	rt->hw.period_bytes_max = 2048 * UAC8_SAMPLE_BYTES * s->max_ch;

	err = snd_pcm_hw_constraint_list(rt, 0, SNDRV_PCM_HW_PARAM_RATE,
					 &uac8_rate_list);
	if (err < 0)
		goto out;

	if (other->hw_rate) {
		err = snd_pcm_hw_constraint_minmax(rt, SNDRV_PCM_HW_PARAM_RATE,
						   other->hw_rate,
						   other->hw_rate);
		if (err < 0)
			goto out;
	}

	/* 32〜2048 のリストを制約として明示 */
	err = snd_pcm_hw_constraint_list(rt, 0, SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
					 &uac8_period_sizes);
	if (err < 0)
		goto out;

	err = snd_pcm_hw_constraint_step(rt, 0, SNDRV_PCM_HW_PARAM_BUFFER_SIZE,
					 UAC8_URB_FRAMES);
	if (err < 0)
		goto out;

	err = snd_pcm_hw_constraint_integer(rt, SNDRV_PCM_HW_PARAM_PERIODS);
	if (err < 0)
		goto out;

	spin_lock_irqsave(&chip->lock, flags);
	s->running = false;
	s->ch = 0;
	s->hwptr = 0;
	s->period_pos = 0;
	s->buffer_size = 0;
	s->period_size = 0;
	s->substream = ss;
	spin_unlock_irqrestore(&chip->lock, flags);
	err = 0;
out:
	mutex_unlock(&chip->mutex);
	return err;
}

static int uac8_pcm_close(struct snd_pcm_substream *ss)
{
	struct uac8 *chip = snd_pcm_substream_chip(ss);
	struct uac8_stream *s = uac8_stream_of(chip, ss);
	unsigned long flags;

	mutex_lock(&chip->mutex);

	spin_lock_irqsave(&chip->lock, flags);
	s->running = false;
	s->substream = NULL;
	spin_unlock_irqrestore(&chip->lock, flags);
	uac8_wait_idle(s);

	s->hw_rate = 0;
	if (!chip->stream[0].substream && !chip->stream[1].substream)
		uac8_stop_engine(chip);

	mutex_unlock(&chip->mutex);
	return 0;
}

static int uac8_pcm_hw_params(struct snd_pcm_substream *ss,
			      struct snd_pcm_hw_params *hw)
{
	struct uac8 *chip = snd_pcm_substream_chip(ss);
	struct uac8_stream *s = uac8_stream_of(chip, ss);
	struct uac8_stream *other = &chip->stream[!ss->stream];
	unsigned int rate = params_rate(hw);
	int err = 0;

	mutex_lock(&chip->mutex);
	if (READ_ONCE(chip->disconnected))
		err = -ENODEV;
	else if (other->hw_rate && other->hw_rate != rate)
		err = -EBUSY;
	else
		s->hw_rate = rate;
	mutex_unlock(&chip->mutex);
	return err;
}

static int uac8_pcm_hw_free(struct snd_pcm_substream *ss)
{
	struct uac8 *chip = snd_pcm_substream_chip(ss);
	struct uac8_stream *s = uac8_stream_of(chip, ss);

	mutex_lock(&chip->mutex);
	s->hw_rate = 0;
	mutex_unlock(&chip->mutex);
	return 0;
}

static int uac8_pcm_sync_stop(struct snd_pcm_substream *ss)
{
	struct uac8 *chip = snd_pcm_substream_chip(ss);

	uac8_wait_idle(uac8_stream_of(chip, ss));
	return 0;
}

static int uac8_pcm_prepare(struct snd_pcm_substream *ss)
{
	struct uac8 *chip = snd_pcm_substream_chip(ss);
	struct snd_pcm_runtime *rt = ss->runtime;
	struct uac8_stream *s = uac8_stream_of(chip, ss);
	unsigned long flags;
	int err = 0;

	if (rt->channels < 1 || rt->channels > s->max_ch ||
	    !rt->period_size || !rt->buffer_size || !rt->dma_area)
		return -EINVAL;
	if ((size_t)rt->buffer_size * rt->channels * UAC8_SAMPLE_BYTES >
	    rt->dma_bytes)
		return -EINVAL;

	mutex_lock(&chip->mutex);
	if (READ_ONCE(chip->disconnected)) {
		err = -ENODEV;
		goto out;
	}

	if (chip->engine_running &&
	    (chip->rate != rt->rate ||
	     test_bit(UAC8_FLAG_FAILED, &chip->flags)))
		uac8_stop_engine(chip);

	if (!chip->engine_running) {
		err = uac8_start_engine(chip, rt->rate, rt->period_size);
		if (err)
			goto out;
	}

	if (s->playback)
		memset(rt->dma_area, 0, rt->dma_bytes);

	spin_lock_irqsave(&chip->lock, flags);
	s->running = false;
	s->ch = rt->channels;
	s->buffer_size = rt->buffer_size;
	s->period_size = rt->period_size;
	s->hwptr = 0;
	s->period_pos = 0;
	spin_unlock_irqrestore(&chip->lock, flags);

	/* 再生ディレイを実際に投入された URB 本数に合わせて動的計算 */
	rt->delay = s->playback ? s->active_urbs * UAC8_URB_FRAMES : 0;
out:
	mutex_unlock(&chip->mutex);
	return err;
}

static int uac8_pcm_trigger(struct snd_pcm_substream *ss, int cmd)
{
	struct uac8 *chip = snd_pcm_substream_chip(ss);
	struct uac8_stream *s = uac8_stream_of(chip, ss);
	unsigned long flags;
	int err = 0;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		if (READ_ONCE(chip->disconnected))
			return -ENODEV;
		if (test_bit(UAC8_FLAG_FAILED, &chip->flags))
			return -EIO;
		spin_lock_irqsave(&chip->lock, flags);
		s->running = true;
		spin_unlock_irqrestore(&chip->lock, flags);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		spin_lock_irqsave(&chip->lock, flags);
		s->running = false;
		spin_unlock_irqrestore(&chip->lock, flags);
		break;
	default:
		err = -EINVAL;
		break;
	}
	return err;
}

static snd_pcm_uframes_t uac8_pcm_pointer(struct snd_pcm_substream *ss)
{
	struct uac8 *chip = snd_pcm_substream_chip(ss);
	struct uac8_stream *s = uac8_stream_of(chip, ss);
	unsigned long flags;
	unsigned int pos;

	if (READ_ONCE(chip->disconnected))
		return SNDRV_PCM_POS_XRUN;

	spin_lock_irqsave(&chip->lock, flags);
	pos = s->hwptr;
	spin_unlock_irqrestore(&chip->lock, flags);
	return pos;
}

static const struct snd_pcm_ops uac8_pcm_ops = {
	.open		= uac8_pcm_open,
	.close		= uac8_pcm_close,
	.hw_params	= uac8_pcm_hw_params,
	.hw_free	= uac8_pcm_hw_free,
	.prepare	= uac8_pcm_prepare,
	.trigger	= uac8_pcm_trigger,
	.sync_stop	= uac8_pcm_sync_stop,
	.pointer	= uac8_pcm_pointer,
};

/* ------------------------------------------------------------------ */
/* USB ドライバ                                                       */
/* ------------------------------------------------------------------ */
static const struct usb_endpoint_descriptor *
uac8_find_bulk_ep(struct usb_interface *iface, int alt, u8 addr)
{
	struct usb_host_interface *alts = usb_altnum_to_altsetting(iface, alt);
	int i;

	if (!alts)
		return NULL;
	for (i = 0; i < alts->desc.bNumEndpoints; i++) {
		const struct usb_endpoint_descriptor *d = &alts->endpoint[i].desc;

		if (d->bEndpointAddress == addr && usb_endpoint_xfer_bulk(d))
			return d;
	}
	return NULL;
}

static int uac8_check_alts(struct usb_interface *intf,
			   struct usb_interface *out_if,
			   struct usb_interface *in_if)
{
	static const int required_alts[] = { 1, 2, 3 };
	int i;

	for (i = 0; i < ARRAY_SIZE(required_alts); i++) {
		if (!uac8_find_bulk_ep(out_if, required_alts[i], UAC8_EP_OUT) ||
		    !uac8_find_bulk_ep(in_if, required_alts[i], UAC8_EP_IN)) {
			dev_err(&intf->dev,
				"alt %d: bulk EP 0x%02x/0x%02x not found, unsupported firmware?\n",
				required_alts[i], UAC8_EP_OUT, UAC8_EP_IN);
			return -ENODEV;
		}
	}
	return 0;
}

static int uac8_claim_companion(struct usb_interface *intf,
				struct usb_interface *iface)
{
	struct device_driver *cur = iface->dev.driver;

	if (cur) {
		if (to_usb_driver(cur) == &uac8_driver) {
			usb_set_intfdata(iface, UAC8_COMPANION);
			return 0;
		}
		dev_err(&intf->dev,
			"interface %d is bound to '%s'; disable snd-usb-audio for this device\n",
			iface->cur_altsetting->desc.bInterfaceNumber,
			cur->name);
		return -EBUSY;
	}
	return usb_driver_claim_interface(&uac8_driver, iface, UAC8_COMPANION);
}

static void uac8_card_free(struct snd_card *card)
{
	struct uac8 *chip = card->private_data;

	cancel_work_sync(&chip->fail_work);
	usb_put_dev(chip->udev);
}

static int uac8_probe(struct usb_interface *intf,
		      const struct usb_device_id *usb_id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct usb_interface *out_if, *in_if;
	struct snd_card *card;
	struct snd_pcm *pcm;
	struct uac8 *chip;
	char path[64];
	int err;

	if (intf->cur_altsetting->desc.bInterfaceNumber != UAC8_IF_CTRL)
		return -ENODEV;

	out_if = usb_ifnum_to_if(udev, UAC8_IF_OUT);
	in_if = usb_ifnum_to_if(udev, UAC8_IF_IN);
	if (!out_if || !in_if) {
		dev_err(&intf->dev, "audio streaming interfaces not found\n");
		return -ENODEV;
	}
	err = uac8_check_alts(intf, out_if, in_if);
	if (err)
		return err;

	err = snd_card_new(&intf->dev, card_index, card_id, THIS_MODULE,
			   sizeof(*chip), &card);
	if (err < 0)
		return err;

	chip = card->private_data;
	chip->card = card;
	chip->udev = usb_get_dev(udev);
	mutex_init(&chip->mutex);
	spin_lock_init(&chip->lock);
	INIT_WORK(&chip->fail_work, uac8_fail_work);
	chip->stream[SNDRV_PCM_STREAM_PLAYBACK].chip = chip;
	chip->stream[SNDRV_PCM_STREAM_PLAYBACK].playback = true;
	chip->stream[SNDRV_PCM_STREAM_PLAYBACK].max_ch = UAC8_PLAY_CHANNELS;
	chip->stream[SNDRV_PCM_STREAM_CAPTURE].chip = chip;
	chip->stream[SNDRV_PCM_STREAM_CAPTURE].playback = false;
	chip->stream[SNDRV_PCM_STREAM_CAPTURE].max_ch = UAC8_CAP_CHANNELS;
	card->private_free = uac8_card_free;

	strscpy(card->driver, "snd-uac8", sizeof(card->driver));
	strscpy(card->shortname, "ZOOM UAC-8", sizeof(card->shortname));
	usb_make_path(udev, path, sizeof(path));
	snprintf(card->longname, sizeof(card->longname),
		 "ZOOM UAC-8 (%04x:%04x) at usb-%s",
		 le16_to_cpu(udev->descriptor.idVendor),
		 le16_to_cpu(udev->descriptor.idProduct), path);

	err = snd_pcm_new(card, "UAC-8", 0, 1, 1, &pcm);
	if (err < 0)
		goto err_free;
	chip->pcm = pcm;
	pcm->private_data = chip;
	strscpy(pcm->name, "ZOOM UAC-8", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &uac8_pcm_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &uac8_pcm_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL, 0, 0);

	err = uac8_claim_companion(intf, out_if);
	if (err)
		goto err_free;
	err = uac8_claim_companion(intf, in_if);
	if (err)
		goto err_free;

	usb_set_intfdata(intf, chip);
	err = snd_card_register(card);
	if (err < 0) {
		usb_set_intfdata(intf, NULL);
		goto err_free;
	}

	dev_info(&intf->dev, "ZOOM UAC-8 ready: playback %dch / capture %dch (44.1k-192k, 32-2048 frames supported)\n",
		 UAC8_PLAY_CHANNELS, UAC8_CAP_CHANNELS);
	return 0;

err_free:
	snd_card_free(card);
	return err;
}

static void uac8_disconnect(struct usb_interface *intf)
{
	struct uac8 *chip = usb_get_intfdata(intf);

	if (!chip || chip == UAC8_COMPANION)
		return;

	WRITE_ONCE(chip->disconnected, true);
	snd_card_disconnect(chip->card);

	mutex_lock(&chip->mutex);
	uac8_stop_engine(chip);
	mutex_unlock(&chip->mutex);

	cancel_work_sync(&chip->fail_work);

	usb_set_intfdata(intf, NULL);
	snd_card_free_when_closed(chip->card);
}

static int uac8_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct uac8 *chip = usb_get_intfdata(intf);

	if (!chip || chip == UAC8_COMPANION)
		return 0;

	snd_power_change_state(chip->card, SNDRV_CTL_POWER_D3hot);
	snd_pcm_suspend_all(chip->pcm);

	mutex_lock(&chip->mutex);
	uac8_stop_engine(chip);
	mutex_unlock(&chip->mutex);
	return 0;
}

static int uac8_resume(struct usb_interface *intf)
{
	struct uac8 *chip = usb_get_intfdata(intf);

	if (!chip || chip == UAC8_COMPANION)
		return 0;

	snd_power_change_state(chip->card, SNDRV_CTL_POWER_D0);
	return 0;
}

static const struct usb_device_id uac8_ids[] = {
	{ USB_DEVICE(UAC8_VENDOR_ID, UAC8_PRODUCT_ID) },
	{ }
};
MODULE_DEVICE_TABLE(usb, uac8_ids);

static struct usb_driver uac8_driver = {
	.name = "snd-uac8",
	.id_table = uac8_ids,
	.probe = uac8_probe,
	.disconnect = uac8_disconnect,
	.suspend = uac8_suspend,
	.resume = uac8_resume,
	.reset_resume = uac8_resume,
	.supports_autosuspend = 0,
};

module_usb_driver(uac8_driver);
