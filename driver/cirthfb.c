/*
 * cirthfb — Waveshare 2.13" e-ink framebuffer driver (EPD2IN13V4)
 *
 * Manual sysfs bind (no DT overlay required for testing):
 *
 *   Step 1 — Unbind spidev if it currently owns spi0.0:
 *     echo spi0.0 > /sys/bus/spi/drivers/spidev/unbind
 *
 *   Step 2 — Override driver selection for the device:
 *     echo cirthfb > /sys/bus/spi/devices/spi0.0/driver_override
 *
 *   Step 3 — Trigger probe:
 *     echo spi0.0 > /sys/bus/spi/drivers/cirthfb/bind
 *
 *   To unbind later:
 *     echo spi0.0 > /sys/bus/spi/drivers/cirthfb/unbind
 *     echo > /sys/bus/spi/devices/spi0.0/driver_override
 *
 * GPIO defaults (Linux GPIO = gpiochip base 512 + BCM, Pi Zero 2W / Scarthgap):
 *   dc_gpio=537 (BCM 25)  rst_gpio=529 (BCM 17)  busy_gpio=536 (BCM 24)
 * Override at load time: insmod cirthfb.ko dc_gpio=537 rst_gpio=529 busy_gpio=536
 *
 * module_spi_driver() generates module_init/module_exit, which call
 * spi_register_driver/spi_unregister_driver. No manual init/exit needed.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/spi/spi.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/fb.h>

/* Linux framebuffer geometry (landscape: 250 wide × 122 tall, 1 bpp) */
#define CIRTHFB_XRES     250
#define CIRTHFB_YRES     122
#define CIRTHFB_STRIDE   ((CIRTHFB_XRES + 7) / 8)         /* 32 bytes/row */
#define CIRTHFB_SMEM_LEN (CIRTHFB_STRIDE * CIRTHFB_YRES)  /* 3904 bytes  */

/* Linux GPIO numbers: gpiochip base (512 on Pi Zero 2W / Scarthgap) + BCM pin */
static int dc_gpio   = 537; /* BCM 25 */
static int rst_gpio  = 529; /* BCM 17 */
static int busy_gpio = 536; /* BCM 24 */

module_param(dc_gpio,   int, 0444);
module_param(rst_gpio,  int, 0444);
module_param(busy_gpio, int, 0444);
MODULE_PARM_DESC(dc_gpio,   "Linux GPIO number for DC pin (default 537 = BCM 25)");
MODULE_PARM_DESC(rst_gpio,  "Linux GPIO number for RST pin (default 529 = BCM 17)");
MODULE_PARM_DESC(busy_gpio, "Linux GPIO number for BUSY pin (default 536 = BCM 24)");

struct cirthfb_dev {
	struct spi_device  *spi;
	struct fb_info     *info;
	struct mutex        lock; /* serialises SPI transactions */
	struct gpio_desc   *dc;
	struct gpio_desc   *rst;
	struct gpio_desc   *busy;
};

/* SSD1675-family command codes used by EPD2IN13V4 */
#define EPD_CMD_DRIVER_OUTPUT    0x01
#define EPD_CMD_DATA_ENTRY       0x11
#define EPD_CMD_SW_RESET         0x12
#define EPD_CMD_DISPLAY_UPDATE1  0x21
#define EPD_CMD_WRITE_BW_RAM     0x24
#define EPD_CMD_WRITE_RED_RAM    0x26
#define EPD_CMD_BORDER_WAVEFORM  0x3C
#define EPD_CMD_TEMP_SENSOR      0x18
#define EPD_CMD_SET_RAMX_ADDR    0x44
#define EPD_CMD_SET_RAMY_ADDR    0x45
#define EPD_CMD_SET_RAMX_COUNTER 0x4E
#define EPD_CMD_SET_RAMY_COUNTER 0x4F
#define EPD_CMD_DISPLAY_UPDATE2  0x22
#define EPD_CMD_MASTER_ACTIVATE  0x20

