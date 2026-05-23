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
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/spi/spi.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>

/* Display geometry (EPD2IN13V4: 122 columns × 250 rows, monochrome) */
#define EPD_WIDTH    122
#define EPD_HEIGHT   250
#define EPD_STRIDE   (EPD_WIDTH / 8)           /* 16 bytes per row */
#define EPD_FB_SIZE  (EPD_STRIDE * EPD_HEIGHT) /* 4000 bytes total */

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

static char *display = "waveshare2in13v4";
module_param(display, charp, 0444);
MODULE_PARM_DESC(display, "Display type (default: waveshare2in13v4)");

struct cirthfb_dev {
	struct spi_device *spi;
	u8               *fb_buf;
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

static int epd_cmd(struct spi_device *spi, u8 cmd)
{
	gpio_set_value(dc_gpio, 0); /* DC low = command */
	return spi_write(spi, &cmd, 1);
}

static int epd_dat(struct spi_device *spi, u8 data)
{
	gpio_set_value(dc_gpio, 1); /* DC high = data */
	return spi_write(spi, &data, 1);
}

static int epd_dat_buf(struct spi_device *spi, const u8 *buf, size_t len)
{
	gpio_set_value(dc_gpio, 1);
	return spi_write(spi, buf, len);
}

/* ------------------------------------------------------------------ */
/* Hardware control                                                    */
/* ------------------------------------------------------------------ */

static void epd_hw_reset(void)
{
	gpio_set_value(rst_gpio, 1);
	msleep(10);
	gpio_set_value(rst_gpio, 0);
	msleep(2);
	gpio_set_value(rst_gpio, 1);
	msleep(10);
}

/* BUSY is HIGH while the controller is busy; poll until LOW */
static int epd_wait_busy(void)
{
	int retries = 500; /* 5 s timeout at 10 ms per poll */

	while (gpio_get_value(busy_gpio) == 1) {
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

static int epd_init_display(struct spi_device *spi)
{
	int ret;

	epd_hw_reset();

	ret = epd_wait_busy();
	if (ret)
		return ret;

	/* Software reset — controller re-loads OTP defaults */
	ret = epd_cmd(spi, EPD_CMD_SW_RESET);
	if (ret)
		return ret;
	ret = epd_wait_busy();
	if (ret)
		return ret;

	/* Driver output control: MUX = height − 1, normal gate order */
	ret  = epd_cmd(spi, EPD_CMD_DRIVER_OUTPUT);
	ret |= epd_dat(spi, (EPD_HEIGHT - 1) & 0xFF);
	ret |= epd_dat(spi, ((EPD_HEIGHT - 1) >> 8) & 0x01);
	ret |= epd_dat(spi, 0x00);
	if (ret)
		return ret;

	/* Data entry mode: X-increment then Y-increment, address-counter mode */
	ret  = epd_cmd(spi, EPD_CMD_DATA_ENTRY);
	ret |= epd_dat(spi, 0x03);
	if (ret)
		return ret;

	/* RAM X address window: bytes 0 … EPD_STRIDE-1 */
	ret  = epd_cmd(spi, EPD_CMD_SET_RAMX_ADDR);
	ret |= epd_dat(spi, 0x00);
	ret |= epd_dat(spi, EPD_STRIDE - 1);
	if (ret)
		return ret;

	/* RAM Y address window: rows 0 … EPD_HEIGHT-1 */
	ret  = epd_cmd(spi, EPD_CMD_SET_RAMY_ADDR);
	ret |= epd_dat(spi, 0x00);
	ret |= epd_dat(spi, 0x00);
	ret |= epd_dat(spi, (EPD_HEIGHT - 1) & 0xFF);
	ret |= epd_dat(spi, ((EPD_HEIGHT - 1) >> 8) & 0x01);
	if (ret)
		return ret;

	/* Border waveform: VSS / VBD_GS / follow LUT */
	ret  = epd_cmd(spi, EPD_CMD_BORDER_WAVEFORM);
	ret |= epd_dat(spi, 0x05);
	if (ret)
		return ret;

	/* Display update control 1: keep defaults, enable bypass clock */
	ret  = epd_cmd(spi, EPD_CMD_DISPLAY_UPDATE1);
	ret |= epd_dat(spi, 0x00);
	ret |= epd_dat(spi, 0x80);
	if (ret)
		return ret;

	/* Use built-in temperature sensor */
	ret  = epd_cmd(spi, EPD_CMD_TEMP_SENSOR);
	ret |= epd_dat(spi, 0x80);
	if (ret)
		return ret;

	/* Reset RAM address counters to (0, 0) */
	ret  = epd_cmd(spi, EPD_CMD_SET_RAMX_COUNTER);
	ret |= epd_dat(spi, 0x00);
	if (ret)
		return ret;
	ret  = epd_cmd(spi, EPD_CMD_SET_RAMY_COUNTER);
	ret |= epd_dat(spi, 0x00);
	ret |= epd_dat(spi, 0x00);
	if (ret)
		return ret;

	return epd_wait_busy();
}

/* Trigger a full-panel refresh from whichever data is already in RAM */
static int epd_turn_on_display(struct spi_device *spi)
{
	int ret;

	ret  = epd_cmd(spi, EPD_CMD_DISPLAY_UPDATE2);
	ret |= epd_dat(spi, 0xF7); /* full refresh sequence */
	if (ret)
		return ret;
	ret = epd_cmd(spi, EPD_CMD_MASTER_ACTIVATE);
	if (ret)
		return ret;
	return epd_wait_busy();
}

/* Write 0xFF to both BW and RED RAM and refresh → all-white panel */
static int epd_clear_to_white(struct spi_device *spi)
{
	u8 *white;
	int ret;

	white = kmalloc(EPD_FB_SIZE, GFP_KERNEL);
	if (!white)
		return -ENOMEM;
	memset(white, 0xFF, EPD_FB_SIZE);

	ret  = epd_cmd(spi, EPD_CMD_WRITE_BW_RAM);
	ret |= epd_dat_buf(spi, white, EPD_FB_SIZE);
	if (!ret) {
		ret  = epd_cmd(spi, EPD_CMD_WRITE_RED_RAM);
		ret |= epd_dat_buf(spi, white, EPD_FB_SIZE);
	}

	kfree(white);

	if (ret)
		return ret;

	return epd_turn_on_display(spi);
}

/* ------------------------------------------------------------------ */
/* Framebuffer → display transfer                                     */
/* ------------------------------------------------------------------ */

static int cirthfb_update_display(struct cirthfb_dev *priv)
{
	struct spi_device *spi = priv->spi;
	int ret;

	ret  = epd_cmd(spi, EPD_CMD_WRITE_BW_RAM);
	ret |= epd_dat_buf(spi, priv->fb_buf, EPD_FB_SIZE);
	if (ret)
		return ret;

	return epd_turn_on_display(spi);
}

/* ------------------------------------------------------------------ */
/* Test pattern: 8×8 checkerboard                                     */
/* ------------------------------------------------------------------ */

static void fill_checkerboard(u8 *buf)
{
	int row, col;

	for (row = 0; row < EPD_HEIGHT; row++) {
		for (col = 0; col < EPD_STRIDE; col++) {
			/*
			 * Tile on 8-pixel boundaries in both axes.
			 * 0xFF = all white bits, 0x00 = all black bits.
			 */
			buf[row * EPD_STRIDE + col] =
				(((row / 8) + col) & 1) ? 0x00 : 0xFF;
		}
	}
}

/* ------------------------------------------------------------------ */
/* SPI driver probe / remove                                          */
/* ------------------------------------------------------------------ */

static int cirthfb_probe(struct spi_device *spi)
{
	struct cirthfb_dev *priv;
	int ret;

	pr_info("cirthfb: probe called (display=%s)\n", display);

	priv = devm_kzalloc(&spi->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->spi = spi;
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

	/* Claim GPIO pins; devm_ ensures release on probe failure or remove */
	ret = devm_gpio_request_one(&spi->dev, dc_gpio,
				    GPIOF_OUT_INIT_LOW, "cirthfb-dc");
	if (ret) {
		pr_err("cirthfb: dc gpio %d request failed: %d\n", dc_gpio, ret);
		return ret;
	}

	ret = devm_gpio_request_one(&spi->dev, rst_gpio,
				    GPIOF_OUT_INIT_HIGH, "cirthfb-rst");
	if (ret) {
		pr_err("cirthfb: rst gpio %d request failed: %d\n", rst_gpio, ret);
		return ret;
	}

	ret = devm_gpio_request_one(&spi->dev, busy_gpio,
				    GPIOF_IN, "cirthfb-busy");
	if (ret) {
		pr_err("cirthfb: busy gpio %d request failed: %d\n", busy_gpio, ret);
		return ret;
	}

	/* Hardware initialisation sequence */
	ret = epd_init_display(spi);
	if (ret) {
		pr_err("cirthfb: display init failed: %d\n", ret);
		return ret;
	}

	/* Clear display to all-white after init */
	ret = epd_clear_to_white(spi);
	if (ret) {
		pr_err("cirthfb: clear to white failed: %d\n", ret);
		return ret;
	}
	pr_info("cirthfb: display initialised and cleared to white\n");

	/* Allocate kernel framebuffer */
	priv->fb_buf = vmalloc(EPD_FB_SIZE);
	if (!priv->fb_buf) {
		pr_err("cirthfb: vmalloc(%u) failed\n", EPD_FB_SIZE);
		return -ENOMEM;
	}

	/* Fill buffer with checkerboard test pattern and push to panel */
	fill_checkerboard(priv->fb_buf);
	ret = cirthfb_update_display(priv);
	if (ret) {
		pr_err("cirthfb: update_display failed: %d\n", ret);
		vfree(priv->fb_buf);
		priv->fb_buf = NULL;
		return ret;
	}

	pr_info("cirthfb: probe done — checkerboard visible on display\n");
	return 0;
}

static void cirthfb_remove(struct spi_device *spi)
{
	struct cirthfb_dev *priv = spi_get_drvdata(spi);

	vfree(priv->fb_buf);
	/* GPIOs released automatically by devm */
	pr_info("cirthfb: removed\n");
}

/* ------------------------------------------------------------------ */
/* SPI driver registration                                            */
/* ------------------------------------------------------------------ */

static const struct spi_device_id cirthfb_spi_id[] = {
	{ "waveshare_epd2in13v4", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, cirthfb_spi_id);

static struct spi_driver cirthfb_spi_driver = {
	.driver = {
		.name  = "cirthfb",
		.owner = THIS_MODULE,
	},
	.probe    = cirthfb_probe,
	.remove   = cirthfb_remove,
	.id_table = cirthfb_spi_id,
};

module_spi_driver(cirthfb_spi_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("reboot-required");
MODULE_DESCRIPTION("cirthfb e-ink framebuffer driver");
