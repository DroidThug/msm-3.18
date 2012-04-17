/*
 * Copyright (c) 2006 Dave Airlie <airlied@linux.ie>
 * Copyright © 2006-2008,2010 Intel Corporation
 *   Jesse Barnes <jesse.barnes@intel.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *	Eric Anholt <eric@anholt.net>
 *	Chris Wilson <chris@chris-wilson.co.uk>
 */
#include <linux/i2c.h>
#include <linux/i2c-algo-bit.h>
#include <linux/export.h>
#include "drmP.h"
#include "drm.h"
#include "intel_drv.h"
#include "i915_drm.h"
#include "i915_drv.h"

struct gmbus_port {
	const char *name;
	int reg;
};

static const struct gmbus_port gmbus_ports[] = {
	{ "ssc", GPIOB },
	{ "vga", GPIOA },
	{ "panel", GPIOC },
	{ "dpc", GPIOD },
	{ "dpb", GPIOE },
	{ "dpd", GPIOF },
};

/* Intel GPIO access functions */

#define I2C_RISEFALL_TIME 20

#define BITBANG_FALLBACK_TIMEOUT 50
#define EXTENDED_FALLBACK_TIMEOUT 300

static inline struct intel_gmbus *
to_intel_gmbus(struct i2c_adapter *i2c)
{
	return container_of(i2c, struct intel_gmbus, adapter);
}

struct intel_gpio {
	struct i2c_adapter adapter;
	struct i2c_algo_bit_data algo;
	struct drm_i915_private *dev_priv;
	u32 reg;
};

void
intel_i2c_reset(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	if (HAS_PCH_SPLIT(dev))
		I915_WRITE(PCH_GMBUS0, 0);
	else
		I915_WRITE(GMBUS0, 0);
}

static void intel_i2c_quirk_set(struct drm_i915_private *dev_priv, bool enable)
{
	u32 val;

	/* When using bit bashing for I2C, this bit needs to be set to 1 */
	if (!IS_PINEVIEW(dev_priv->dev))
		return;

	val = I915_READ(DSPCLK_GATE_D);
	if (enable)
		val |= DPCUNIT_CLOCK_GATE_DISABLE;
	else
		val &= ~DPCUNIT_CLOCK_GATE_DISABLE;
	I915_WRITE(DSPCLK_GATE_D, val);
}

static u32 get_reserved(struct intel_gpio *gpio)
{
	struct drm_i915_private *dev_priv = gpio->dev_priv;
	struct drm_device *dev = dev_priv->dev;
	u32 reserved = 0;

	/* On most chips, these bits must be preserved in software. */
	if (!IS_I830(dev) && !IS_845G(dev))
		reserved = I915_READ_NOTRACE(gpio->reg) &
					     (GPIO_DATA_PULLUP_DISABLE |
					      GPIO_CLOCK_PULLUP_DISABLE);

	return reserved;
}

static int get_clock(void *data)
{
	struct intel_gpio *gpio = data;
	struct drm_i915_private *dev_priv = gpio->dev_priv;
	u32 reserved = get_reserved(gpio);
	I915_WRITE_NOTRACE(gpio->reg, reserved | GPIO_CLOCK_DIR_MASK);
	I915_WRITE_NOTRACE(gpio->reg, reserved);
	return (I915_READ_NOTRACE(gpio->reg) & GPIO_CLOCK_VAL_IN) != 0;
}

static int get_data(void *data)
{
	struct intel_gpio *gpio = data;
	struct drm_i915_private *dev_priv = gpio->dev_priv;
	u32 reserved = get_reserved(gpio);
	I915_WRITE_NOTRACE(gpio->reg, reserved | GPIO_DATA_DIR_MASK);
	I915_WRITE_NOTRACE(gpio->reg, reserved);
	return (I915_READ_NOTRACE(gpio->reg) & GPIO_DATA_VAL_IN) != 0;
}

