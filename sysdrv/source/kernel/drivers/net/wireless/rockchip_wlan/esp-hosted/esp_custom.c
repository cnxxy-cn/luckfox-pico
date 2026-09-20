// SPDX-License-Identifier: GPL-2.0-only
/*
 * Espressif Systems Wireless LAN device driver
 *
 * Custom data interface (ESP_CUSTOM_IF) - Linux host side.
 *
 * See include/esp_custom.h for the user space contract.
 */
#include "utils.h"
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/skbuff.h>
#include <linux/poll.h>
#include <linux/uaccess.h>
#include <linux/slab.h>

#include "esp.h"
#include "esp_kernel_port.h"
#include "esp_if.h"
#include "esp_api.h"
#include "esp_custom.h"

/* Frames received from the ESP and not yet read() by user space. */
static struct sk_buff_head esp_custom_rx_q;
static wait_queue_head_t esp_custom_rx_waitq;

/* Non-zero while /dev/esp_custom is open. When nobody is listening we drop
 * inbound frames instead of queueing them forever. */
static atomic_t esp_custom_opened = ATOMIC_INIT(0);

/* Cleared by esp_custom_deinit() so a blocked read() can bail out. */
static bool esp_custom_up;

/*
 * SDIO TX backpressure lives in sdio/esp_sdio.c write_packet():
 *
 *	if (cb && cb->priv && (atomic_read(&tx_pending) >= TX_MAX_PENDING_COUNT))
 *		... drop, return -EBUSY
 *
 * i.e. it only kicks in when the skb carries a non-NULL cb->priv, and it then
 * calls esp_tx_pause(cb->priv). This stub exists purely to arm that check on
 * frames that have no natural owning netdev - its ndev is NULL, which
 * esp_tx_pause()/esp_tx_resume() already tolerate.
 */
static struct esp_wifi_device esp_custom_stub_priv;

static int esp_custom_open(struct inode *inode, struct file *filp)
{
	atomic_inc(&esp_custom_opened);
	return 0;
}

static int esp_custom_release(struct inode *inode, struct file *filp)
{
	if (atomic_dec_and_test(&esp_custom_opened))
		skb_queue_purge(&esp_custom_rx_q);

	return 0;
}

static ssize_t esp_custom_read(struct file *filp, char __user *ubuf,
			       size_t count, loff_t *ppos)
{
	struct sk_buff *skb = NULL;
	int ret = 0;

	if (!esp_custom_up)
		return -ENODEV;

	if (skb_queue_empty(&esp_custom_rx_q)) {
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;

		ret = wait_event_interruptible(esp_custom_rx_waitq,
					       !skb_queue_empty(&esp_custom_rx_q) ||
					       !esp_custom_up);
		if (ret)
			return ret;
		if (!esp_custom_up)
			return -ENODEV;
	}

	skb = skb_dequeue(&esp_custom_rx_q);
	if (!skb)
		return -EAGAIN;

	/*
	 * Frame-oriented: a short user buffer drops the whole frame rather than
	 * splitting it, so a reader can never see a half frame.
	 */
	if (count < skb->len) {
		dev_kfree_skb_any(skb);
		return -EMSGSIZE;
	}

	if (copy_to_user(ubuf, skb->data, skb->len)) {
		dev_kfree_skb_any(skb);
		return -EFAULT;
	}

	ret = skb->len;
	dev_kfree_skb_any(skb);

	return ret;
}

