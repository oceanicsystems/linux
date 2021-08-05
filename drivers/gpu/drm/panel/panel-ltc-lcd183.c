#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_crtc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_print.h>

static const char * const regulator_names[] = {
	"vddp",
	"iovcc"
};

struct lcd183_panel {
	struct drm_panel base;
	struct mipi_dsi_device *dsi;

	struct regulator_bulk_data supplies[ARRAY_SIZE(regulator_names)];

	struct gpio_desc *reset_gpio;

	bool prepared;
	bool enabled;

	const struct drm_display_mode *mode;
};

static inline struct lcd183_panel *to_lcd183_panel(struct drm_panel *panel)
{
	return container_of(panel, struct lcd183_panel, base);
}

static int lcd183_panel_init(struct lcd183_panel *lcd183)
{
	gpiod_set_value_cansleep(lcd183->reset_gpio, 0);
	msleep(25);
	gpiod_set_value_cansleep(lcd183->reset_gpio, 1);
	msleep(510);
	return 0;
}

static int lcd183_panel_on(struct lcd183_panel *lcd183)
{
	struct mipi_dsi_device *dsi = lcd183->dsi;
	struct device *dev = &lcd183->dsi->dev;
	int ret;

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret < 0)
		dev_err(dev, "failed to set display on: %d\n",ret);

	msleep(100);

	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret < 0)
		dev_err(dev, "failed to exit sleep mode: %d\n",ret);

	msleep(100);
	return ret;
}

static void lcd183_panel_off(struct lcd183_panel *lcd183)
{
	struct mipi_dsi_device *dsi = lcd183->dsi;
	struct device *dev = &lcd183->dsi->dev;
	int ret;

	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret < 0)
		dev_err(dev, "failed to set display off: %d\n", ret);

	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret < 0)
		dev_err(dev, "failed to enter sleep mode: %d\n", ret);

	msleep(100);
}

static int lcd183_panel_disable(struct drm_panel *panel)
{
	struct lcd183_panel *lcd183 = to_lcd183_panel(panel);

	if (!lcd183->enabled)
		return 0;

	lcd183->enabled = false;

	return 0;
}

static int lcd183_panel_unprepare(struct drm_panel *panel)
{
	struct lcd183_panel *lcd183 = to_lcd183_panel(panel);
	struct device *dev = &lcd183->dsi->dev;
	int ret;

	if (!lcd183->prepared)
		return 0;

	lcd183_panel_off(lcd183);

	ret = regulator_bulk_disable(ARRAY_SIZE(lcd183->supplies), lcd183->supplies);
	if (ret < 0)
		dev_err(dev, "regulator disable failed, %d\n", ret);

	lcd183->prepared = false;

	return 0;
}

static int lcd183_panel_prepare(struct drm_panel *panel)
{
	struct lcd183_panel *lcd183 = to_lcd183_panel(panel);
	struct device *dev = &lcd183->dsi->dev;
	struct mipi_dsi_device *dsi = lcd183->dsi;
	int ret;
	if (lcd183->prepared)
		return 0;

	ret = regulator_bulk_enable(ARRAY_SIZE(lcd183->supplies), lcd183->supplies);
	if (ret < 0) {
		dev_err(dev, "regulator enable failed, %d\n", ret);
		return ret;
	}

	ret = lcd183_panel_init(lcd183);

	if (ret < 0) {
		dev_err(dev, "failed to init panel: %d\n", ret);
		goto poweroff;
	}

	lcd183->prepared = true;
	return 0;

poweroff:
	ret = regulator_bulk_disable(ARRAY_SIZE(lcd183->supplies), lcd183->supplies);
	if (ret < 0)
		dev_err(dev, "regulator disable failed, %d\n", ret);

	return ret;
}

static int lcd183_panel_enable(struct drm_panel *panel)
{
	struct lcd183_panel *lcd183 = to_lcd183_panel(panel);
	struct device *dev = &lcd183->dsi->dev;
	int ret;

	if (lcd183->enabled)
		return 0;

	ret = lcd183_panel_on(lcd183);
	if (ret < 0) {
		dev_err(dev, "failed to set panel on: %d\n", ret);
		return ret;
	}

	msleep(100);
	lcd183->enabled = true;
	return 0;
}

static const struct drm_display_mode default_mode = {
	.clock = 	162000,
	.hdisplay = 	1200,
	.hsync_start = 	1200 + 11,
	.hsync_end = 	1200 + 11 + 8,
	.htotal = 	1200 + 11 + 8 + 10,
	.vdisplay = 	1920,
	.vsync_start = 	1920 + 4,
	.vsync_end = 	1920 + 4 + 4,
	.vtotal = 	1920 + 4+ 4 + 76,
};