static void set_clock(void *data, int state_high)
{
	struct intel_gpio *gpio = data;
	struct drm_i915_private *dev_priv = gpio->dev_priv;
	u32 reserved = get_reserved(gpio);
	u32 clock_bits;

	if (state_high)
		clock_bits = GPIO_CLOCK_DIR_IN | GPIO_CLOCK_DIR_MASK;
	else
		clock_bits = GPIO_CLOCK_DIR_OUT | GPIO_CLOCK_DIR_MASK |
			GPIO_CLOCK_VAL_MASK;

	I915_WRITE_NOTRACE(gpio->reg, reserved | clock_bits);
	POSTING_READ(gpio->reg);
}

static void set_data(void *data, int state_high)
{
	struct intel_gpio *gpio = data;
	struct drm_i915_private *dev_priv = gpio->dev_priv;
	u32 reserved = get_reserved(gpio);
	u32 data_bits;

	if (state_high)
		data_bits = GPIO_DATA_DIR_IN | GPIO_DATA_DIR_MASK;
	else
		data_bits = GPIO_DATA_DIR_OUT | GPIO_DATA_DIR_MASK |
			GPIO_DATA_VAL_MASK;

	I915_WRITE_NOTRACE(gpio->reg, reserved | data_bits);
	POSTING_READ(gpio->reg);
}

static struct i2c_adapter *
intel_gpio_create(struct drm_i915_private *dev_priv, u32 pin)
{
	struct intel_gpio *gpio;

	BUG_ON(!intel_gmbus_is_port_valid(pin));

	gpio = kzalloc(sizeof(struct intel_gpio), GFP_KERNEL);
	if (gpio == NULL)
		return NULL;

	/* NB: -1 to map pin pair to gmbus array index */
	gpio->reg = gmbus_ports[pin - 1].reg;
	if (HAS_PCH_SPLIT(dev_priv->dev))
		gpio->reg += PCH_GPIOA - GPIOA;
	gpio->dev_priv = dev_priv;

	snprintf(gpio->adapter.name, sizeof(gpio->adapter.name),
		 "i915 GPIO%c", "BACDEF"[pin]);
	gpio->adapter.owner = THIS_MODULE;
	gpio->adapter.algo_data	= &gpio->algo;
	gpio->adapter.dev.parent = &dev_priv->dev->pdev->dev;
	gpio->algo.setsda = set_data;
	gpio->algo.setscl = set_clock;
	gpio->algo.getsda = get_data;
	gpio->algo.getscl = get_clock;
	gpio->algo.udelay = I2C_RISEFALL_TIME;
	gpio->algo.timeout = usecs_to_jiffies(2200);
	gpio->algo.data = gpio;

	if (i2c_bit_add_bus(&gpio->adapter))
		goto out_free;

	return &gpio->adapter;

out_free:
	kfree(gpio);
	return NULL;
}

static int
intel_i2c_quirk_xfer(struct drm_i915_private *dev_priv,
		     struct i2c_adapter *adapter,
		     struct i2c_msg *msgs,
		     int num)
{
	struct intel_gpio *gpio = container_of(adapter,
					       struct intel_gpio,
					       adapter);
	int ret;

	mutex_lock(&dev_priv->gmbus_mutex);

	intel_i2c_reset(dev_priv->dev);

	intel_i2c_quirk_set(dev_priv, true);
	set_data(gpio, 1);
	set_clock(gpio, 1);
	udelay(I2C_RISEFALL_TIME);

	ret = adapter->algo->master_xfer(adapter, msgs, num);

	set_data(gpio, 1);
	set_clock(gpio, 1);
	intel_i2c_quirk_set(dev_priv, false);

	mutex_unlock(&dev_priv->gmbus_mutex);

	return ret;
}