static ssize_t esp_custom_write(struct file *filp, const char __user *ubuf,
				size_t count, loff_t *ppos)
{
	struct esp_adapter *adapter = esp_get_adapter();
	struct esp_payload_header *hdr = NULL;
	struct esp_skb_cb *cb = NULL;
	struct sk_buff *skb = NULL;
	u16 pad = 0, len = 0, total = 0;
	int ret = 0;

	if (!esp_custom_up)
		return -ENODEV;

	if (!adapter || !adapter->if_ops || !adapter->if_ops->write)
		return -ENODEV;

	if (count == 0)
		return 0;

	if (count > ESP_CUSTOM_MAX_FRAME)
		return -EMSGSIZE;

	len = count;

	/*
	 * Same header/padding layout process_tx_packet() uses for WiFi frames:
	 * pad = 12 (header) plus 0..3 bytes so the total is 4-byte aligned.
	 * The resulting offset therefore always lands in [12, 16], which is what
	 * ESP_OFFSET_VALID() and the ESP firmware's process_rx_pkt() require.
	 */
	pad = sizeof(struct esp_payload_header);
	total = len + pad;
	pad += (SKB_DATA_ADDR_ALIGNMENT - (total % SKB_DATA_ADDR_ALIGNMENT)) %
		SKB_DATA_ADDR_ALIGNMENT;
	total = len + pad;

	skb = esp_if_alloc_skb(adapter, total);
	if (!skb)
		return -ENOMEM;

	skb_put(skb, total);
	memset(skb->data, 0, pad);

	hdr = (struct esp_payload_header *)skb->data;
	hdr->if_type = ESP_CUSTOM_IF;
	hdr->if_num = 0;
	hdr->len = esp_wire_cpu_to_le16(len);
	hdr->offset = esp_wire_cpu_to_le16(pad);
	/*
	 * Must be PACKET_TYPE_DATA: the ESP firmware dispatches on packet_type
	 * first, and anything else is routed to the command/event parser.
	 */
	hdr->packet_type = PACKET_TYPE_DATA;

	if (copy_from_user(skb->data + pad, ubuf, len)) {
		dev_kfree_skb_any(skb);
		return -EFAULT;
	}

	if (adapter->capabilities & ESP_CHECKSUM_ENABLED)
		hdr->checksum = esp_wire_cpu_to_le16(
			compute_checksum(skb->data, len + pad));

	cb = (struct esp_skb_cb *)skb->cb;
	cb->priv = adapter->priv[0] ? adapter->priv[0] : &esp_custom_stub_priv;

	ret = esp_send_packet(adapter, skb);
	if (ret) {
		/*
		 * write_packet() already freed the skb on every error path it
		 * can produce (-EBUSY / -EPERM / -ENODEV), so do NOT free it
		 * again here. -EINVAL is unreachable because of the if_ops
		 * check above.
		 */
		if (ret == -EBUSY)
			return -EAGAIN;

		return ret;
	}

	return len;
}

static __poll_t esp_custom_poll(struct file *filp,
				struct poll_table_struct *wait)
{
	__poll_t mask = 0;

	poll_wait(filp, &esp_custom_rx_waitq, wait);

	if (!skb_queue_empty(&esp_custom_rx_q))
		mask |= EPOLLIN | EPOLLRDNORM;

	/*
	 * Always writable from select()'s point of view: the TX side never
	 * blocks, it returns -EAGAIN when the SDIO queue is full.
	 */
	mask |= EPOLLOUT | EPOLLWRNORM;

	return mask;
}

static const struct file_operations esp_custom_fops = {
	.owner   = THIS_MODULE,
	.open    = esp_custom_open,
	.release = esp_custom_release,
	.read    = esp_custom_read,
	.write   = esp_custom_write,
	.poll    = esp_custom_poll,
	.llseek  = no_llseek,
};

static struct miscdevice esp_custom_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = ESP_CUSTOM_DEV_NAME,
	.fops  = &esp_custom_fops,
	.mode  = 0666,
};

int esp_custom_init(void)
{
	int ret = 0;

	skb_queue_head_init(&esp_custom_rx_q);
	init_waitqueue_head(&esp_custom_rx_waitq);
	atomic_set(&esp_custom_opened, 0);
	memset(&esp_custom_stub_priv, 0, sizeof(esp_custom_stub_priv));
	esp_custom_up = true;

	ret = misc_register(&esp_custom_miscdev);
	if (ret) {
		esp_custom_up = false;
		esp_err("Failed to register /dev/%s: %d\n",
			ESP_CUSTOM_DEV_NAME, ret);
		return ret;
	}

	esp_info("Custom interface ready at /dev/%s (max frame %d bytes)\n",
		 ESP_CUSTOM_DEV_NAME, ESP_CUSTOM_MAX_FRAME);

	return 0;
}

void esp_custom_deinit(void)
{
	/*
	 * Caller must guarantee no producer is left: main.c calls this after
	 * deinit_adapter() has destroyed (and therefore drained) the RX
	 * workqueue, so esp_custom_rx() cannot race with the teardown below.
	 */
	misc_deregister(&esp_custom_miscdev);

	esp_custom_up = false;
	wake_up_interruptible(&esp_custom_rx_waitq);
	skb_queue_purge(&esp_custom_rx_q);
}

void esp_custom_rx(struct sk_buff *skb)
{
	if (!skb)
		return;

	/*
	 * Drop rather than queue when nobody is reading: this is a debug/control
	 * style channel where stale frames are worse than lost ones, and it
	 * bounds memory if user space opens the device and stops reading.
	 */
	if (!atomic_read(&esp_custom_opened) ||
	    skb_queue_len(&esp_custom_rx_q) >= ESP_CUSTOM_QUEUE_DEPTH) {
		dev_kfree_skb_any(skb);
		return;
	}

	skb_queue_tail(&esp_custom_rx_q, skb);
	wake_up_interruptible(&esp_custom_rx_waitq);
}

