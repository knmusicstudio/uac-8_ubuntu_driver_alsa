// SPDX-License-Identifier: GPL-2.0
/*
 * snd-uac8 - ALSA USB オーディオドライバ (ZOOM UAC-8, USB 1686:f02b)
 *
 * libusb + JACK + GTK で書かれていたユーザ空間ブリッジ (main.c) を、
 * カーネル内の ALSA ドライバとして書き直したもの。
 *
 *   元の main.c                               このドライバ
 *   ------------------------------------  ---------------------------------
 *   libusb bulk transfer x128 (IN/OUT)    bulk URB x128 (IN/OUT)
 *   リングバッファ + JACK process()       ALSA PCM バッファ (URB 完了で直接コピー)
 *   JACK capture 18ch / playback 20ch     PCM capture 1..18ch / playback 1..20ch
 *   configure_uac8_hardware()             uac8_configure_hw()
 *   サンプルレート→Alt 切替               uac8_rate_to_alt()
 *   Start/Stop ボタン                     PCM prepare / close
 *   GTK / jackd / a2jmidid                不要 (ALSA がそのまま見える)
 *
 * パニック対策の方針:
 *   - URB のライフサイクルは「全 URB を usb_kill_urb() で個別に殺してから解放」
 *     に統一 (完了ハンドラ実行中の解放を防ぐ)
 *   - 完了ハンドラ内では sleep しない / 自前ロックを持ったまま ALSA を呼ばない
 *   - ハンドラで触る PCM 状態は chip->lock で保護し、substream の close 後に
 *     触らないよう running/substream を同ロック下で落としてから待ち合わせる
 *   - エラー連発時は無限再投入せず、work で PCM を XRUN にして止める
 *   - 配列添字・除算・バッファ境界は全部ハンドラ内でも検証する
 *   - デバイス切断後は USB 通信を一切しない (disconnected / udev->state)
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
/* デバイス定数 (元の main.c の #define に対応)                         */
/* ------------------------------------------------------------------ */
#define UAC8_VENDOR_ID		0x1686
#define UAC8_PRODUCT_ID		0xf02b

#define UAC8_IF_CTRL		0	/* AudioControl */
#define UAC8_IF_OUT		1	/* AudioStreaming: host -> device */
#define UAC8_IF_IN		2	/* AudioStreaming: device -> host */

#define UAC8_EP_OUT		0x01	/* EP_AUDIO_OUT */
#define UAC8_EP_IN		0x82	/* EP_AUDIO_IN  */

#define UAC8_PHYS_CHANNELS	32	/* NUM_PHYS_CHANNELS */
#define UAC8_SAMPLE_BYTES	4	/* S32_LE */
#define UAC8_FRAME_BYTES	(UAC8_PHYS_CHANNELS * UAC8_SAMPLE_BYTES)	/* 128 */

#define UAC8_URB_BYTES		1024	/* PACKET_SIZE */
#define UAC8_URB_FRAMES		(UAC8_URB_BYTES / UAC8_FRAME_BYTES)	/* 8 */
#define UAC8_NUM_URBS		128	/* NUM_TRANSFERS (各方向) */

#define UAC8_PLAY_CHANNELS	20	/* JACK_OUT_CHANNELS */
#define UAC8_CAP_CHANNELS	18	/* JACK_IN_CHANNELS  */

#define UAC8_PERIOD_FRAMES_MIN	32
#define UAC8_PERIOD_FRAMES_MAX	16384
#define UAC8_BUFFER_FRAMES_MAX	32768
#define UAC8_PERIODS_MIN	2
#define UAC8_PERIODS_MAX	64

#define UAC8_CTRL_TIMEOUT_MS	1000
#define UAC8_MAX_ERRORS		32	/* 連続エラーがこの数に達したら諦める */

#define UAC8_COMPANION		((void *)-1L)	/* claim しただけの IF の目印 */

static_assert(UAC8_URB_BYTES % UAC8_FRAME_BYTES == 0);
static_assert(UAC8_PLAY_CHANNELS <= UAC8_PHYS_CHANNELS);
static_assert(UAC8_CAP_CHANNELS <= UAC8_PHYS_CHANNELS);

/* ------------------------------------------------------------------ */
/* モジュールパラメータ                                                */
/* ------------------------------------------------------------------ */
static int card_index = SNDRV_DEFAULT_IDX1;
static char *card_id = SNDRV_DEFAULT_STR1;
static bool ext_rates;

