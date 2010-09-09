/*===========================================================================
FILE:
   QCUSBNet.c

DESCRIPTION:
   Qualcomm USB Network device for Gobi 2000 ()

Copyright (c) 2010, Code Aurora Forum. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of Code Aurora Forum nor
      the names of its contributors may be used to endorse or promote
      products derived from this software without specific prior written
      permission.

Alternatively, provided that this notice is retained in full, this software
may be relicensed by the recipient under the terms of the GNU General Public
License version 2 ("GPL") and only version 2, in which case the provisions of
the GPL apply INSTEAD OF those given above.  If the recipient relicenses the
software under the GPL, then the identification text in the MODULE_LICENSE
macro must be changed to reflect "GPLv2" instead of "Dual BSD/GPL".  Once a
recipient changes the license terms to the GPL, subsequent recipients shall
not relicense under alternate licensing terms, including the BSD or dual
BSD/GPL terms.  In addition, the following license statement immediately
below and between the words START and END shall also then apply when this
software is relicensed under the GPL:

START

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License version 2 and only version 2 as
published by the Free Software Foundation.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.

END

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
===========================================================================*/

#include "structs.h"
#include "qmidevice.h"
#include "qmi.h"
#include "qcusbnet.h"

#define DRIVER_VERSION "1.0.110"
#define DRIVER_AUTHOR "Qualcomm Innovation Center"
#define DRIVER_DESC "QCUSBNet2k"

int debug;
static struct class *devclass;

int qc_suspend(struct usb_interface *iface, pm_message_t event)
{
	struct usbnet * usbnet;
	struct qcusbnet * dev;
	
	if (!iface)
		return -ENOMEM;
	
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,23)
	usbnet = usb_get_intfdata(iface);
#else
	usbnet = iface->dev.platform_data;
#endif

	if (!usbnet || !usbnet->net) {
		DBG("failed to get netdevice\n");
		return -ENXIO;
	}
	
	dev = (struct qcusbnet *)usbnet->data[0];
	if (!dev) {
		DBG("failed to get QMIDevice\n");
		return -ENXIO;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,33)
	if (!usbnet->udev->auto_pm)
#else
	if (!(event.event & PM_EVENT_AUTO))
#endif
	{
		DBG("device suspended to power level %d\n", 
			  event.event);
		qc_setdown(dev, DOWN_DRIVER_SUSPENDED);
	} else {
		DBG("device autosuspend\n");
	}
	  
	if (event.event & PM_EVENT_SUSPEND) {
		qc_stopread(dev);
		usbnet->udev->reset_resume = 0;
		iface->dev.power.power_state.event = event.event;
	} else {
		usbnet->udev->reset_resume = 1;
	}
	
	return usbnet_suspend(iface, event);
}

static int qc_resume(struct usb_interface * iface)
{
	struct usbnet *usbnet;
	struct qcusbnet *dev;
	int ret;
	int oldstate;
	
	if (iface == 0)
		return -ENOMEM;
	
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,23)
	usbnet = usb_get_intfdata(iface);
#else
	usbnet = iface->dev.platform_data;
#endif

	if (!usbnet || !usbnet->net) {
		DBG("failed to get netdevice\n");
		return -ENXIO;
	}
	
	dev = (struct qcusbnet *)usbnet->data[0];
	if (!dev) {
		DBG("failed to get QMIDevice\n");
		return -ENXIO;
	}

	oldstate = iface->dev.power.power_state.event;
	iface->dev.power.power_state.event = PM_EVENT_ON;
	DBG("resuming from power mode %d\n", oldstate);

	if (oldstate & PM_EVENT_SUSPEND) {
		qc_cleardown(dev, DOWN_DRIVER_SUSPENDED);
	
		ret = usbnet_resume(iface);
		if (ret) {
			DBG("usbnet_resume error %d\n", ret);
			return ret;
		}

		ret = qc_startread(dev);
		if (ret) {
			DBG("qc_startread error %d\n", ret);
			return ret;
		}

		complete(&dev->worker.work);
	} else {
		DBG("nothing to resume\n");
		return 0;
	}
	
	return ret;
}

