// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Semtech SX1301 LoRa concentrator
 *
 * Copyright (c) 2018 Andreas Färber
 *
 * Based on SX1301 HAL code:
 * Copyright (c) 2013 Semtech-Cycleo
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/lora.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/lora/dev.h>
#include <linux/spi/spi.h>

#define REG_PAGE_RESET			0
#define REG_VERSION			1
#define REG_2_SPI_RADIO_A_DATA		33
#define REG_2_SPI_RADIO_A_DATA_READBACK	34
#define REG_2_SPI_RADIO_A_ADDR		35
#define REG_2_SPI_RADIO_A_CS		37
#define REG_2_SPI_RADIO_B_DATA		38
#define REG_2_SPI_RADIO_B_DATA_READBACK	39
#define REG_2_SPI_RADIO_B_ADDR		40
#define REG_2_SPI_RADIO_B_CS		42

#define REG_PAGE_RESET_SOFT_RESET	BIT(7)

#define REG_16_GLOBAL_EN		BIT(3)

#define REG_17_CLK32M_EN		BIT(0)

#define REG_2_43_RADIO_A_EN		BIT(0)
#define REG_2_43_RADIO_B_EN		BIT(1)
#define REG_2_43_RADIO_RST		BIT(2)

struct spi_sx1301 {
	struct spi_device *parent;
	u8 page;
	u8 regs;
};

struct sx1301_priv {
	struct lora_priv lora;
	struct gpio_desc *rst_gpio;
	u8 cur_page;
	struct spi_controller *radio_a_ctrl, *radio_b_ctrl;
};

static int sx1301_read(struct spi_device *spi, u8 reg, u8 *val)
{
	u8 addr = reg & 0x7f;
	return spi_write_then_read(spi, &addr, 1, val, 1);
}

static int sx1301_write(struct spi_device *spi, u8 reg, u8 val)
{
	u8 buf[2];

	buf[0] = reg | BIT(7);
	buf[1] = val;
	return spi_write(spi, buf, 2);
}

static int sx1301_page_switch(struct spi_device *spi, u8 page)
{
	struct sx1301_priv *priv = spi_get_drvdata(spi);
	int ret;

	if (priv->cur_page == page)
		return 0;

	dev_dbg(&spi->dev, "switching to page %u\n", (unsigned)page);
	ret = sx1301_write(spi, REG_PAGE_RESET, page & 0x3);
	if (ret) {
		dev_err(&spi->dev, "switching to page %u failed\n", (unsigned)page);
		return ret;
	}

	priv->cur_page = page;

	return 0;
}

static int sx1301_soft_reset(struct spi_device *spi)
{
	return sx1301_write(spi, REG_PAGE_RESET, REG_PAGE_RESET_SOFT_RESET);
}

#define REG_RADIO_X_DATA		0
#define REG_RADIO_X_DATA_READBACK	1
#define REG_RADIO_X_ADDR		2
#define REG_RADIO_X_CS			4

static int sx1301_radio_set_cs(struct spi_controller *ctrl, bool enable)
{
	struct spi_sx1301 *ssx = spi_controller_get_devdata(ctrl);
	u8 cs;
	int ret;

	dev_dbg(&ctrl->dev, "setting CS to %s\n", enable ? "1" : "0");

	ret = sx1301_page_switch(ssx->parent, ssx->page);
	if (ret) {
		dev_warn(&ctrl->dev, "failed to switch page for CS (%d)\n", ret);
		return ret;
	}

	ret = sx1301_read(ssx->parent, ssx->regs + REG_RADIO_X_CS, &cs);
	if (ret) {
		dev_warn(&ctrl->dev, "failed to read CS (%d)\n", ret);
		cs = 0;
	}

	if (enable)
		cs |= BIT(0);
	else
		cs &= ~BIT(0);

	ret = sx1301_write(ssx->parent, ssx->regs + REG_RADIO_X_CS, cs);
	if (ret)
		dev_warn(&ctrl->dev, "failed to write CS (%d)\n", ret);

	return 0;
}

static void sx1301_radio_spi_set_cs(struct spi_device *spi, bool enable)
{
	int ret;

	dev_dbg(&spi->dev, "setting SPI CS to %s\n", enable ? "1" : "0");

	if (enable)
		return;

	ret = sx1301_radio_set_cs(spi->controller, enable);
	if (ret)
		dev_warn(&spi->dev, "failed to write CS (%d)\n", ret);
}