static int
gmbus_xfer(struct i2c_adapter *adapter,
	   struct i2c_msg *msgs,
	   int num)
{
	struct intel_gmbus *bus = container_of(adapter,
					       struct intel_gmbus,
					       adapter);
	struct drm_i915_private *dev_priv = adapter->algo_data;
	int i, reg_offset;
	int ret = 0;
	int bitbang_fallback_timeout_ms = BITBANG_FALLBACK_TIMEOUT;
	u32 gmbus2 = 0;
	u32 gmbus0;

	if (bus->force_bit)
		return intel_i2c_quirk_xfer(dev_priv,
					    bus->force_bit, msgs, num);

	mutex_lock(&dev_priv->gmbus_mutex);

	reg_offset = HAS_PCH_SPLIT(dev_priv->dev) ? PCH_GMBUS0 - GMBUS0 : 0;

	/* Hack to use 400kHz only for atmel_mxt i2c devices on ddc ports */
	gmbus0 = bus->reg0;
	if (((gmbus0 & GMBUS_PORT_MASK) == GMBUS_PORT_VGADDC &&
	     msgs[0].addr == 0x4b) ||
	    ((gmbus0 & GMBUS_PORT_MASK) == GMBUS_PORT_PANEL &&
	     msgs[0].addr == 0x4a))
		gmbus0 = (gmbus0 & ~GMBUS_RATE_MASK) | GMBUS_RATE_400KHZ;
	I915_WRITE(GMBUS0 + reg_offset, gmbus0);

	/*
	 * Hack to increase bitbang fallout timeout for atmel_mxt devices
	 * in Bootloader mode. Observed a 220ms delay on a byte read when
	 * updating atmel_mxt firmware.
	 */
	if ((gmbus0 & GMBUS_PORT_MASK) == GMBUS_PORT_PANEL &&
	     msgs[0].addr == 0x26)
		bitbang_fallback_timeout_ms = EXTENDED_FALLBACK_TIMEOUT;

	for (i = 0; i < num; i++) {
		u16 len;
		u8 *buf;
		u32 gmbus5 = 0;
		u32 gmbus1_index = 0;

		/*
		 * The gmbus controller can combine a 1 or 2 byte write with a
		 * read that immediately follows it by using an "INDEX" cycle.
		 */
		if (i + 1 < num &&
		    !(msgs[i].flags & I2C_M_RD) &&
		    (msgs[i + 1].flags & I2C_M_RD) &&
		    msgs[i].len <= 2) {
			if (msgs[i].len == 2)
				gmbus5 = GMBUS_2BYTE_INDEX_EN |
					 msgs[i].buf[1] |
					 (msgs[i].buf[0] << 8);
			if (msgs[i].len == 1)
				gmbus1_index = GMBUS_CYCLE_INDEX |
					       (msgs[i].buf[0] <<
						GMBUS_SLAVE_INDEX_SHIFT);
			i += 1;  /* set i to the index of the read xfer */
		}

		len = msgs[i].len;
		buf = msgs[i].buf;

		/* GMBUS5 holds 16-bit index, but must be 0 if not used */
		I915_WRITE(GMBUS5 + reg_offset, gmbus5);

		if (msgs[i].flags & I2C_M_RD) {
			I915_WRITE(GMBUS1 + reg_offset,
				   GMBUS_CYCLE_WAIT |
				   gmbus1_index |
				   (len << GMBUS_BYTE_COUNT_SHIFT) |
				   (msgs[i].addr << GMBUS_SLAVE_ADDR_SHIFT) |
				   GMBUS_SLAVE_READ | GMBUS_SW_RDY);
			POSTING_READ(GMBUS2 + reg_offset);
			do {
				u32 val, loop = 0;

				if (wait_for((gmbus2 = I915_READ(GMBUS2 +
								 reg_offset)) &
					     (GMBUS_SATOER | GMBUS_HW_RDY),
					     bitbang_fallback_timeout_ms))
					goto timeout;
				if (gmbus2 & GMBUS_SATOER)
					goto clear_err;

				val = I915_READ(GMBUS3 + reg_offset);
				do {
					*buf++ = val & 0xff;
					val >>= 8;
				} while (--len && ++loop < 4);
			} while (len);
		} else {
			u32 val, loop;

			val = loop = 0;
			while (len && loop < 4) {
				val |= *buf++ << (8 * loop++);
				len -= 1;
			}

			I915_WRITE(GMBUS3 + reg_offset, val);
			I915_WRITE(GMBUS1 + reg_offset,
				   GMBUS_CYCLE_WAIT |
				   (msgs[i].len << GMBUS_BYTE_COUNT_SHIFT) |
				   (msgs[i].addr << GMBUS_SLAVE_ADDR_SHIFT) |
				   GMBUS_SLAVE_WRITE | GMBUS_SW_RDY);
			POSTING_READ(GMBUS2 + reg_offset);

			while (len) {
				val = loop = 0;
				do {
					val |= *buf++ << (8 * loop);
				} while (--len && ++loop < 4);

				I915_WRITE(GMBUS3 + reg_offset, val);
				POSTING_READ(GMBUS2 + reg_offset);

				if (wait_for((gmbus2 = I915_READ(GMBUS2 +
								 reg_offset)) &
					     (GMBUS_SATOER | GMBUS_HW_RDY),
					     bitbang_fallback_timeout_ms))
					goto timeout;
				if (gmbus2 & GMBUS_SATOER)
					goto clear_err;
			}
		}

		if (wait_for((gmbus2 = I915_READ(GMBUS2 + reg_offset)) &
			     (GMBUS_SATOER | GMBUS_HW_WAIT_PHASE),
			     bitbang_fallback_timeout_ms))
			goto timeout;
		if (gmbus2 & GMBUS_SATOER)
			goto clear_err;
	}

	/* Generate a STOP condition on the bus */
	I915_WRITE(GMBUS1 + reg_offset, GMBUS_CYCLE_STOP | GMBUS_SW_RDY);

	/* Mark the GMBUS interface as disabled after waiting for idle.
	 * We will re-enable it at the start of the next xfer,
	 * till then let it sleep.
	 */
	if (wait_for((I915_READ(GMBUS2 + reg_offset) & GMBUS_ACTIVE) == 0,
		     10)) {
		DRM_INFO("GMBUS [%s] timed out waiting for IDLE\n",
			 adapter->name);
		ret = -ETIMEDOUT;
	}
	I915_WRITE(GMBUS0 + reg_offset, 0);

	mutex_unlock(&dev_priv->gmbus_mutex);

	return ret ?: i;

clear_err:
	/*
	 * Wait for bus to IDLE before clearing NAK.
	 * If we clear the NAK while bus is still active, then it will stay
	 * active and the next transaction may fail.
	 */
	if (wait_for((I915_READ(GMBUS2 + reg_offset) & GMBUS_ACTIVE) == 0,
		     10))
		DRM_INFO("GMBUS [%s] timed out after NAK\n", adapter->name);

	/* Toggle the Software Clear Interrupt bit. This has the effect
	 * of resetting the GMBUS controller and so clearing the
	 * BUS_ERROR raised by the slave's NAK.
	 */
	I915_WRITE(GMBUS1 + reg_offset, GMBUS_SW_CLR_INT);
	I915_WRITE(GMBUS1 + reg_offset, 0);
	I915_WRITE(GMBUS0 + reg_offset, 0);

	DRM_DEBUG_DRIVER("GMBUS [%s] NAK for addr: %04x %c(%d)\n",
			 adapter->name, msgs[i].addr,
			 (msgs[i].flags & I2C_M_RD) ? 'r' : 'w', msgs[i].len);

	mutex_unlock(&dev_priv->gmbus_mutex);

	/*
	 * If no ACK is received during the address phase of a transaction,
	 * the adapter must report -ENXIO.
	 * It is not clear what to return if no ACK is received at other times.
	 * So, we always return -ENXIO in all NAK cases, to ensure we send
	 * it at least during the one case that is specified.
	 */
	return -ENXIO;

timeout:
	DRM_INFO("GMBUS timed out, falling back to bit banging on pin %d [%s]\n",
		 bus->reg0 & 0xff, bus->adapter.name);
	I915_WRITE(GMBUS0 + reg_offset, 0);

	mutex_unlock(&dev_priv->gmbus_mutex);

	/* Hardware may not support GMBUS over these pins? Try GPIO bitbanging instead. */
	bus->force_bit = intel_gpio_create(dev_priv, bus->reg0 & 0xff);
	if (!bus->force_bit)
		return -ENOMEM;

	return intel_i2c_quirk_xfer(dev_priv, bus->force_bit, msgs, num);
}

