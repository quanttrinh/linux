// SPDX-License-Identifier: GPL-2.0
/*
 * SGM31324 3-channel current-regulated RGB LED driver.
 */
#include <linux/bitfield.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/led-class-multicolor.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>

#define SGM31324_REG_ENABLE_RESET	0x00
#define SGM31324_EN_CTRL		GENMASK(4, 3)
#define SGM31324_EN_ALWAYS_ON		3

#define SGM31324_REG_CHANNEL		0x04
#define SGM31324_DEV_EN			BIT(6)
#define SGM31324_LED_MODE_MASK		GENMASK(1, 0)
#define SGM31324_LED_MODE_ALWAYS_ON	1

#define SGM31324_REG_LED1_CURRENT	0x06
#define SGM31324_REG_LED2_CURRENT	0x07
#define SGM31324_REG_LED3_CURRENT	0x08

#define SGM31324_MAX_BRIGHTNESS		0xbf
#define SGM31324_MAX_COLORS		3

struct sgm31324_led {
	struct regmap *regmap;
	struct led_classdev_mc mc_cdev;
	struct mc_subled subled_info[SGM31324_MAX_COLORS];
};

static unsigned int sgm31324_current_reg(unsigned int color)
{
	switch (color) {
	case LED_COLOR_ID_RED:
		return SGM31324_REG_LED1_CURRENT;
	case LED_COLOR_ID_GREEN:
		return SGM31324_REG_LED2_CURRENT;
	case LED_COLOR_ID_BLUE:
		return SGM31324_REG_LED3_CURRENT;
	default:
		return 0;
	}
}

static unsigned int sgm31324_mode_shift(unsigned int color)
{
	switch (color) {
	case LED_COLOR_ID_RED:
		return 0;
	case LED_COLOR_ID_GREEN:
		return 2;
	case LED_COLOR_ID_BLUE:
		return 4;
	default:
		return 0;
	}
}

static int sgm31324_brightness_set(struct led_classdev *cdev,
				   enum led_brightness brightness)
{
	struct led_classdev_mc *mc = lcdev_to_mccdev(cdev);
	struct sgm31324_led *led = container_of(mc, struct sgm31324_led, mc_cdev);
	unsigned int mode_mask = 0, mode_val = 0;
	int i, ret;

	led_mc_calc_color_components(mc, brightness);

	for (i = 0; i < mc->num_colors; i++) {
		struct mc_subled *subled = &mc->subled_info[i];
		unsigned int reg = sgm31324_current_reg(subled->color_index);
		unsigned int shift = sgm31324_mode_shift(subled->color_index);

		ret = regmap_write(led->regmap, reg,
				   min_t(unsigned int, subled->brightness,
					 SGM31324_MAX_BRIGHTNESS));
		if (ret)
			return ret;

		mode_mask |= SGM31324_LED_MODE_MASK << shift;
		if (subled->brightness)
			mode_val |= SGM31324_LED_MODE_ALWAYS_ON << shift;
	}

	return regmap_update_bits(led->regmap, SGM31324_REG_CHANNEL,
				  SGM31324_DEV_EN | mode_mask,
				  (brightness ? SGM31324_DEV_EN : 0) | mode_val);
}

static const struct regmap_config sgm31324_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = SGM31324_REG_LED3_CURRENT,
};

static int sgm31324_probe(struct i2c_client *client,
			  const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct device_node *node = dev->of_node;
	struct device_node *multi_led, *sub_node;
	struct gpio_desc *enable;
	struct regmap *regmap;
	int ret;

	enable = devm_gpiod_get_optional(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(enable))
		return dev_err_probe(dev, PTR_ERR(enable), "Failed to get enable GPIO\n");

	regmap = devm_regmap_init_i2c(client, &sgm31324_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap), "Failed to init regmap\n");

	gpiod_set_value_cansleep(enable, 1);
	usleep_range(100, 200);

	/* Keep the chip awake regardless of SCL/SDA idle state. */
	ret = regmap_update_bits(regmap, SGM31324_REG_ENABLE_RESET,
				 SGM31324_EN_CTRL,
				 FIELD_PREP(SGM31324_EN_CTRL,
					    SGM31324_EN_ALWAYS_ON));
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable device\n");
	/* Start with all channels off, matching led_cdev brightness = 0. */
	ret = regmap_write(regmap, SGM31324_REG_CHANNEL, 0);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to initialise channels\n");

	for_each_available_child_of_node(node, multi_led) {
		struct sgm31324_led *led;
		struct led_init_data init_data = {};
		unsigned int color, num_colors = 0;

		led = devm_kzalloc(dev, sizeof(*led), GFP_KERNEL);
		if (!led) {
			of_node_put(multi_led);
			return -ENOMEM;
		}
		led->regmap = regmap;
		init_data.fwnode = of_fwnode_handle(multi_led);

		for_each_available_child_of_node(multi_led, sub_node) {
			ret = of_property_read_u32(sub_node, "color", &color);
			if (ret) {
				dev_err(dev, "sub-LED has no color property\n");
				of_node_put(sub_node);
				of_node_put(multi_led);
				return ret;
			}
			if (num_colors >= SGM31324_MAX_COLORS ||
			    !sgm31324_current_reg(color)) {
				dev_warn(dev, "unsupported color %u\n", color);
				continue;
			}
			led->subled_info[num_colors].color_index = color;
			led->subled_info[num_colors].intensity =
				SGM31324_MAX_BRIGHTNESS;
			num_colors++;
		}

		if (!num_colors) {
			of_node_put(multi_led);
			return dev_err_probe(dev, -EINVAL, "no valid sub-LEDs\n");
		}

		led->mc_cdev.num_colors = num_colors;
		led->mc_cdev.subled_info = led->subled_info;
		led->mc_cdev.led_cdev.max_brightness = SGM31324_MAX_BRIGHTNESS;
		led->mc_cdev.led_cdev.brightness_set_blocking =
			sgm31324_brightness_set;

		ret = devm_led_classdev_multicolor_register_ext(dev, &led->mc_cdev,
							      &init_data);
		if (ret) {
			of_node_put(multi_led);
			return dev_err_probe(dev, ret, "Failed to register LED\n");
		}
	}

	return 0;
}

static const struct of_device_id sgm31324_of_match[] = {
	{ .compatible = "sgmic,sgm31324" },
	{ }
};
MODULE_DEVICE_TABLE(of, sgm31324_of_match);

static const struct i2c_device_id sgm31324_i2c_id[] = {
	{ "sgm31324", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, sgm31324_i2c_id);

static struct i2c_driver sgm31324_driver = {
	.driver = {
		.name = "sgm31324",
		.of_match_table = sgm31324_of_match,
	},
	.probe = sgm31324_probe,
	.id_table = sgm31324_i2c_id,
};
module_i2c_driver(sgm31324_driver);

MODULE_DESCRIPTION("SGM31324 RGB LED driver");
MODULE_LICENSE("GPL");