static int qcnet_bind(struct usbnet *usbnet, struct usb_interface *iface)
{
	int numends;
	int i;
	struct usb_host_endpoint *endpoint = NULL;
	struct usb_host_endpoint *in = NULL;
	struct usb_host_endpoint *out = NULL;
	
	if (iface->num_altsetting != 1) {
		DBG("invalid num_altsetting %u\n", iface->num_altsetting);
		return -EINVAL;
	}

	if (iface->cur_altsetting->desc.bInterfaceNumber != 0) {
		DBG("invalid interface %d\n", 
			  iface->cur_altsetting->desc.bInterfaceNumber);
		return -EINVAL;
	}
	
	numends = iface->cur_altsetting->desc.bNumEndpoints;
	for (i = 0; i < numends; i++) {
		endpoint = iface->cur_altsetting->endpoint + i;
		if (!endpoint) {
			DBG("invalid endpoint %u\n", i);
			return -EINVAL;
		}
		
		if (usb_endpoint_dir_in(&endpoint->desc)
		&&  !usb_endpoint_xfer_int(&endpoint->desc)) {
			in = endpoint;
		} else if (!usb_endpoint_dir_out(&endpoint->desc)) {
			out = endpoint;
		}
	}
	
	if (!in || !out) {
		DBG("invalid endpoints\n");
		return -EINVAL;
	}

	if (usb_set_interface(usbnet->udev,
	                      iface->cur_altsetting->desc.bInterfaceNumber, 0))	{
		DBG("unable to set interface\n");
		return -EINVAL;
	}

	usbnet->in = usb_rcvbulkpipe(usbnet->udev, in->desc.bEndpointAddress & USB_ENDPOINT_NUMBER_MASK);
	usbnet->out = usb_sndbulkpipe(usbnet->udev, out->desc.bEndpointAddress & USB_ENDPOINT_NUMBER_MASK);

	DBG("in %x, out %x\n",
	    in->desc.bEndpointAddress, 
	    out->desc.bEndpointAddress);

#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,23)
	iface->dev.platform_data = usbnet;
#endif
	return 0;
}

static void qcnet_unbind(struct usbnet *usbnet, struct usb_interface *iface)
{
	struct qcusbnet *dev = (struct qcusbnet *)usbnet->data[0];

	netif_carrier_off(usbnet->net);
	qc_deregister(dev);
	
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,29)
	kfree(usbnet->net->netdev_ops);
	usbnet->net->netdev_ops = NULL;
#endif

#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,23)
	iface->dev.platform_data = NULL;
#endif

	kfree(dev);
}

static void qcnet_urbhook(struct urb * urb)
{
	unsigned long flags;
	struct worker * worker = urb->context;
	if (!worker) {
		DBG("bad context\n");
		return;
	}

	if (urb->status) {
		DBG("urb finished with error %d\n", urb->status);
	}

	spin_lock_irqsave(&worker->active_lock, flags);
	worker->active = ERR_PTR(-EAGAIN);
	spin_unlock_irqrestore(&worker->active_lock, flags);
	/* XXX-fix race against qcnet_stop()? */
	complete(&worker->work);
	usb_free_urb(urb);
}

