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
	struct mipi_dsi_device *dsi = r69429->dsi;
	int ret;
	int err;

	// Command access protect
	u8 com1[] = { 0xb0, 0x00 };
	err =  mipi_dsi_generic_write(dsi, com1, 2);
	err +=  mipi_dsi_generic_write(dsi, com1, 2);

	u8 com2[] = { 0xd6, 0x01 };
	err +=  mipi_dsi_generic_write(dsi, com2, 2);

	u8 com3[] = { 0xb3, 0x04, 0x08, 0x00, 0x22, 0x00 };
	err +=  mipi_dsi_generic_write(dsi, com3, 6);

	//Interface setting
	u8 com4[] = { 0xb3, 0x14, 0x08, 0x00, 0x22, 0x00 };
	// video thru mode w/o RAM
	err +=  mipi_dsi_generic_write(dsi, com4, 6);

	// interface ID setting
	u8 com5[] = { 0xb4, 0x0c }; // virt chan 0
	err +=  mipi_dsi_generic_write(dsi, com5, 2);

	u8 coma1[] = { 0xde, 0x00 };
	err +=  mipi_dsi_generic_write(dsi, coma1, 2 );

	// dsi control - 1Gbit/s/lane / 500MHz.
	u8 com6[] = { 0xb6, 0x3a, 0xd3 };
	err +=  mipi_dsi_generic_write(dsi, com6, 3);

	u8 com7[] = { 0x67, 0x60, 0x02 };
	err +=  mipi_dsi_generic_write(dsi, com7, 3);

	// write_display_brightness
	u8 com8[] = { 0x51, 0xe0 };
	err +=  mipi_dsi_generic_write(dsi, com8, 2);

	// write_CTRL_display
	u8 com9[] = { 0x53, 0x04 };
	err +=  mipi_dsi_generic_write(dsi, com9, 2);

	// set pixel format
	u8 com10[] = { 0x3a, 0x77 }; // 24 bpp
	err +=  mipi_dsi_generic_write(dsi, com10, 2);

	// set tear on
	u8 coma2[] = { 0x35, 0x01 };
	err +=  mipi_dsi_generic_write(dsi, coma2, 2);

	// set column address
	u8 com11[] = { 0x2a, 0x00, 0x00, 0x04, 0xaf };
	err +=  mipi_dsi_generic_write(dsi, com11, 5);

	// set page address
	u8 com12[] = { 0x2b, 0x00, 0x00, 0x07, 0x7f };
	err +=  mipi_dsi_generic_write(dsi, com12, 5);

	if (err < 0) {
		DRM_ERROR("failed to init: %d\n",ret);
		return err;
	}
	return 0;
}

static int r69429_panel_on(struct r69429_panel *r69429)
{
	struct mipi_dsi_device *dsi = r69429->dsi;
	struct device *dev = &r69429->dsi->dev;
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
	.clock = 	151350,
	.hdisplay = 	1200,
	.hsync_start = 	1200 + 60,
	.hsync_end = 	1200 + 60 + 10,
	.htotal = 	1200 + 60 + 10 + 35,
	.vdisplay = 	1920,
	.vsync_start = 	1920 + 5,
	.vsync_end = 	1920 + 5 + 2,
	.vtotal = 	1920 + 5 + 2 + 6,
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
	dsi->mode_flags =  MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_LPM;

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