static int sx1301_radio_spi_transfer_one(struct spi_controller *ctrl,
	struct spi_device *spi, struct spi_transfer *xfr)
{
	struct spi_sx1301 *ssx = spi_controller_get_devdata(ctrl);
	const u8 *tx_buf = xfr->tx_buf;
	u8 *rx_buf = xfr->rx_buf;
	int ret;

	if (xfr->len == 0 || xfr->len > 3)
		return -EINVAL;

	dev_dbg(&spi->dev, "transferring one (%u)\n", xfr->len);

	ret = sx1301_page_switch(ssx->parent, ssx->page);
	if (ret) {
		dev_err(&spi->dev, "failed to switch page for transfer (%d)\n", ret);
		return ret;
	}

	if (tx_buf) {
		ret = sx1301_write(ssx->parent, ssx->regs + REG_RADIO_X_ADDR, tx_buf ? tx_buf[0] : 0);
		if (ret) {
			dev_err(&spi->dev, "SPI radio address write failed\n");
			return ret;
		}

		ret = sx1301_write(ssx->parent, ssx->regs + REG_RADIO_X_DATA, (tx_buf && xfr->len >= 2) ? tx_buf[1] : 0);
		if (ret) {
			dev_err(&spi->dev, "SPI radio data write failed\n");
			return ret;
		}

		ret = sx1301_radio_set_cs(ctrl, true);
		if (ret) {
			dev_err(&spi->dev, "SPI radio CS set failed\n");
			return ret;
		}

		ret = sx1301_radio_set_cs(ctrl, false);
		if (ret) {
			dev_err(&spi->dev, "SPI radio CS unset failed\n");
			return ret;
		}
	}

	if (rx_buf) {
		ret = sx1301_read(ssx->parent, ssx->regs + REG_RADIO_X_DATA_READBACK, &rx_buf[xfr->len - 1]);
		if (ret) {
			dev_err(&spi->dev, "SPI radio data read failed\n");
			return ret;
		}
	}

	return 0;
}

static void sx1301_radio_setup(struct spi_controller *ctrl)
{
	ctrl->mode_bits = SPI_CS_HIGH | SPI_NO_CS;
	ctrl->bits_per_word_mask = SPI_BPW_MASK(8);
	ctrl->num_chipselect = 1;
	ctrl->set_cs = sx1301_radio_spi_set_cs;
	ctrl->transfer_one = sx1301_radio_spi_transfer_one;
}