static void qcnet_txtimeout(struct net_device *netdev)
{
	struct list_head *node, *tmp;
	struct qcusbnet *dev;
	struct worker *worker;
	struct urbreq *req;
	unsigned long activeflags, listflags;
	struct usbnet *usbnet = netdev_priv(netdev);

	if (!usbnet || !usbnet->net) {
		DBG("failed to get usbnet device\n");
		return;
	}
	
	dev = (struct qcusbnet *)usbnet->data[0];
	if (!dev) {
		DBG("failed to get QMIDevice\n");
		return;
	}
	worker = &dev->worker;

	DBG("\n");

	spin_lock_irqsave(&worker->active_lock, activeflags);
	if (worker->active)
		usb_kill_urb(worker->active);
	spin_unlock_irqrestore(&worker->active_lock, activeflags);

	spin_lock_irqsave(&worker->urbs_lock, listflags);
	list_for_each_safe(node, tmp, &worker->urbs) {
		req = list_entry(node, struct urbreq, node);
		usb_free_urb(req->urb);
		list_del(&req->node);
		kfree(req);
	}
	spin_unlock_irqrestore(&worker->urbs_lock, listflags);

	complete(&worker->work);
}

static int qcnet_worker(void *arg)
{
	struct list_head *node, *tmp;
	unsigned long activeflags, listflags;
	struct urbreq *req;
	int status;
	struct usb_device *usbdev;
	struct worker *worker = arg;
	if (!worker) {
		DBG("passed null pointer\n");
		return -EINVAL;
	}
	
	usbdev = interface_to_usbdev(worker->iface);

	DBG("traffic thread started\n");

	while (!kthread_should_stop()) {
		wait_for_completion_interruptible(&worker->work);

		if (kthread_should_stop()) {
			spin_lock_irqsave(&worker->active_lock, activeflags);
			if (worker->active) {
				usb_kill_urb(worker->active);
			}
			spin_unlock_irqrestore(&worker->active_lock, activeflags);

			spin_lock_irqsave(&worker->urbs_lock, listflags);
			list_for_each_safe(node, tmp, &worker->urbs) {
				req = list_entry(node, struct urbreq, node);
				usb_free_urb(req->urb);
				list_del(&req->node);
				kfree(req);
			}
			spin_unlock_irqrestore(&worker->urbs_lock, listflags);

			break;
		}
		
		spin_lock_irqsave(&worker->active_lock, activeflags);
		if (IS_ERR(worker->active) && PTR_ERR(worker->active) == -EAGAIN) {
			worker->active = NULL;
			spin_unlock_irqrestore(&worker->active_lock, activeflags);
			usb_autopm_put_interface(worker->iface);
			spin_lock_irqsave(&worker->active_lock, activeflags);
		}

		if (worker->active) {
			spin_unlock_irqrestore(&worker->active_lock, activeflags);
			continue;
		}
		
		spin_lock_irqsave(&worker->urbs_lock, listflags);
		if (list_empty(&worker->urbs)) {
			spin_unlock_irqrestore(&worker->urbs_lock, listflags);
			spin_unlock_irqrestore(&worker->active_lock, activeflags);
			continue;
		}

		req = list_first_entry(&worker->urbs, struct urbreq, node);
		list_del(&req->node);
		spin_unlock_irqrestore(&worker->urbs_lock, listflags);

		worker->active = req->urb;
		spin_unlock_irqrestore(&worker->active_lock, activeflags);

		status = usb_autopm_get_interface(worker->iface);
		if (status < 0) {
			DBG("unable to autoresume interface: %d\n", status);
			if (status == -EPERM)
			{
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,33)
				usbdev->auto_pm = 0;
#endif
				qc_suspend(worker->iface, PMSG_SUSPEND);
			}

			spin_lock_irqsave(&worker->urbs_lock, listflags);
			list_add(&req->node, &worker->urbs);
			spin_unlock_irqrestore(&worker->urbs_lock, listflags);
			
			spin_lock_irqsave(&worker->active_lock, activeflags);
			worker->active = NULL;
			spin_unlock_irqrestore(&worker->active_lock, activeflags);
			
			continue;
		}

		status = usb_submit_urb(worker->active, GFP_KERNEL);
		if (status < 0) {
			DBG("Failed to submit URB: %d.  Packet dropped\n", status);
			spin_lock_irqsave(&worker->active_lock, activeflags);
			usb_free_urb(worker->active);
			worker->active = NULL;
			spin_unlock_irqrestore(&worker->active_lock, activeflags);
			usb_autopm_put_interface(worker->iface);
			complete(&worker->work);
		}
		
		kfree(req);
	}	
	
	DBG("traffic thread exiting\n");
	worker->thread = NULL;
	return 0;
}