static u32 gmbus_func(struct i2c_adapter *adapter)
{
	struct intel_gmbus *bus = container_of(adapter,
					       struct intel_gmbus,
					       adapter);

	if (bus->force_bit)
		bus->force_bit->algo->functionality(bus->force_bit);

	return (I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL |
		/* I2C_FUNC_10BIT_ADDR | */
		I2C_FUNC_SMBUS_READ_BLOCK_DATA |
		I2C_FUNC_SMBUS_BLOCK_PROC_CALL);
}

static const struct i2c_algorithm gmbus_algorithm = {
	.master_xfer	= gmbus_xfer,
	.functionality	= gmbus_func
};

/**
 * intel_gmbus_setup - instantiate all Intel i2c GMBuses
 * @dev: DRM device
 */
int intel_setup_gmbus(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	int ret, i;

	dev_priv->gmbus = kcalloc(sizeof(struct intel_gmbus),
				  ARRAY_SIZE(gmbus_ports), GFP_KERNEL);
	if (dev_priv->gmbus == NULL)
		return -ENOMEM;

	mutex_init(&dev_priv->gmbus_mutex);

	for (i = 0; i < ARRAY_SIZE(gmbus_ports); i++) {
		struct intel_gmbus *bus = &dev_priv->gmbus[i];
		u32 port = i + 1; /* +1 to map gmbus index to pin pair */

		bus->adapter.owner = THIS_MODULE;
		bus->adapter.class = I2C_CLASS_DDC;
		snprintf(bus->adapter.name, sizeof(bus->adapter.name),
			 "i915 gmbus %s", gmbus_ports[i].name);

		bus->adapter.dev.parent = &dev->pdev->dev;
		bus->adapter.algo_data	= dev_priv;

		bus->adapter.algo = &gmbus_algorithm;
		ret = i2c_add_adapter(&bus->adapter);
		if (ret)
			goto err;

		/* By default use a conservative clock rate */
		bus->reg0 = port | GMBUS_RATE_100KHZ;
		if (port != GMBUS_PORT_VGADDC &&
		    port != GMBUS_PORT_PANEL) {
			/* XXX force bit banging until GMBUS is debugged */
			bus->force_bit = intel_gpio_create(dev_priv, port);
		}
	}

	intel_i2c_reset(dev_priv->dev);

	return 0;

err:
	while (--i) {
		struct intel_gmbus *bus = &dev_priv->gmbus[i];
		i2c_del_adapter(&bus->adapter);
	}
	kfree(dev_priv->gmbus);
	dev_priv->gmbus = NULL;
	return ret;
}