static int sx1301_probe(struct spi_device *spi)
{
	struct net_device *netdev;
	struct sx1301_priv *priv;
	struct spi_sx1301 *radio;
	struct gpio_desc *rst;
	int ret;
	u8 val;

	rst = devm_gpiod_get_optional(&spi->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(rst))
		return PTR_ERR(rst);

	gpiod_set_value_cansleep(rst, 1);
	msleep(100);
	gpiod_set_value_cansleep(rst, 0);
	msleep(100);

	spi->bits_per_word = 8;
	spi_setup(spi);

	ret = sx1301_read(spi, REG_VERSION, &val);
	if (ret) {
		dev_err(&spi->dev, "version read failed\n");
		goto err_version;
	}

	if (val != 103) {
		dev_err(&spi->dev, "unexpected version: %u\n", val);
		ret = -ENXIO;
		goto err_version;
	}

	netdev = alloc_loradev(sizeof(*priv));
	if (!netdev) {
		ret = -ENOMEM;
		goto err_alloc_loradev;
	}

	priv = netdev_priv(netdev);
	priv->rst_gpio = rst;
	priv->cur_page = 0xff;

	spi_set_drvdata(spi, netdev);
	SET_NETDEV_DEV(netdev, &spi->dev);

	ret = sx1301_write(spi, REG_PAGE_RESET, 0);
	if (ret) {
		dev_err(&spi->dev, "page/reset write failed\n");
		return ret;
	}

	ret = sx1301_soft_reset(spi);
	if (ret) {
		dev_err(&spi->dev, "soft reset failed\n");
		return ret;
	}

	ret = sx1301_read(spi, 16, &val);
	if (ret) {
		dev_err(&spi->dev, "16 read failed\n");
		return ret;
	}

	val &= ~REG_16_GLOBAL_EN;

	ret = sx1301_write(spi, 16, val);
	if (ret) {
		dev_err(&spi->dev, "16 write failed\n");
		return ret;
	}

	ret = sx1301_read(spi, 17, &val);
	if (ret) {
		dev_err(&spi->dev, "17 read failed\n");
		return ret;
	}

	val &= ~REG_17_CLK32M_EN;

	ret = sx1301_write(spi, 17, val);
	if (ret) {
		dev_err(&spi->dev, "17 write failed\n");
		return ret;
	}

	ret = sx1301_page_switch(spi, 2);
	if (ret) {
		dev_err(&spi->dev, "page 2 switch failed\n");
		return ret;
	}

	ret = sx1301_read(spi, 43, &val);
	if (ret) {
		dev_err(&spi->dev, "2|43 read failed\n");
		return ret;
	}

	val |= REG_2_43_RADIO_B_EN | REG_2_43_RADIO_A_EN;

	ret = sx1301_write(spi, 43, val);
	if (ret) {
		dev_err(&spi->dev, "2|43 write failed\n");
		return ret;
	}

	msleep(500);

	ret = sx1301_read(spi, 43, &val);
	if (ret) {
		dev_err(&spi->dev, "2|43 read failed\n");
		return ret;
	}

	val |= REG_2_43_RADIO_RST;

	ret = sx1301_write(spi, 43, val);
	if (ret) {
		dev_err(&spi->dev, "2|43 write failed\n");
		return ret;
	}

	msleep(5);

	ret = sx1301_read(spi, 43, &val);
	if (ret) {
		dev_err(&spi->dev, "2|43 read failed\n");
		return ret;
	}

	val &= ~REG_2_43_RADIO_RST;

	ret = sx1301_write(spi, 43, val);
	if (ret) {
		dev_err(&spi->dev, "2|43 write failed\n");
		return ret;
	}

	/* radio A */

	priv->radio_a_ctrl = spi_alloc_master(&spi->dev, sizeof(*radio));
	if (!priv->radio_a_ctrl) {
		ret = -ENOMEM;
		goto err_radio_a_alloc;
	}

	sx1301_radio_setup(priv->radio_a_ctrl);
	priv->radio_a_ctrl->dev.of_node = of_get_child_by_name(spi->dev.of_node, "radio-a");

	radio = spi_controller_get_devdata(priv->radio_a_ctrl);
	radio->page = 2;
	radio->regs = REG_2_SPI_RADIO_A_DATA;
	radio->parent = spi;

	dev_info(&spi->dev, "registering radio A SPI\n");

	ret = devm_spi_register_controller(&spi->dev, priv->radio_a_ctrl);
	if (ret) {
		dev_err(&spi->dev, "radio A SPI register failed\n");
		goto err_radio_a_register;
	}

	/* radio B */

	priv->radio_b_ctrl = spi_alloc_master(&spi->dev, sizeof(*radio));
	if (!priv->radio_b_ctrl) {
		ret = -ENOMEM;
		goto err_radio_b_alloc;
	}

	sx1301_radio_setup(priv->radio_b_ctrl);
	priv->radio_b_ctrl->dev.of_node = of_get_child_by_name(spi->dev.of_node, "radio-b");

	radio = spi_controller_get_devdata(priv->radio_b_ctrl);
	radio->page = 2;
	radio->regs = REG_2_SPI_RADIO_B_DATA;
	radio->parent = spi;

	dev_info(&spi->dev, "registering radio B SPI\n");

	ret = devm_spi_register_controller(&spi->dev, priv->radio_b_ctrl);
	if (ret) {
		dev_err(&spi->dev, "radio B SPI register failed\n");
		goto err_radio_b_register;
	}

	dev_info(&spi->dev, "SX1301 module probed\n");

	return 0;
err_radio_b_register:
	spi_controller_put(priv->radio_b_ctrl);
err_radio_b_alloc:
err_radio_a_register:
	spi_controller_put(priv->radio_a_ctrl);
err_radio_a_alloc:
	free_loradev(netdev);
err_alloc_loradev:
err_version:
	return ret;
}

static int sx1301_remove(struct spi_device *spi)
{
	struct net_device *netdev = spi_get_drvdata(spi);

	//unregister_loradev(netdev);
	free_loradev(netdev);

	dev_info(&spi->dev, "SX1301 module removed\n");

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id sx1301_dt_ids[] = {
	{ .compatible = "semtech,sx1301" },
	{}
};
MODULE_DEVICE_TABLE(of, sx1301_dt_ids);
#endif

static struct spi_driver sx1301_spi_driver = {
	.driver = {
		.name = "sx1301",
		.of_match_table = of_match_ptr(sx1301_dt_ids),
	},
	.probe = sx1301_probe,
	.remove = sx1301_remove,
};

module_spi_driver(sx1301_spi_driver);

MODULE_DESCRIPTION("SX1301 SPI driver");
MODULE_AUTHOR("Andreas Färber <afaerber@suse.de>");
MODULE_LICENSE("GPL");
