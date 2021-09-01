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

struct r69429_panel {
	struct drm_panel base;
	struct mipi_dsi_device *dsi;

	struct regulator_bulk_data supplies[ARRAY_SIZE(regulator_names)];

	struct gpio_desc *reset_gpio;

	bool prepared;
	bool enabled;

	const struct drm_display_mode *mode;
};

static inline struct r69429_panel *to_r69429_panel(struct drm_panel *panel)
{
	return container_of(panel, struct r69429_panel, base);
}

static int r69429_panel_init(struct r69429_panel *r69429)
{
	gpiod_set_value_cansleep(r69429->reset_gpio, 0);
	msleep(25);
	gpiod_set_value_cansleep(r69429->reset_gpio, 1);
	msleep(510);
	return 0;
}

static int r69429_panel_on(struct r69429_panel *r69429)
{
	struct mipi_dsi_device *dsi = r69429->dsi;
	struct device *dev = &r69429->dsi->dev;
	int ret;

	// ret = mipi_dsi_dcs_write(dev, 0x09, 0x01);
	// if (ret < 0)
	// 	dev_err(dev, "failed to enter bypass mode: %d\n",ret);

	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret < 0)
		dev_err(dev, "failed to exit sleep mode: %d\n",ret);

	msleep(150);

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret < 0)
		dev_err(dev, "failed to set display on: %d\n",ret);

	msleep(150);

	return ret;
}

static void r69429_panel_off(struct r69429_panel *r69429)
{
	struct mipi_dsi_device *dsi = r69429->dsi;
	struct device *dev = &r69429->dsi->dev;
	int ret;

	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret < 0)
		dev_err(dev, "failed to set display off: %d\n", ret);

	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret < 0)
		dev_err(dev, "failed to enter sleep mode: %d\n", ret);

	msleep(100);
}

static int r69429_panel_disable(struct drm_panel *panel)
{
	struct r69429_panel *r69429 = to_r69429_panel(panel);

	if (!r69429->enabled)
		return 0;

	r69429->enabled = false;

	return 0;
}

static int r69429_panel_unprepare(struct drm_panel *panel)
{
	struct r69429_panel *r69429 = to_r69429_panel(panel);
	struct device *dev = &r69429->dsi->dev;
	int ret;

	if (!r69429->prepared)
		return 0;

	r69429_panel_off(r69429);

	ret = regulator_bulk_disable(ARRAY_SIZE(r69429->supplies), r69429->supplies);
	if (ret < 0)
		dev_err(dev, "regulator disable failed, %d\n", ret);

	r69429->prepared = false;

	return 0;
}

static int r69429_panel_prepare(struct drm_panel *panel)
{
	struct r69429_panel *r69429 = to_r69429_panel(panel);
	struct device *dev = &r69429->dsi->dev;
	struct mipi_dsi_device *dsi = r69429->dsi;
	int ret;
	if (r69429->prepared)
		return 0;

	ret = regulator_bulk_enable(ARRAY_SIZE(r69429->supplies), r69429->supplies);
	if (ret < 0) {
		dev_err(dev, "regulator enable failed, %d\n", ret);
		return ret;
	}

	ret = r69429_panel_init(r69429);

	if (ret < 0) {
		dev_err(dev, "failed to init panel: %d\n", ret);
		goto poweroff;
	}

	r69429->prepared = true;
	return 0;

poweroff:
	ret = regulator_bulk_disable(ARRAY_SIZE(r69429->supplies), r69429->supplies);
	if (ret < 0)
		dev_err(dev, "regulator disable failed, %d\n", ret);

	return ret;
}

static int r69429_panel_enable(struct drm_panel *panel)
{
	struct r69429_panel *r69429 = to_r69429_panel(panel);
	struct device *dev = &r69429->dsi->dev;
	int ret;

	if (r69429->enabled)
		return 0;

	ret = r69429_panel_on(r69429);
	if (ret < 0) {
		dev_err(dev, "failed to set panel on: %d\n", ret);
		return ret;
	}

	msleep(100);
	r69429->enabled = true;
	return 0;
}