module_param_named(index, card_index, int, 0444);
MODULE_PARM_DESC(index, "Index value for the UAC-8 sound card.");
module_param_named(id, card_id, charp, 0444);
MODULE_PARM_DESC(id, "ID string for the UAC-8 sound card.");
module_param(ext_rates, bool, 0444);
MODULE_PARM_DESC(ext_rates,
		 "Also offer 44.1/48/88.2/176.4/192 kHz (UNTESTED; default: 96 kHz only).");

/* ------------------------------------------------------------------ */
/* 構造体                                                              */
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
	struct uac8_urb_ctx ctx[UAC8_NUM_URBS];

	/* ---- chip->lock で保護 (URB 完了ハンドラからも触る) ---- */
	struct snd_pcm_substream *substream;	/* open 中のみ非 NULL */
	bool running;				/* trigger START 済み */
	unsigned int ch;			/* PCM のチャンネル数 */
	unsigned int hwptr;			/* フレーム単位 */
	unsigned int period_pos;
	unsigned int buffer_size;		/* フレーム */
	unsigned int period_size;		/* フレーム */

	/* ---- chip->mutex で保護 ---- */
	unsigned int hw_rate;			/* hw_params 済みレート (0=未設定) */

	atomic_t notify_busy;			/* snd_pcm_period_elapsed 実行中の数 */
	atomic_t errors;			/* 連続 URB エラー数 */
};

enum {
	UAC8_FLAG_FAILED,			/* ストリーミング継続不能 */
};

struct uac8 {
	struct usb_device *udev;
	struct snd_card *card;
	struct snd_pcm *pcm;

	struct mutex mutex;			/* open/close/prepare/engine 制御 */
	spinlock_t lock;			/* PCM 状態 (ハンドラと共有) */

	struct uac8_stream stream[2];		/* [SNDRV_PCM_STREAM_*] */

	/* mutex 保護 */
	bool engine_running;
	bool hw_configured;
	unsigned int rate;

	bool disconnected;			/* READ_ONCE/WRITE_ONCE */
	bool stopping;				/* READ_ONCE/WRITE_ONCE */
	unsigned long flags;			/* UAC8_FLAG_* (atomic bitops) */
	struct work_struct fail_work;
};

static struct usb_driver uac8_driver;

static inline struct uac8_stream *uac8_stream_of(struct uac8 *chip,
						 struct snd_pcm_substream *ss)
{
	return &chip->stream[ss->stream];
}

/* ------------------------------------------------------------------ */
/* ハードウェア初期化 (元: configure_uac8_hardware)                    */
/* ------------------------------------------------------------------ */
static int uac8_rate_to_alt(unsigned int rate)
{
	if (rate >= 176400)
		return 1;
	if (rate >= 88200)
		return 2;
	return 3;
}