static int qcnet_startxmit(struct sk_buff *skb, struct net_device *netdev)
{
	unsigned long listflags;
	struct qcusbnet *dev;
	struct worker *worker;
	struct urbreq *req;
	void *data;
	struct usbnet *usbnet = netdev_priv(netdev);
	
	DBG("\n");
	
	if (!usbnet || !usbnet->net) {
		DBG("failed to get usbnet device\n");
		return NETDEV_TX_BUSY;
	}
	
	dev = (struct qcusbnet *)usbnet->data[0];
	if (!dev) {
		DBG("failed to get QMIDevice\n");
		return NETDEV_TX_BUSY;
	}
	worker = &dev->worker;
	
	if (qc_isdown(dev, DOWN_DRIVER_SUSPENDED)) {
		DBG("device is suspended\n");
		dump_stack();
		return NETDEV_TX_BUSY;
	}
	
	req = kmalloc(sizeof(*req), GFP_ATOMIC);
	if (!req) {
		DBG("unable to allocate URBList memory\n");
		return NETDEV_TX_BUSY;
	}

	req->urb = usb_alloc_urb(0, GFP_ATOMIC);

	if (!req->urb) {
		kfree(req);
		DBG("unable to allocate URB\n");
		return NETDEV_TX_BUSY;
	}

	data = kmalloc(skb->len, GFP_ATOMIC);
	if (!data) {
		usb_free_urb(req->urb);
		kfree(req);
		DBG("unable to allocate URB data\n");
		return NETDEV_TX_BUSY;
	}
	memcpy(data, skb->data, skb->len);

	usb_fill_bulk_urb(req->urb, dev->usbnet->udev, dev->usbnet->out,
	                  data, skb->len, qcnet_urbhook, worker);
	
	spin_lock_irqsave(&worker->urbs_lock, listflags);
	list_add_tail(&req->node, &worker->urbs);
	spin_unlock_irqrestore(&worker->urbs_lock, listflags);

	complete(&worker->work);

	netdev->trans_start = jiffies;
	dev_kfree_skb_any(skb);

	return NETDEV_TX_OK;
}

static int qcnet_open(struct net_device *netdev)
{
	int status = 0;
	struct qcusbnet *dev;
	struct usbnet *usbnet = netdev_priv(netdev);
	
	if (!usbnet) {
		DBG("failed to get usbnet device\n");
		return -ENXIO;
	}
	
	dev = (struct qcusbnet *)usbnet->data[0];
	if (!dev) {
		DBG("failed to get QMIDevice\n");
		return -ENXIO;
	}

	DBG("\n");

	dev->worker.iface = dev->iface;
	INIT_LIST_HEAD(&dev->worker.urbs);
	dev->worker.active = NULL;
	spin_lock_init(&dev->worker.urbs_lock);
	spin_lock_init(&dev->worker.active_lock);
	init_completion(&dev->worker.work);
	
	dev->worker.thread = kthread_run(qcnet_worker, &dev->worker, "qcnet_worker");
	if (IS_ERR(dev->worker.thread))
	{
		DBG("AutoPM thread creation error\n");
		return PTR_ERR(dev->worker.thread);
	}

	qc_cleardown(dev, DOWN_NET_IFACE_STOPPED);
	if (dev->open)
	{
		status = dev->open(netdev);
		if (status == 0)
		{
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,33)
			usb_autopm_enable(dev->iface);
#else
			usb_autopm_put_interface(dev->iface);
#endif
		}
	} else {
		DBG("no USBNetOpen defined\n");
	}
	
	return status;
}