static int lcd183_panel_get_modes(struct drm_panel *panel,
				struct drm_connector *connector)
{
	struct drm_display_mode *mode;
	struct lcd183_panel *lcd183 = to_lcd183_panel(panel);
	struct device *dev = &lcd183->dsi->dev;

	mode = drm_mode_duplicate(connector->dev, &default_mode);
	if (!mode) {
		dev_err(dev, "failed to add mode %ux%ux@%u\n",
			default_mode.hdisplay, default_mode.vdisplay,
			drm_mode_vrefresh(&default_mode));
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = 94;
	connector->display_info.height_mm = 151;

	return 1;
}

static const struct drm_panel_funcs lcd183_panel_funcs = {
	.disable = lcd183_panel_disable,
	.unprepare = lcd183_panel_unprepare,
	.prepare = lcd183_panel_prepare,
	.enable = lcd183_panel_enable,
	.get_modes = lcd183_panel_get_modes,
};

static const struct of_device_id lcd183_of_match[] = {
	{ .compatible = "lts,lcd183", },
	{ }
};
MODULE_DEVICE_TABLE(of, lcd183_of_match);

static int lcd183_panel_add(struct lcd183_panel *lcd183)
{
	struct device *dev = &lcd183->dsi->dev;
	int ret;
	unsigned int i;

	lcd183->mode = &default_mode;

	for (i = 0; i < ARRAY_SIZE(lcd183->supplies); i++)
		lcd183->supplies[i].supply = regulator_names[i];

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(lcd183->supplies),
				lcd183->supplies);
	if (ret < 0) {
		dev_err(dev, "failed to init regulator, ret=%d\n", ret);
		return ret;
	}

	lcd183->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(lcd183->reset_gpio)) {
		ret = PTR_ERR(lcd183->reset_gpio);
		dev_err(dev, "cannot get reset-gpios %d\n", ret);
		return ret;
	}

	drm_panel_init(&lcd183->base, &lcd183->dsi->dev, &lcd183_panel_funcs,
			DRM_MODE_CONNECTOR_DSI);
	ret = drm_panel_of_backlight(&lcd183->base);
	if (ret)
		return ret;
	drm_panel_add(&lcd183->base);
	return 0;
}

static void lcd183_panel_del(struct lcd183_panel *lcd183)
{
	if (lcd183->base.dev)
		drm_panel_remove(&lcd183->base);
}

static int lcd183_panel_probe(struct mipi_dsi_device *dsi)
{
	struct lcd183_panel *lcd183;
	int ret;

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags =  MIPI_DSI_MODE_VIDEO |
				MIPI_DSI_MODE_VIDEO_HSE |
				MIPI_DSI_CLOCK_NON_CONTINUOUS |
				MIPI_DSI_MODE_VIDEO_BURST;

	lcd183 = devm_kzalloc(&dsi->dev, sizeof(*lcd183), GFP_KERNEL);
	if (!lcd183)
		return -ENOMEM;

	mipi_dsi_set_drvdata(dsi, lcd183);
	lcd183->dsi = dsi;

	ret = lcd183_panel_add(lcd183);
	if (ret < 0)
		return ret;

	return mipi_dsi_attach(dsi);
}

static int lcd183_panel_remove(struct mipi_dsi_device *dsi)
{
	struct lcd183_panel *lcd183 = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = lcd183_panel_disable(&lcd183->base);
	if (ret < 0)
		dev_err(&dsi->dev, "failed to disable panel: %d\n", ret);

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n",
				ret);

	lcd183_panel_del(lcd183);

	return 0;
}

static void lcd183_panel_shutdown(struct mipi_dsi_device *dsi)
{
	struct lcd183_panel *lcd183 = mipi_dsi_get_drvdata(dsi);

	lcd183_panel_disable(&lcd183->base);
}

static struct mipi_dsi_driver lcd183_panel_driver = {
	.driver = {
		.name = "panel-lts-lcd183",
		.of_match_table = lcd183_of_match,
	},
	.probe = lcd183_panel_probe,
	.remove = lcd183_panel_remove,
	.shutdown = lcd183_panel_shutdown,
};
module_mipi_dsi_driver(lcd183_panel_driver);

MODULE_AUTHOR("Ryan Pannell <ryan@osukl.com>");
MODULE_AUTHOR("Dave Stevenson <dave.stevenson@raspberrypi.com>");
MODULE_DESCRIPTION("MIPI DSI Driver for lts lcd183");
MODULE_LICENSE("GPL v2");