static const struct drm_display_mode default_mode = {
	.clock = 	162560,
	.hdisplay = 	1200,
	.hsync_start = 	1200 + 70,
	.hsync_end = 	1200 + 70 + 8,
	.htotal = 	1200 + 70 + 8 + 70,
	.vdisplay = 	1920,
	.vsync_start = 	1920 + 4,
	.vsync_end = 	1920 + 4 + 2,
	.vtotal = 	1920 + 4 + 2 + 84,
};

static int r69429_panel_get_modes(struct drm_panel *panel,
				struct drm_connector *connector)
{
	struct drm_display_mode *mode;
	struct r69429_panel *r69429 = to_r69429_panel(panel);
	struct device *dev = &r69429->dsi->dev;

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

static const struct drm_panel_funcs r69429_panel_funcs = {
	.disable = r69429_panel_disable,
	.unprepare = r69429_panel_unprepare,
	.prepare = r69429_panel_prepare,
	.enable = r69429_panel_enable,
	.get_modes = r69429_panel_get_modes,
};

static const struct of_device_id r69429_of_match[] = {
	{ .compatible = "renesassp,r69429", },
	{ }
};
MODULE_DEVICE_TABLE(of, r69429_of_match);

static int r69429_panel_add(struct r69429_panel *r69429)
{
	struct device *dev = &r69429->dsi->dev;
	int ret;
	unsigned int i;

	r69429->mode = &default_mode;

	for (i = 0; i < ARRAY_SIZE(r69429->supplies); i++)
		r69429->supplies[i].supply = regulator_names[i];

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(r69429->supplies),
				r69429->supplies);
	if (ret < 0) {
		dev_err(dev, "failed to init regulator, ret=%d\n", ret);
		return ret;
	}

	r69429->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(r69429->reset_gpio)) {
		ret = PTR_ERR(r69429->reset_gpio);
		dev_err(dev, "cannot get reset-gpios %d\n", ret);
		return ret;
	}

	drm_panel_init(&r69429->base, &r69429->dsi->dev, &r69429_panel_funcs,
			DRM_MODE_CONNECTOR_DSI);
	drm_panel_add(&r69429->base);
	return 0;
}

static void r69429_panel_del(struct r69429_panel *r69429)
{
	if (r69429->base.dev)
		drm_panel_remove(&r69429->base);
}

static int r69429_panel_probe(struct mipi_dsi_device *dsi)
{
	struct r69429_panel *r69429;
	int ret;

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags =  MIPI_DSI_MODE_VIDEO |
											MIPI_DSI_MODE_LPM;

	r69429 = devm_kzalloc(&dsi->dev, sizeof(*r69429), GFP_KERNEL);
	if (!r69429)
		return -ENOMEM;

	mipi_dsi_set_drvdata(dsi, r69429);
	r69429->dsi = dsi;

	ret = r69429_panel_add(r69429);
	if (ret < 0)
		return ret;

	return mipi_dsi_attach(dsi);
}

static int r69429_panel_remove(struct mipi_dsi_device *dsi)
{
	struct r69429_panel *r69429 = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = r69429_panel_disable(&r69429->base);
	if (ret < 0)
		dev_err(&dsi->dev, "failed to disable panel: %d\n", ret);

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n",
				ret);

	r69429_panel_del(r69429);

	return 0;
}

static void r69429_panel_shutdown(struct mipi_dsi_device *dsi)
{
	struct r69429_panel *r69429 = mipi_dsi_get_drvdata(dsi);

	r69429_panel_disable(&r69429->base);
}

static struct mipi_dsi_driver r69429_panel_driver = {
	.driver = {
		.name = "panel-renesassp-r69429",
		.of_match_table = r69429_of_match,
	},
	.probe = r69429_panel_probe,
	.remove = r69429_panel_remove,
	.shutdown = r69429_panel_shutdown,
};
module_mipi_dsi_driver(r69429_panel_driver);

MODULE_AUTHOR("Ryan Pannell <ryan@osukl.com>");
MODULE_AUTHOR("Dave Stevenson <dave.stevenson@raspberrypi.com>");
MODULE_DESCRIPTION("MIPI DSI Driver for RenesasSP R69429 LCD Controller IC");
MODULE_LICENSE("GPL v2");