int qcnet_stop(struct net_device *netdev)
{
	struct qcusbnet *dev;
	struct usbnet *usbnet = netdev_priv(netdev);

	if (!usbnet || !usbnet->net) {
		DBG("failed to get netdevice\n");
		return -ENXIO;
	}
	
	dev = (struct qcusbnet *)usbnet->data[0];
	if (!dev) {
		DBG("failed to get QMIDevice\n");
		return -ENXIO;
	}

	qc_setdown(dev, DOWN_NET_IFACE_STOPPED);
	complete(&dev->worker.work);
	kthread_stop(dev->worker.thread);
	DBG("thread stopped\n");

	if (dev->stop != NULL)
		return dev->stop(netdev);
	return 0;
}

static const struct driver_info qc_netinfo = 
{
   .description   = "QCUSBNet Ethernet Device",
   .flags         = FLAG_ETHER,
   .bind          = qcnet_bind,
   .unbind        = qcnet_unbind,
   .data          = 0,
};

#define MKVIDPID(v,p) \
	{ \
		USB_DEVICE(v,p), \
		.driver_info = (unsigned long)&qc_netinfo, \
	}

static const struct usb_device_id qc_vidpids [] =
{
	MKVIDPID(0x05c6, 0x9215),	/* Acer Gobi 2000 */
	MKVIDPID(0x05c6, 0x9265),	/* Asus Gobi 2000 */
	MKVIDPID(0x16d8, 0x8002),	/* CMOTech Gobi 2000 */
	MKVIDPID(0x413c, 0x8186),	/* Dell Gobi 2000 */
	MKVIDPID(0x1410, 0xa010),	/* Entourage Gobi 2000 */
	MKVIDPID(0x1410, 0xa011),	/* Entourage Gobi 2000 */
	MKVIDPID(0x1410, 0xa012),	/* Entourage Gobi 2000 */
	MKVIDPID(0x1410, 0xa013),	/* Entourage Gobi 2000 */
	MKVIDPID(0x03f0, 0x251d),	/* HP Gobi 2000 */
	MKVIDPID(0x05c6, 0x9205),	/* Lenovo Gobi 2000 */
	MKVIDPID(0x05c6, 0x920b),	/* Generic Gobi 2000 */
	MKVIDPID(0x04da, 0x250f),	/* Panasonic Gobi 2000 */
	MKVIDPID(0x05c6, 0x9245),	/* Samsung Gobi 2000 */
	MKVIDPID(0x1199, 0x9001),	/* Sierra Wireless Gobi 2000 */
	MKVIDPID(0x1199, 0x9002),	/* Sierra Wireless Gobi 2000 */
	MKVIDPID(0x1199, 0x9003),	/* Sierra Wireless Gobi 2000 */
	MKVIDPID(0x1199, 0x9004),	/* Sierra Wireless Gobi 2000 */ 
	MKVIDPID(0x1199, 0x9005),	/* Sierra Wireless Gobi 2000 */
	MKVIDPID(0x1199, 0x9006),	/* Sierra Wireless Gobi 2000 */
	MKVIDPID(0x1199, 0x9007),	/* Sierra Wireless Gobi 2000 */
	MKVIDPID(0x1199, 0x9008),	/* Sierra Wireless Gobi 2000 */
	MKVIDPID(0x1199, 0x9009),	/* Sierra Wireless Gobi 2000 */
	MKVIDPID(0x1199, 0x900a),	/* Sierra Wireless Gobi 2000 */
	MKVIDPID(0x05c6, 0x9225),	/* Sony Gobi 2000 */
	MKVIDPID(0x05c6, 0x9235),	/* Top Global Gobi 2000 */
	MKVIDPID(0x05c6, 0x9275),	/* iRex Technologies Gobi 2000 */
	{ }
};

MODULE_DEVICE_TABLE(usb, qc_vidpids);

int qcnet_probe(struct usb_interface *iface, const struct usb_device_id *vidpids)
{
	int status;
	struct usbnet *usbnet;
	struct qcusbnet *dev;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,29)
	struct net_device_ops *netdevops;