/* ------------------------------------------------------------------ */
/* Low-level SPI helpers                                               */
/* ------------------------------------------------------------------ */

static int epd_cmd(struct cirthfb_dev *priv, u8 cmd)
{
	gpiod_set_value(priv->dc, 0); /* DC low = command */
	return spi_write(priv->spi, &cmd, 1);
}

static int epd_dat(struct cirthfb_dev *priv, u8 data)
{
	gpiod_set_value(priv->dc, 1); /* DC high = data */
	return spi_write(priv->spi, &data, 1);
}

static int epd_dat_buf(struct cirthfb_dev *priv, const u8 *buf, size_t len)
{
	gpiod_set_value(priv->dc, 1);
	return spi_write(priv->spi, buf, len);
}

/* ------------------------------------------------------------------ */
/* Hardware control                                                    */
/* ------------------------------------------------------------------ */

static void epd_hw_reset(struct cirthfb_dev *priv)
{
	gpiod_set_value(priv->rst, 1);
	msleep(10);
	gpiod_set_value(priv->rst, 0);
	msleep(2);
	gpiod_set_value(priv->rst, 1);
	msleep(10);
}

/* BUSY is HIGH while the controller is busy; poll until LOW */
static int epd_wait_busy(struct cirthfb_dev *priv)
{
	int retries = 500; /* 5 s timeout at 10 ms per poll */

	msleep(1); /* give controller time to assert BUSY after a command */
	while (gpiod_get_value(priv->busy) == 1) {
		msleep(10);
		if (--retries <= 0) {
			pr_err("cirthfb: BUSY timeout\n");
			return -ETIMEDOUT;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* EPD2IN13V4 initialisation sequence                                 */
/* ------------------------------------------------------------------ */

static int epd_init_display(struct cirthfb_dev *priv)
{
	int ret;

	epd_hw_reset(priv);

	ret = epd_wait_busy(priv);
	if (ret)
		return ret;

	/* Software reset — controller re-loads OTP defaults */
	ret = epd_cmd(priv, EPD_CMD_SW_RESET);
	if (ret)
		return ret;
	ret = epd_wait_busy(priv);
	if (ret)
		return ret;

	/* Driver output control: MUX = height − 1, normal gate order */
	ret = epd_cmd(priv, EPD_CMD_DRIVER_OUTPUT);
	if (ret) return ret;
	ret = epd_dat(priv, (CIRTHFB_XRES - 1) & 0xFF);
	if (ret) return ret;
	ret = epd_dat(priv, ((CIRTHFB_XRES - 1) >> 8) & 0x01);
	if (ret) return ret;
	ret = epd_dat(priv, 0x00);
	if (ret) return ret;

	/* Data entry mode: X-increment then Y-increment, address-counter mode */
	ret = epd_cmd(priv, EPD_CMD_DATA_ENTRY);
	if (ret) return ret;
	ret = epd_dat(priv, 0x03);
	if (ret) return ret;

	/* RAM X address window: bytes 0 … CIRTHFB_STRIDE-1 */
	ret = epd_cmd(priv, EPD_CMD_SET_RAMX_ADDR);
	if (ret) return ret;
	ret = epd_dat(priv, 0x00);
	if (ret) return ret;
	ret = epd_dat(priv, CIRTHFB_STRIDE - 1);
	if (ret) return ret;

	/* RAM Y address window: rows 0 … CIRTHFB_XRES-1 */
	ret = epd_cmd(priv, EPD_CMD_SET_RAMY_ADDR);
	if (ret) return ret;
	ret = epd_dat(priv, 0x00);
	if (ret) return ret;
	ret = epd_dat(priv, 0x00);
	if (ret) return ret;
	ret = epd_dat(priv, (CIRTHFB_XRES - 1) & 0xFF);
	if (ret) return ret;
	ret = epd_dat(priv, ((CIRTHFB_XRES - 1) >> 8) & 0x01);
	if (ret) return ret;

	/* Border waveform: VSS / VBD_GS / follow LUT */
	ret = epd_cmd(priv, EPD_CMD_BORDER_WAVEFORM);
	if (ret) return ret;
	ret = epd_dat(priv, 0x05);
	if (ret) return ret;

	/* Display update control 1: keep defaults, enable bypass clock */
	ret = epd_cmd(priv, EPD_CMD_DISPLAY_UPDATE1);
	if (ret) return ret;
	ret = epd_dat(priv, 0x00);
	if (ret) return ret;
	ret = epd_dat(priv, 0x80);
	if (ret) return ret;

	/* Use built-in temperature sensor */
	ret = epd_cmd(priv, EPD_CMD_TEMP_SENSOR);
	if (ret) return ret;
	ret = epd_dat(priv, 0x80);
	if (ret) return ret;

	/* Reset RAM address counters to (0, 0) */
	ret = epd_cmd(priv, EPD_CMD_SET_RAMX_COUNTER);
	if (ret) return ret;
	ret = epd_dat(priv, 0x00);
	if (ret) return ret;

	ret = epd_cmd(priv, EPD_CMD_SET_RAMY_COUNTER);
	if (ret) return ret;
	ret = epd_dat(priv, 0x00);
	if (ret) return ret;
	ret = epd_dat(priv, 0x00);
	if (ret) return ret;

	return epd_wait_busy(priv);
}

/* Trigger a full-panel refresh from whichever data is already in RAM */
static int epd_turn_on_display(struct cirthfb_dev *priv)
{
	int ret;

	ret = epd_cmd(priv, EPD_CMD_DISPLAY_UPDATE2);
	if (ret) return ret;
	ret = epd_dat(priv, 0xF7); /* full refresh sequence */
	if (ret) return ret;
	ret = epd_cmd(priv, EPD_CMD_MASTER_ACTIVATE);
	if (ret) return ret;
	return epd_wait_busy(priv);
}

/* Write 0xFF to both BW and RED RAM and refresh → all-white panel */
static int epd_clear_to_white(struct cirthfb_dev *priv)
{
	u8 *white;
	int ret;

	white = kmalloc(CIRTHFB_SMEM_LEN, GFP_KERNEL);
	if (!white)
		return -ENOMEM;
	memset(white, 0xFF, CIRTHFB_SMEM_LEN);

	ret = epd_cmd(priv, EPD_CMD_WRITE_BW_RAM);
	if (!ret)
		ret = epd_dat_buf(priv, white, CIRTHFB_SMEM_LEN);
	if (!ret)
		ret = epd_cmd(priv, EPD_CMD_WRITE_RED_RAM);
	if (!ret)
		ret = epd_dat_buf(priv, white, CIRTHFB_SMEM_LEN);

	kfree(white);

	if (ret)
		return ret;
	return epd_turn_on_display(priv);
}

/* ------------------------------------------------------------------ */
/* Framebuffer screen descriptors                                      */
/* ------------------------------------------------------------------ */

static const struct fb_var_screeninfo cirthfb_var = {
	.xres           = CIRTHFB_XRES,
	.yres           = CIRTHFB_YRES,
	.xres_virtual   = CIRTHFB_XRES,
	.yres_virtual   = CIRTHFB_YRES,
	.bits_per_pixel = 1,
	.grayscale      = 1,
};

static const struct fb_fix_screeninfo cirthfb_fix = {
	.id          = "cirthfb",
	.type        = FB_TYPE_PACKED_PIXELS,
	.visual      = FB_VISUAL_MONO10,
	.line_length = CIRTHFB_STRIDE,
	.accel       = FB_ACCEL_NONE,
};

/* ------------------------------------------------------------------ */
/* Framebuffer operations                                             */
/* ------------------------------------------------------------------ */

/*
 * Rotate the landscape framebuffer 90° CW into the portrait EPD buffer.
 * FB pixel (x, y) → EPD col = y, row = (CIRTHFB_XRES − 1 − x).
 * Both formats are 1 bpp MSB-first; 1 = white.
 * If the image appears mirrored, swap to: col = CIRTHFB_YRES-1-y, row = x.
 */
static int cirthfb_flush(struct fb_info *info)
{
	struct cirthfb_dev *priv = info->par;
	const u8 *src = (const u8 *)info->screen_base;
	u8 *epd_buf;
	int ret, x, y, col, row, epd_byte, pixel;

	epd_buf = kmalloc(CIRTHFB_SMEM_LEN, GFP_KERNEL);
	if (!epd_buf)
		return -ENOMEM;
	memset(epd_buf, 0xFF, CIRTHFB_SMEM_LEN);

	for (y = 0; y < CIRTHFB_YRES; y++) {
		for (x = 0; x < CIRTHFB_XRES; x++) {
			pixel = (src[y * CIRTHFB_STRIDE + x / 8] >> (7 - (x % 8))) & 1;

			col      = y;
			row      = CIRTHFB_XRES - 1 - x;
			epd_byte = row * CIRTHFB_STRIDE + col / 8;
			if (pixel)
				epd_buf[epd_byte] |=  (1u << (7 - (col % 8)));
			else
				epd_buf[epd_byte] &= ~(1u << (7 - (col % 8)));
		}
	}

	mutex_lock(&priv->lock);
	ret = epd_cmd(priv, EPD_CMD_WRITE_BW_RAM);
	if (!ret)
		ret = epd_dat_buf(priv, epd_buf, CIRTHFB_SMEM_LEN);
	if (!ret)
		ret = epd_turn_on_display(priv);
	mutex_unlock(&priv->lock);
	kfree(epd_buf);
	return ret;
}

static ssize_t cirthfb_write(struct fb_info *info, const char __user *buf,
			      size_t count, loff_t *ppos)
{
	ssize_t ret = fb_sys_write(info, buf, count, ppos);

	if (ret >= 0) {
		int err = cirthfb_flush(info);

		if (err)
			return err;
	}
	return ret;
}

static const struct fb_ops cirthfb_ops = {
	.owner        = THIS_MODULE,
	.fb_read      = fb_sys_read,
	.fb_write     = cirthfb_write,
	.fb_fillrect  = sys_fillrect,
	.fb_copyarea  = sys_copyarea,
	.fb_imageblit = sys_imageblit,
	.fb_sync      = cirthfb_flush,
};

/* ------------------------------------------------------------------ */
/* SPI driver probe / remove                                          */
/* ------------------------------------------------------------------ */

static int cirthfb_probe(struct spi_device *spi)
{
	struct cirthfb_dev *priv;
	struct fb_info *info;
	int ret;

	pr_info("cirthfb: probe called\n");

	priv = devm_kzalloc(&spi->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->spi = spi;
	mutex_init(&priv->lock);
	spi_set_drvdata(spi, priv);

	/* Configure SPI bus parameters */
	spi->mode          = SPI_MODE_0;
	spi->max_speed_hz  = 4000000;
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret) {
		pr_err("cirthfb: spi_setup failed: %d\n", ret);
		return ret;
	}

	/*
	 * Claim GPIO pins via the legacy number-based API (no DT overlay),
	 * then bridge to gpio_desc for all subsequent operations.
	 * devm_ ensures automatic release on probe failure or remove.
	 */
	ret = devm_gpio_request_one(&spi->dev, dc_gpio,
				    GPIOF_OUT_INIT_LOW, "cirthfb-dc");
	if (ret) {
		pr_err("cirthfb: dc gpio %d request failed: %d\n", dc_gpio, ret);
		return ret;
	}
	priv->dc = gpio_to_desc(dc_gpio);

	ret = devm_gpio_request_one(&spi->dev, rst_gpio,
				    GPIOF_OUT_INIT_HIGH, "cirthfb-rst");
	if (ret) {
		pr_err("cirthfb: rst gpio %d request failed: %d\n", rst_gpio, ret);
		return ret;
	}
	priv->rst = gpio_to_desc(rst_gpio);

	ret = devm_gpio_request_one(&spi->dev, busy_gpio,
				    GPIOF_IN, "cirthfb-busy");
	if (ret) {
		pr_err("cirthfb: busy gpio %d request failed: %d\n", busy_gpio, ret);
		return ret;
	}
	priv->busy = gpio_to_desc(busy_gpio);

	if (!priv->dc || !priv->rst || !priv->busy) {
		pr_err("cirthfb: gpio_to_desc failed\n");
		return -EINVAL;
	}

	/* Hardware initialisation sequence */
	ret = epd_init_display(priv);
	if (ret) {
		pr_err("cirthfb: display init failed: %d\n", ret);
		return ret;
	}

	/* Clear display to all-white after init */
	ret = epd_clear_to_white(priv);
	if (ret) {
		pr_err("cirthfb: clear to white failed: %d\n", ret);
		return ret;
	}
	pr_info("cirthfb: display initialised and cleared to white\n");

	/* Allocate and register Linux framebuffer */
	info = framebuffer_alloc(0, &spi->dev);
	if (!info) {
		pr_err("cirthfb: framebuffer_alloc failed\n");
		return -ENOMEM;
	}

	info->screen_base = vzalloc(CIRTHFB_SMEM_LEN);
	if (!info->screen_base) {
		pr_err("cirthfb: vzalloc(%u) failed\n", CIRTHFB_SMEM_LEN);
		framebuffer_release(info);
		return -ENOMEM;
	}

	/* Mirror the all-white state left by epd_clear_to_white */
	memset(info->screen_base, 0xFF, CIRTHFB_SMEM_LEN);

	info->fix          = cirthfb_fix;
	info->fix.smem_len = CIRTHFB_SMEM_LEN; /* smem_start stays 0: vzalloc has no phys addr */
	info->var          = cirthfb_var;
	info->fbops        = &cirthfb_ops;
	info->par          = priv;
	info->flags        = 0;

	priv->info = info;

	ret = register_framebuffer(info);
	if (ret) {
		pr_err("cirthfb: register_framebuffer failed: %d\n", ret);
		vfree(info->screen_base);
		framebuffer_release(info);
		return ret;
	}

	pr_info("cirthfb: registered as /dev/fb%d\n", info->node);
	pr_info("cirthfb: probe done\n");
	return 0;
}

static void cirthfb_remove(struct spi_device *spi)
{
	struct cirthfb_dev *priv = spi_get_drvdata(spi);

	if (priv->info) {
		unregister_framebuffer(priv->info);
		vfree(priv->info->screen_base);
		framebuffer_release(priv->info);
	}
	mutex_destroy(&priv->lock);
	/* GPIOs released automatically by devm */
	pr_info("cirthfb: removed\n");
}

/* ------------------------------------------------------------------ */
/* SPI driver registration                                            */
/* ------------------------------------------------------------------ */

static const struct of_device_id cirthfb_of_match[] = {
	{ .compatible = "waveshare,epd2in13v4" },
	{ }
};
MODULE_DEVICE_TABLE(of, cirthfb_of_match);

static const struct spi_device_id cirthfb_spi_id[] = {
	{ "waveshare,epd2in13v4", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, cirthfb_spi_id);

static struct spi_driver cirthfb_spi_driver = {
	.driver = {
		.name           = "cirthfb",
		.owner          = THIS_MODULE,
		.of_match_table = cirthfb_of_match,
	},
	.probe    = cirthfb_probe,
	.remove   = cirthfb_remove,
	.id_table = cirthfb_spi_id,
};

module_spi_driver(cirthfb_spi_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("reboot-required");
MODULE_DESCRIPTION("Framebuffer driver for Waveshare 2.13in e-ink HAT V4");
MODULE_ALIAS("spi:epd2in13v4");