/* 戻り値は無視してよい (元コードも無視していた) 制御転送。DMA 安全なコピーを使う */
static int uac8_ctrl_out(struct uac8 *chip, u8 reqtype, u8 req, u16 value,
			 u16 index, const void *data, u16 len)
{
	struct usb_device *udev = chip->udev;
	void *buf = NULL;
	int ret;

	if (len) {
		buf = kmemdup(data, len, GFP_KERNEL);
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

/* chip->mutex 保持・URB 未投入の状態で呼ぶこと */
static int uac8_configure_hw(struct uac8 *chip, unsigned int rate, int alt)
{
	struct usb_device *udev = chip->udev;
	static const u8 zero2[2] = { 0x00, 0x00 };
	__le32 rate_le = cpu_to_le32(rate);
	u8 clk_sel = 1;
	int err;

	chip->hw_configured = true;

	/* Alt 0 に落として落ち着かせる */
	err = usb_set_interface(udev, UAC8_IF_OUT, 0);
	if (err < 0)
		goto err_if;
	err = usb_set_interface(udev, UAC8_IF_IN, 0);
	if (err < 0)
		goto err_if;
	msleep(20);

	/* クロックセレクタ / サンプリング周波数 (UAC2 class request) */
	uac8_ctrl_out(chip, 0x21, 0x01, 0x0100, 0x2900, &clk_sel, 1);
	uac8_ctrl_out(chip, 0x21, 0x01, 0x0100, 0x2500, &rate_le, 4);
	uac8_ctrl_out(chip, 0x21, 0x01, 0x0100, 0x2800, &rate_le, 4);

	/* ミュート解除 / ボリューム 0 (元コードのまま) */
	uac8_ctrl_out(chip, 0x21, 0x01, 0x0100, 0x2900, zero2, 2);
	uac8_ctrl_out(chip, 0x21, 0x01, 0x0200, 0x2900, zero2, 2);
	uac8_ctrl_out(chip, 0x21, 0x01, 0x0201, 0x2900, zero2, 2);
	uac8_ctrl_out(chip, 0x21, 0x01, 0x0202, 0x2900, zero2, 2);

	/*
	 * 生の SET_INTERFACE(0x0B) / CLEAR_FEATURE(HALT)(0x01) を元コードと
	 * 同じ順序で送る。最後に usb_set_interface() でホスト側の状態を合わせる。
	 */
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
/* URB 管理                                                            */
/* ------------------------------------------------------------------ */
static void uac8_fail(struct uac8 *chip)
{
	if (!test_and_set_bit(UAC8_FLAG_FAILED, &chip->flags))
		schedule_work(&chip->fail_work);
}

/* chip->lock 保持中に呼ぶ。ハンドラが PCM バッファを安全に触れる状態か */
static bool uac8_stream_active(const struct uac8_stream *s)
{
	const struct snd_pcm_runtime *rt;

	if (!s->running || !s->substream)
		return false;
	rt = s->substream->runtime;
	if (!rt || !rt->dma_area)
		return false;
	if (!s->ch || s->ch * UAC8_SAMPLE_BYTES > UAC8_FRAME_BYTES)
		return false;
	if (!s->buffer_size || !s->period_size)
		return false;
	if ((size_t)s->buffer_size * s->ch * UAC8_SAMPLE_BYTES > rt->dma_bytes)
		return false;
	return true;
}

/* 1 フレーム進める。period 境界を跨いだら true */
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

/* PCM -> URB バッファ (32ch フレームの先頭 ch 分を埋め、残りは無音) */
static bool uac8_fill_playback(struct uac8_stream *s, u8 *buf)
{
	const struct snd_pcm_runtime *rt = s->substream->runtime;
	const unsigned int cb = s->ch * UAC8_SAMPLE_BYTES;
	bool elapsed = false;
	unsigned int i;

	for (i = 0; i < UAC8_URB_FRAMES; i++) {
		u8 *dst = buf + i * UAC8_FRAME_BYTES;

		memcpy(dst, rt->dma_area + (size_t)s->hwptr * cb, cb);
		memset(dst + cb, 0, UAC8_FRAME_BYTES - cb);
		if (uac8_advance(s))
			elapsed = true;
	}
	return elapsed;
}

/* URB バッファ -> PCM (32ch フレームの先頭 ch 分だけ取る) */
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

/* 再投入してよい URB か判定。無限エラーループ防止もここで行う */
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
	case -ESHUTDOWN:	/* 切断 / endpoint 無効化 */
	case -ENODEV:
		return false;
	case -ENOENT:		/* 想定外の unlink */
	case -ECONNRESET:
	case -EPIPE:		/* STALL: process context での復旧が必要 */
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

/* OUT 完了: 次に送る 8 フレームを用意して再投入 (元: cb_out) */
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
		/* PCM が動いていない間は無音を送り続ける (元: underrun 時 memset 0) */
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

/* IN 完了: 受信フレームを PCM へ入れて再投入 (元: cb_in) */
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

	for (i = 0; i < UAC8_NUM_URBS; i++) {
		struct uac8_urb_ctx *c = &s->ctx[i];

		if (c->buf)
			usb_free_coherent(udev, UAC8_URB_BYTES, c->buf,
					  c->urb->transfer_dma);
		usb_free_urb(c->urb);
		c->buf = NULL;
		c->urb = NULL;
	}
}

static int uac8_alloc_stream(struct uac8_stream *s, unsigned int pipe,
			     usb_complete_t complete)
{
	struct usb_device *udev = s->chip->udev;
	int i;

	for (i = 0; i < UAC8_NUM_URBS; i++) {
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

/*
 * 全 URB を個別に kill する。usb_kill_urb() は「その URB の完了ハンドラが
 * 戻るまで」待つので、これが終われば全ハンドラは確実に退出済み。
 */
static void uac8_kill_stream(struct uac8_stream *s)
{
	int i;

	for (i = 0; i < UAC8_NUM_URBS; i++)
		if (s->ctx[i].urb)
			usb_kill_urb(s->ctx[i].urb);
}

static int uac8_submit_stream(struct uac8_stream *s)
{
	int i, err;

	for (i = 0; i < UAC8_NUM_URBS; i++) {
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

/* chip->mutex 保持 */
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
 * 元の audio_worker_thread() の USB 部分に相当。
 * IN/OUT の bulk URB を常時回し続ける。PCM が動いていない間は
 * OUT=無音 / IN=捨てる、という動作 (元コードと同じ挙動)。
 * chip->mutex 保持。
 */
static int uac8_start_engine(struct uac8 *chip, unsigned int rate)
{
	struct usb_device *udev = chip->udev;
	struct uac8_stream *play = &chip->stream[SNDRV_PCM_STREAM_PLAYBACK];
	struct uac8_stream *cap = &chip->stream[SNDRV_PCM_STREAM_CAPTURE];
	unsigned int pipe_out, pipe_in;
	int err;

	lockdep_assert_held(&chip->mutex);

	if (READ_ONCE(chip->disconnected))
		return -ENODEV;

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

	err = uac8_alloc_stream(cap, pipe_in, uac8_capture_complete);
	if (err)
		goto err_stop;
	err = uac8_alloc_stream(play, pipe_out, uac8_play_complete);
	if (err)
		goto err_stop;

	clear_bit(UAC8_FLAG_FAILED, &chip->flags);
	atomic_set(&cap->errors, 0);
	atomic_set(&play->errors, 0);
	chip->rate = rate;
	WRITE_ONCE(chip->stopping, false);

	err = uac8_submit_stream(cap);	/* 元コードも IN から投入 */
	if (err)
		goto err_stop;
	err = uac8_submit_stream(play);
	if (err)
		goto err_stop;

	chip->engine_running = true;
	dev_dbg(&udev->dev, "streaming started: %u Hz, alt %d\n",
		rate, uac8_rate_to_alt(rate));
	return 0;

err_stop:
	uac8_stop_engine(chip);
	return err;
}

/* 異常時に process context で PCM を XRUN にする。再 prepare でエンジン再起動 */
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
			/* substream は open/close とも mutex 下で変わるので安全 */
			if (chip->stream[i].substream)
				snd_pcm_stop_xrun(chip->stream[i].substream);
		}
	}
	mutex_unlock(&chip->mutex);
}

/* ------------------------------------------------------------------ */
/* PCM                                                                */
/* ------------------------------------------------------------------ */
static const unsigned int uac8_rates_std[] = { 96000 };
static const unsigned int uac8_rates_ext[] = {
	44100, 48000, 88200, 96000, 176400, 192000
};
static const struct snd_pcm_hw_constraint_list uac8_rate_list_std = {
	.list = uac8_rates_std,
	.count = ARRAY_SIZE(uac8_rates_std),
};
static const struct snd_pcm_hw_constraint_list uac8_rate_list_ext = {
	.list = uac8_rates_ext,
	.count = ARRAY_SIZE(uac8_rates_ext),
};

static const struct snd_pcm_hardware uac8_pcm_hw = {
	.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_BATCH | SNDRV_PCM_INFO_JOINT_DUPLEX,
	.formats = SNDRV_PCM_FMTBIT_S32_LE,
	.rates = SNDRV_PCM_RATE_KNOT,
	.rate_min = 96000,
	.rate_max = 96000,
	.channels_min = 1,
	.channels_max = UAC8_PLAY_CHANNELS,	/* open で上書き */
	.buffer_bytes_max = UAC8_BUFFER_FRAMES_MAX * UAC8_SAMPLE_BYTES *
			    UAC8_PHYS_CHANNELS,
	.period_bytes_min = UAC8_PERIOD_FRAMES_MIN * UAC8_SAMPLE_BYTES,
	.period_bytes_max = UAC8_PERIOD_FRAMES_MAX * UAC8_SAMPLE_BYTES *
			    UAC8_PHYS_CHANNELS,
	.periods_min = UAC8_PERIODS_MIN,
	.periods_max = UAC8_PERIODS_MAX,
};

static void uac8_wait_idle(struct uac8_stream *s)
{
	int i;

	/* 実行中の snd_pcm_period_elapsed() が抜けるのを待つ (最大 ~200ms) */
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
	rt->hw.period_bytes_max = UAC8_PERIOD_FRAMES_MAX * UAC8_SAMPLE_BYTES *
				  s->max_ch;
	if (ext_rates) {
		rt->hw.rate_min = 44100;
		rt->hw.rate_max = 192000;
	}

	err = snd_pcm_hw_constraint_list(rt, 0, SNDRV_PCM_HW_PARAM_RATE,
					 ext_rates ? &uac8_rate_list_ext :
						     &uac8_rate_list_std);
	if (err < 0)
		goto out;
	/* 反対方向が設定済みなら同じレートに縛る (デバイスのクロックは 1 つ) */
	if (other->hw_rate) {
		err = snd_pcm_hw_constraint_minmax(rt, SNDRV_PCM_HW_PARAM_RATE,
						   other->hw_rate,
						   other->hw_rate);
		if (err < 0)
			goto out;
	}
	/* URB が 8 フレーム単位なので period / buffer も 8 の倍数に揃える */
	err = snd_pcm_hw_constraint_minmax(rt, SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
					   UAC8_PERIOD_FRAMES_MIN,
					   UAC8_PERIOD_FRAMES_MAX);
	if (err < 0)
		goto out;
	err = snd_pcm_hw_constraint_step(rt, 0, SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
					 UAC8_URB_FRAMES);
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

	/* 異常停止後 / レート変更時はエンジンを作り直す */
	if (chip->engine_running &&
	    (chip->rate != rt->rate ||
	     test_bit(UAC8_FLAG_FAILED, &chip->flags)))
		uac8_stop_engine(chip);
	if (!chip->engine_running) {
		err = uac8_start_engine(chip, rt->rate);
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

	/* 再生は URB 128 本ぶんの先読みがある */
	rt->delay = s->playback ? UAC8_NUM_URBS * UAC8_URB_FRAMES : 0;
out:
	mutex_unlock(&chip->mutex);
	return err;
}

/* atomic context。USB には触らず、ハンドラが見るフラグを切り替えるだけ */
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
/* USB ドライバ                                                        */
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

/* 想定と違うファームウェアに誤って bind しないよう、使う Alt の EP を事前検証 */
static int uac8_check_alts(struct usb_interface *intf,
			   struct usb_interface *out_if,
			   struct usb_interface *in_if)
{
	static const int std_alts[] = { 2 };
	static const int ext_alts[] = { 1, 2, 3 };
	const int *alts = ext_rates ? ext_alts : std_alts;
	int n = ext_rates ? ARRAY_SIZE(ext_alts) : ARRAY_SIZE(std_alts);
	int i;

	for (i = 0; i < n; i++) {
		if (!uac8_find_bulk_ep(out_if, alts[i], UAC8_EP_OUT) ||
		    !uac8_find_bulk_ep(in_if, alts[i], UAC8_EP_IN)) {
			dev_err(&intf->dev,
				"alt %d: bulk EP 0x%02x/0x%02x not found, unsupported firmware?\n",
				alts[i], UAC8_EP_OUT, UAC8_EP_IN);
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
			/* 以前の bind の claim が残っているだけ */
			usb_set_intfdata(iface, UAC8_COMPANION);
			return 0;
		}
		dev_err(&intf->dev,
			"interface %d is bound to '%s'; disable snd-usb-audio for this device (see README)\n",
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

	/* AudioControl IF だけが入口。他の IF (MIDI 等) は触らない */
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

	dev_info(&intf->dev, "ZOOM UAC-8 ready: playback %dch / capture %dch, %s\n",
		 UAC8_PLAY_CHANNELS, UAC8_CAP_CHANNELS,
		 ext_rates ? "44.1-192 kHz (ext_rates)" : "96 kHz");
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
	uac8_stop_engine(chip);		/* 全 URB を kill して解放 */
	mutex_unlock(&chip->mutex);

	/* work は mutex を取るので、mutex を持たずに待つ */
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

/* 再開は SNDRV_PCM_INFO_RESUME を出していないので、アプリが prepare し直す */
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