#endif

	status = usbnet_probe(iface, vidpids);
	if (status < 0) {
		DBG("usbnet_probe failed %d\n", status);
		return status;
	}

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,23)
	usbnet = usb_get_intfdata(iface);
#else
	usbnet = iface->dev.platform_data;
#endif

	if (!usbnet || !usbnet->net) {
		DBG("failed to get netdevice\n");
		return -ENXIO;
	}

	dev = kmalloc(sizeof(struct qcusbnet), GFP_KERNEL);
	if (!dev) {
		DBG("falied to allocate device buffers");
		return -ENOMEM;
	}
	
	usbnet->data[0] = (unsigned long)dev;
	
	dev->usbnet = usbnet;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,29)
	dev->open = usbnet->net->open;
	usbnet->net->open = qcnet_open;
	dev->stop = usbnet->net->stop;
	usbnet->net->stop = qcnet_stop;
	usbnet->net->hard_start_xmit = qcnet_startxmit;
	usbnet->net->tx_timeout = qcnet_txtimeout;
#else
	netdevops = kmalloc(sizeof(struct net_device_ops), GFP_KERNEL);
	if (!netdevops) {
		DBG("falied to allocate net device ops");
		return -ENOMEM;
	}
	memcpy(netdevops, usbnet->net->netdev_ops, sizeof(struct net_device_ops));
	
	dev->open = netdevops->ndo_open;
	netdevops->ndo_open = qcnet_open;
	dev->stop = netdevops->ndo_stop;
	netdevops->ndo_stop = qcnet_stop;
	netdevops->ndo_start_xmit = qcnet_startxmit;
	netdevops->ndo_tx_timeout = qcnet_txtimeout;

	usbnet->net->netdev_ops = netdevops;
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,31)
	memset(&(dev->usbnet->stats), 0, sizeof(struct net_device_stats));
#else
	memset(&(dev->usbnet->net->stats), 0, sizeof(struct net_device_stats));
#endif

	dev->iface = iface;
	memset(&(dev->meid), '0', 14);
	
	DBG("Mac Address:\n");
	printhex(&dev->usbnet->net->dev_addr[0], 6);

	dev->valid = false;
	memset(&dev->qmi, 0, sizeof(struct qmidev));

	dev->qmi.devclass = devclass;
	
	INIT_LIST_HEAD(&dev->qmi.clients);
	init_completion(&dev->worker.work);
	spin_lock_init(&dev->qmi.clients_lock);

	dev->down = 0;
	qc_setdown(dev, DOWN_NO_NDIS_CONNECTION);
	qc_setdown(dev, DOWN_NET_IFACE_STOPPED);

	status = qc_register(dev);
	if (status) {
		qc_deregister(dev);
	}
	
	return status;
}

EXPORT_SYMBOL_GPL(qcnet_probe);

static struct usb_driver qcusbnet =
{
	.name       = "QCUSBNet2k",
	.id_table   = qc_vidpids,
	.probe      = qcnet_probe,
	.disconnect = usbnet_disconnect,
	.suspend    = qc_suspend,
	.resume     = qc_resume,
	.supports_autosuspend = true,
};

static int __init modinit(void)
{
	devclass = class_create(THIS_MODULE, "QCQMI");
	if (IS_ERR(devclass)) {
		DBG("error at class_create %ld\n", PTR_ERR(devclass));
		return -ENOMEM;
	}
	printk(KERN_INFO "%s: %s\n", DRIVER_DESC, DRIVER_VERSION);
	return usb_register(&qcusbnet);
}
module_init(modinit);

static void __exit modexit(void)
{
	usb_deregister(&qcusbnet);
	class_destroy(devclass);
}
module_exit(modexit);

MODULE_VERSION(DRIVER_VERSION);
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("Dual BSD/GPL");

module_param(debug, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(debug, "Debuging enabled or not");