struct i2c_adapter *intel_gmbus_get_adapter(struct drm_i915_private *dev_priv,
					    unsigned port)
{
	BUG_ON(!intel_gmbus_is_port_valid(port));
	/* NB: -1 to map pin pair to gmbus array index */
	return &dev_priv->gmbus[port - 1].adapter;
}

void intel_gmbus_set_speed(struct i2c_adapter *adapter, int speed)
{
	struct intel_gmbus *bus = to_intel_gmbus(adapter);

	bus->reg0 = (bus->reg0 & ~(0x3 << 8)) | speed;
}

void intel_gmbus_force_bit(struct i2c_adapter *adapter, bool force_bit)
{
	struct intel_gmbus *bus = to_intel_gmbus(adapter);

	if (force_bit) {
		if (bus->force_bit == NULL) {
			struct drm_i915_private *dev_priv = adapter->algo_data;
			bus->force_bit = intel_gpio_create(dev_priv,
							   bus->reg0 & 0xff);
		}
	} else {
		if (bus->force_bit) {
			i2c_del_adapter(bus->force_bit);
			kfree(bus->force_bit);
			bus->force_bit = NULL;
		}
	}
}

void intel_teardown_gmbus(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	int i;

	if (dev_priv->gmbus == NULL)
		return;

	for (i = 0; i < ARRAY_SIZE(gmbus_ports); i++) {
		struct intel_gmbus *bus = &dev_priv->gmbus[i];
		if (bus->force_bit) {
			i2c_del_adapter(bus->force_bit);
			kfree(bus->force_bit);
		}
		i2c_del_adapter(&bus->adapter);
	}

	kfree(dev_priv->gmbus);
	dev_priv->gmbus = NULL;
}
