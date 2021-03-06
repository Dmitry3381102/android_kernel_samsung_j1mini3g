/*
 * Copyright (C) 2013 Samsung Electronics. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/wakelock.h>
#include <linux/regulator/consumer.h>
#include <linux/power_supply.h>

#include "sensors_core.h"
#include "sx9306_wifi_reg.h"

#define VENDOR_NAME		"SEMTECH"
#define MODEL_NAME               "SX9306_WIFI"
#define MODULE_NAME              "grip_sensor_wifi"

#define I2C_M_WR                 0 /* for i2c Write */
#define I2c_M_RD                 1 /* for i2c Read */

#define IDLE                     0
#define ACTIVE                   1

#define SX9306_MODE_SLEEP        0
#define SX9306_MODE_NORMAL       1

#define MAIN_SENSOR              0
#define REF_SENSOR               1
#define CSX_STATUS_REG           SX9306_TCHCMPSTAT_TCHSTAT0_FLAG
#define RAW_DATA_BLOCK_SIZE	(SX9306_REGOFFSETLSB - SX9306_REGUSEMSB + 1)
#define DEFAULT_NORMAL_TOUCH_THRESHOLD  17

/* CS0, CS1, CS2, CS3 */
#define TOTAL_BOTTON_COUNT       1
#define ENABLE_CSX               (1 << MAIN_SENSOR)

#define DIFF_READ_NUM            10
#define GRIP_LOG_TIME            30 /* sec */

#define IRQ_PROCESS_CONDITION   (SX9306_IRQSTAT_TOUCH_FLAG	\
				| SX9306_IRQSTAT_RELEASE_FLAG)

struct sx9306_p {
	struct i2c_client *client;
	struct input_dev *input;
	struct device *factory_device;
	struct delayed_work init_work;
	struct delayed_work irq_work;
	struct delayed_work debug_work;
	struct wake_lock grip_wake_lock;
	struct mutex read_mutex;
	bool skip_data;
	bool check_usb;
	u8 normal_th;
	u8 normal_th_buf;
	int irq;
	int gpio_nirq;
	int state;
	int debug_count;
	int diff_avg;
	int diff_cnt;
	s32 capmain;
	s16 useful;
	s16 avg;
	s16 diff;
	u16 offset;
	u16 freq;
	atomic_t enable;
};


static int check_ta_state(void)
{
	static struct power_supply *psy;
	union power_supply_propval ret = {0, };

	if (psy == NULL) {
		psy = power_supply_get_by_name("battery");
		if (psy == NULL) {
			SENSOR_ERR("failed to get ps battery\n");
			return -EINVAL;
		}
	}

	psy->get_property(psy, POWER_SUPPLY_PROP_ONLINE, &ret);

	return ret.intval;
}

static int sx9306_wifi_get_nirq_state(struct sx9306_p *data)
{
	return gpio_get_value_cansleep(data->gpio_nirq);
}

static int sx9306_wifi_i2c_write(struct sx9306_p *data, u8 reg_addr, u8 buf)
{
	int ret;
	struct i2c_msg msg;
	unsigned char w_buf[2];

	w_buf[0] = reg_addr;
	w_buf[1] = buf;

	msg.addr = data->client->addr;
	msg.flags = I2C_M_WR;
	msg.len = 2;
	msg.buf = (char *)w_buf;

	ret = i2c_transfer(data->client->adapter, &msg, 1);
	if (ret < 0)
		SENSOR_ERR("i2c write error %d\n", ret);

	return ret;
}

static int sx9306_wifi_i2c_read(struct sx9306_p *data, u8 reg_addr, u8 *buf)
{
	int ret;
	struct i2c_msg msg[2];

	msg[0].addr = data->client->addr;
	msg[0].flags = I2C_M_WR;
	msg[0].len = 1;
	msg[0].buf = &reg_addr;

	msg[1].addr = data->client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].len = 1;
	msg[1].buf = buf;

	ret = i2c_transfer(data->client->adapter, msg, 2);
	if (ret < 0)
		SENSOR_ERR("i2c read error %d\n", ret);

	return ret;
}

static int sx9306_wifi_i2c_read_block(struct sx9306_p *data, u8 reg_addr,
	u8 *buf, u8 buf_size)
{
	int ret;
	struct i2c_msg msg[2];

	msg[0].addr = data->client->addr;
	msg[0].flags = I2C_M_WR;
	msg[0].len = 1;
	msg[0].buf = &reg_addr;

	msg[1].addr = data->client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].len = buf_size;
	msg[1].buf = buf;

	ret = i2c_transfer(data->client->adapter, msg, 2);
	if (ret < 0)
		SENSOR_ERR("i2c read error %d\n", ret);

	return ret;
}

static u8 sx9306_wifi_read_irqstate(struct sx9306_p *data)
{
	u8 val = 0;

	if (sx9306_wifi_i2c_read(data, SX9306_IRQSTAT_REG, &val) >= 0)
		return val;

	return 0;
}

static void sx9306_wifi_initialize_register(struct sx9306_p *data)
{
	u8 val = 0;
	int idx;

	for (idx = 0; idx < (sizeof(setup_reg) >> 1); idx++) {
		sx9306_wifi_i2c_write(data, setup_reg[idx].reg,
						setup_reg[idx].val);
		SENSOR_INFO("Write Reg: 0x%x Value: 0x%x\n",
			setup_reg[idx].reg, setup_reg[idx].val);

		sx9306_wifi_i2c_read(data, setup_reg[idx].reg, &val);
		SENSOR_INFO("Read Reg: 0x%x Value: 0x%x\n\n",
			setup_reg[idx].reg, val);
	}
}

static void sx9306_wifi_initialize_chip(struct sx9306_p *data)
{
	int cnt = 0;

	while ((sx9306_wifi_get_nirq_state(data) == 0) && (cnt++ < 10)) {
		sx9306_wifi_read_irqstate(data);
		msleep(20);
	}

	if (cnt >= 10)
		SENSOR_ERR("s/w reset fail(%d)\n", cnt);

	sx9306_wifi_initialize_register(data);
}

static int sx9306_wifi_set_offset_calibration(struct sx9306_p *data)
{
	int ret = 0;

	ret = sx9306_wifi_i2c_write(data, SX9306_IRQSTAT_REG, 0xFF);

	return ret;
}

static void send_event(struct sx9306_p *data, u8 state)
{
	data->normal_th = data->normal_th_buf;

	if (state == ACTIVE) {
		data->state = ACTIVE;
		sx9306_wifi_i2c_write(data, SX9306_CPS_CTRL6_REG,
							data->normal_th);
		SENSOR_INFO("button touched\n");
	} else {
		data->state = IDLE;
		sx9306_wifi_i2c_write(data, SX9306_CPS_CTRL6_REG,
							data->normal_th);
		SENSOR_INFO("button released\n");
	}

	if (data->skip_data == true)
		return;

	if (state == ACTIVE)
		input_report_rel(data->input, REL_MISC, 1);
	else
		input_report_rel(data->input, REL_MISC, 2);

	input_sync(data->input);
}

static void sx9306_wifi_display_data_reg(struct sx9306_p *data)
{
	u8 val, reg;

	sx9306_wifi_i2c_write(data, SX9306_REGSENSORSELECT, MAIN_SENSOR);
	for (reg = SX9306_REGUSEMSB; reg <= SX9306_REGOFFSETLSB; reg++) {
		sx9306_wifi_i2c_read(data, reg, &val);
		SENSOR_INFO("Register(0x%2x) data(0x%2x)\n", reg, val);
	}
}

static void sx9306_wifi_get_data(struct sx9306_p *data)
{
	u8 ms_byte = 0;
	u8 ls_byte = 0;
	u8 buf[RAW_DATA_BLOCK_SIZE];
	s32 gain = 1 << ((setup_reg[3].val >> 5) & 0x03);

	mutex_lock(&data->read_mutex);
	sx9306_wifi_i2c_write(data, SX9306_REGSENSORSELECT, MAIN_SENSOR);
	sx9306_wifi_i2c_read_block(data, SX9306_REGUSEMSB,
		&buf[0], RAW_DATA_BLOCK_SIZE);

	data->useful = (s16)((s32)buf[0] << 8) | ((s32)buf[1]);
	data->avg = (s16)((s32)buf[2] << 8) | ((s32)buf[3]);
	data->offset = ((u16)buf[6] << 8) | ((u16)buf[7]);

	ms_byte = (u8)(data->offset >> 6);
	ls_byte = (u8)(data->offset - (((u16)ms_byte) << 6));

	data->capmain = 2 * (((s32)ms_byte * 3600) + ((s32)ls_byte * 225)) +
		(((s32)data->useful * 50000) / (gain * 65536));

	data->diff = (data->useful - data->avg) >> 4;

	SENSOR_INFO("cm=%d,uf=%d,av=%d,df=%d,Of=%u (%d)\n",
		data->capmain, data->useful, data->avg,
		data->diff, data->offset, data->state);
	mutex_unlock(&data->read_mutex);
}

static int sx9306_wifi_set_mode(struct sx9306_p *data, unsigned char mode)
{
	int ret = -EINVAL;

	if (mode == SX9306_MODE_SLEEP) {
		ret = sx9306_wifi_i2c_write(data, SX9306_CPS_CTRL0_REG,
			setup_reg[1].val);
	} else if (mode == SX9306_MODE_NORMAL) {
		ret = sx9306_wifi_i2c_write(data, SX9306_CPS_CTRL0_REG,
			setup_reg[1].val | ENABLE_CSX);
		msleep(20);

		sx9306_wifi_set_offset_calibration(data);
		msleep(400);
	}

	SENSOR_INFO("change the mode : %u\n", mode);
	return ret;
}

static void sx9306_wifi_set_enable(struct sx9306_p *data, u8 enable)
{
	u8 status = 0;

	if (enable == ON) {
		sx9306_wifi_i2c_read(data, SX9306_TCHCMPSTAT_REG, &status);
		SENSOR_INFO("enable(0x%x)\n", status);
		data->diff_avg = 0;
		data->diff_cnt = 0;
		sx9306_wifi_get_data(data);

		if (data->skip_data == true) {
			input_report_rel(data->input, REL_MISC, 2);
			input_sync(data->input);
		} else if (status & (CSX_STATUS_REG << MAIN_SENSOR)) {
			send_event(data, ACTIVE);
		} else {
			send_event(data, IDLE);
		}

		/* make sure no interrupts are pending since enabling irq
		 * will only work on next falling edge */
		sx9306_wifi_read_irqstate(data);

		enable_irq(data->irq);
		enable_irq_wake(data->irq);
	} else {
		SENSOR_INFO("disable\n");
		disable_irq(data->irq);
		disable_irq_wake(data->irq);
	}
}

static void sx9306_wifi_set_debug_work(struct sx9306_p *data, u8 enable)
{
	if (enable == ON) {
		data->debug_count = 0;
		data->check_usb = false;
		schedule_delayed_work(&data->debug_work,
			msecs_to_jiffies(1000));
	} else {
		cancel_delayed_work_sync(&data->debug_work);
	}
}

static ssize_t sx9306_wifi_get_offset_calibration_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	u8 val = 0;
	struct sx9306_p *data = dev_get_drvdata(dev);

	sx9306_wifi_i2c_read(data, SX9306_IRQSTAT_REG, &val);

	return snprintf(buf, PAGE_SIZE, "%d\n", val);
}

static ssize_t sx9306_wifi_set_offset_calibration_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int val = 0;
	struct sx9306_p *data = dev_get_drvdata(dev);

	if (kstrtoint(buf, 10, &val)) {
		SENSOR_ERR("Invalid Argument\n");
		return -EINVAL;
	}

	if (val)
		sx9306_wifi_set_offset_calibration(data);

	return count;
}

static ssize_t sx9306_wifi_register_write_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int regist = 0, val = 0;
	struct sx9306_p *data = dev_get_drvdata(dev);

	if (sscanf(buf, "%d,%d", &regist, &val) != 2) {
		SENSOR_ERR("The number of data are wrong\n");
		return -EINVAL;
	}

	sx9306_wifi_i2c_write(data, (unsigned char)regist, (unsigned char)val);
	SENSOR_INFO("Register(0x%2x) data(0x%2x)\n", regist, val);

	return count;
}

static ssize_t sx9306_wifi_register_read_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int regist = 0;
	unsigned char val = 0;
	struct sx9306_p *data = dev_get_drvdata(dev);

	if (sscanf(buf, "%d", &regist) != 1) {
		SENSOR_ERR("The number of data are wrong\n");
		return -EINVAL;
	}

	sx9306_wifi_i2c_read(data, (unsigned char)regist, &val);
	SENSOR_INFO("Register(0x%2x) data(0x%2x)\n", regist, val);

	return count;
}

static ssize_t sx9306_wifi_read_data_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct sx9306_p *data = dev_get_drvdata(dev);

	sx9306_wifi_display_data_reg(data);

	return snprintf(buf, PAGE_SIZE, "%d\n", 0);
}

static ssize_t sx9306_wifi_sw_reset_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct sx9306_p *data = dev_get_drvdata(dev);

	SENSOR_INFO("\n");
	sx9306_wifi_set_offset_calibration(data);
	msleep(400);
	sx9306_wifi_get_data(data);
	return snprintf(buf, PAGE_SIZE, "%d\n", 0);
}

static ssize_t sx9306_wifi_freq_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	u8 reg = setup_reg[3].val & 0xE7;
	unsigned long val;
	struct sx9306_p *data = dev_get_drvdata(dev);

	if (kstrtoul(buf, 10, &val)) {
		SENSOR_ERR("Invalid Argument\n");
		return count;
	}

	data->freq = (u16)val & 0x03;
	reg = (((u8)data->freq << 3) | reg) & 0xff;
	sx9306_wifi_i2c_write(data, SX9306_CPS_CTRL2_REG, reg);

	SENSOR_INFO("Freq : 0x%x\n", data->freq);

	return count;
}

static ssize_t sx9306_wifi_freq_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct sx9306_p *data = dev_get_drvdata(dev);

	SENSOR_INFO("Freq : 0x%x\n", data->freq);

	return snprintf(buf, PAGE_SIZE, "%u\n", data->freq);
}

static ssize_t sx9306_wifi_vendor_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n", VENDOR_NAME);
}

static ssize_t sx9306_wifi_name_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n", MODEL_NAME);
}

static ssize_t sx9306_wifi_touch_mode_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "1\n");
}

static ssize_t sx9306_wifi_raw_data_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	static int sum;
	struct sx9306_p *data = dev_get_drvdata(dev);

	sx9306_wifi_get_data(data);
	if (data->diff_cnt == 0)
		sum = data->diff;
	else
		sum += data->diff;

	if (++data->diff_cnt >= DIFF_READ_NUM) {
		data->diff_avg = sum / DIFF_READ_NUM;
		data->diff_cnt = 0;
	}

	return snprintf(buf, PAGE_SIZE, "%d,%d,%u,%d,%d\n", data->capmain,
		data->useful, data->offset, data->diff, data->avg);
}

static ssize_t sx9306_wifi_threshold_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	/* It's for init touch */
	return snprintf(buf, PAGE_SIZE, "0\n");
}

static ssize_t sx9306_wifi_threshold_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	return count;
}

static ssize_t sx9306_wifi_normal_threshold_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct sx9306_p *data = dev_get_drvdata(dev);
	u16 thresh_temp = 0, hysteresis = 0;
	u16 thresh_table[32] = {0, 20, 40, 60, 80, 100, 120, 140, 160, 180,
				200, 220, 240, 260, 280, 300, 350, 400, 450,
				500, 600, 700, 800, 900, 1000, 1100, 1200, 1300,
				1400, 1500, 1600, 1700};

	thresh_temp = data->normal_th & 0x1f;
	thresh_temp = thresh_table[thresh_temp];

	/* CTRL7 */
	hysteresis = (setup_reg[8].val >> 4) & 0x3;

	switch (hysteresis) {
	case 0x00:
		hysteresis = 32;
		break;
	case 0x01:
		hysteresis = 64;
		break;
	case 0x02:
		hysteresis = 128;
		break;
	case 0x03:
		hysteresis = 256;
		break;
	default:
		break;
	}

	return snprintf(buf, PAGE_SIZE, "%d,%d\n", thresh_temp + hysteresis,
			thresh_temp - hysteresis);
}

static ssize_t sx9306_wifi_normal_threshold_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned long val;
	struct sx9306_p *data = dev_get_drvdata(dev);

	/* It's for normal touch */
	if (kstrtoul(buf, 10, &val)) {
		SENSOR_ERR("Invalid Argument\n");
		return -EINVAL;
	}

	SENSOR_INFO("normal threshold %lu\n", val);
	data->normal_th_buf = data->normal_th = (u8)val;

	return count;
}

static ssize_t sx9306_wifi_onoff_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct sx9306_p *data = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%u\n", !data->skip_data);
}

static ssize_t sx9306_wifi_onoff_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	u8 val;
	int ret;
	struct sx9306_p *data = dev_get_drvdata(dev);

	ret = kstrtou8(buf, 2, &val);
	if (ret) {
		SENSOR_ERR("Invalid Argument\n");
		return ret;
	}

	if (val == 0) {
		data->skip_data = true;
		if (atomic_read(&data->enable) == ON) {
			data->state = IDLE;
			input_report_rel(data->input, REL_MISC, 2);
			input_sync(data->input);
		}
	} else {
		data->skip_data = false;
	}

	SENSOR_INFO("%u\n", val);
	return count;
}

static ssize_t sx9306_wifi_calibration_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "2,0,0\n");
}

static ssize_t sx9306_wifi_calibration_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	return count;
}

static ssize_t sx9306_wifi_gain_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int ret;

	switch ((setup_reg[3].val >> 5) & 0x03) {
	case 0x00:
		ret = snprintf(buf, PAGE_SIZE, "x1\n");
		break;
	case 0x01:
		ret = snprintf(buf, PAGE_SIZE, "x2\n");
		break;
	case 0x02:
		ret = snprintf(buf, PAGE_SIZE, "x4\n");
		break;
	default:
		ret = snprintf(buf, PAGE_SIZE, "x8\n");
		break;
	}

	return ret;
}

static ssize_t sx9306_wifi_range_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int ret;

	switch (setup_reg[2].val & 0x03) {
	case 0x00:
		ret = snprintf(buf, PAGE_SIZE, "Large\n");
		break;
	case 0x01:
		ret = snprintf(buf, PAGE_SIZE, "Medium Large\n");
		break;
	case 0x02:
		ret = snprintf(buf, PAGE_SIZE, "Medium Small\n");
		break;
	default:
		ret = snprintf(buf, PAGE_SIZE, "Small\n");
		break;
	}

	return ret;
}

static ssize_t sx9306_wifi_diff_avg_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct sx9306_p *data = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%d\n", data->diff_avg);
}

static DEVICE_ATTR(menual_calibrate, S_IRUGO | S_IWUSR | S_IWGRP,
			sx9306_wifi_get_offset_calibration_show,
			sx9306_wifi_set_offset_calibration_store);
static DEVICE_ATTR(register_write, S_IWUSR | S_IWGRP,
			NULL, sx9306_wifi_register_write_store);
static DEVICE_ATTR(register_read, S_IWUSR | S_IWGRP,
			NULL, sx9306_wifi_register_read_store);
static DEVICE_ATTR(readback, S_IRUGO, sx9306_wifi_read_data_show, NULL);
static DEVICE_ATTR(reset, S_IRUGO, sx9306_wifi_sw_reset_show, NULL);

static DEVICE_ATTR(name, S_IRUGO, sx9306_wifi_name_show, NULL);
static DEVICE_ATTR(vendor, S_IRUGO, sx9306_wifi_vendor_show, NULL);
static DEVICE_ATTR(gain, S_IRUGO, sx9306_wifi_gain_show, NULL);
static DEVICE_ATTR(range, S_IRUGO, sx9306_wifi_range_show, NULL);
static DEVICE_ATTR(mode, S_IRUGO, sx9306_wifi_touch_mode_show, NULL);
static DEVICE_ATTR(raw_data, S_IRUGO, sx9306_wifi_raw_data_show, NULL);
static DEVICE_ATTR(diff_avg, S_IRUGO, sx9306_wifi_diff_avg_show, NULL);
static DEVICE_ATTR(calibration, S_IRUGO | S_IWUSR | S_IWGRP,
			sx9306_wifi_calibration_show,
			sx9306_wifi_calibration_store);
static DEVICE_ATTR(onoff, S_IRUGO | S_IWUSR | S_IWGRP,
			sx9306_wifi_onoff_show,
			sx9306_wifi_onoff_store);
static DEVICE_ATTR(threshold, S_IRUGO | S_IWUSR | S_IWGRP,
			sx9306_wifi_threshold_show,
			sx9306_wifi_threshold_store);
static DEVICE_ATTR(normal_threshold, S_IRUGO | S_IWUSR | S_IWGRP,
			sx9306_wifi_normal_threshold_show,
			sx9306_wifi_normal_threshold_store);
static DEVICE_ATTR(freq, S_IRUGO | S_IWUSR | S_IWGRP,
			sx9306_wifi_freq_show,
			sx9306_wifi_freq_store);

static struct device_attribute *sensor_attrs[] = {
	&dev_attr_menual_calibrate,
	&dev_attr_register_write,
	&dev_attr_register_read,
	&dev_attr_readback,
	&dev_attr_reset,
	&dev_attr_name,
	&dev_attr_vendor,
	&dev_attr_gain,
	&dev_attr_range,
	&dev_attr_mode,
	&dev_attr_diff_avg,
	&dev_attr_raw_data,
	&dev_attr_threshold,
	&dev_attr_normal_threshold,
	&dev_attr_onoff,
	&dev_attr_calibration,
	&dev_attr_freq,
	NULL,
};

/*****************************************************************************/
static ssize_t sx9306_wifi_enable_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	u8 enable;
	int ret;
	struct sx9306_p *data = dev_get_drvdata(dev);
	int pre_enable = atomic_read(&data->enable);

	ret = kstrtou8(buf, 2, &enable);
	if (ret) {
		SENSOR_ERR("Invalid Argument\n");
		return ret;
	}

	SENSOR_INFO("new_value = %u old_value = %d\n", enable, pre_enable);

	if (pre_enable == enable)
		return size;

	atomic_set(&data->enable, enable);
	sx9306_wifi_set_enable(data, enable);

	return size;
}

static ssize_t sx9306_wifi_enable_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct sx9306_p *data = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&data->enable));
}

static ssize_t sx9306_wifi_flush_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	u8 enable;
	int ret;
	struct sx9306_p *data = dev_get_drvdata(dev);

	ret = kstrtou8(buf, 2, &enable);
	if (ret) {
		SENSOR_ERR("Invalid Argument\n");
		return ret;
	}

	if (enable == 1) {
		input_report_rel(data->input, REL_MAX, 1);
		input_sync(data->input);
	}

	return size;
}

static DEVICE_ATTR(enable, S_IRUGO | S_IWUSR | S_IWGRP,
		sx9306_wifi_enable_show, sx9306_wifi_enable_store);
static DEVICE_ATTR(flush, S_IWUSR | S_IWGRP,
		NULL, sx9306_wifi_flush_store);

static struct attribute *sx9306_wifi_attributes[] = {
	&dev_attr_enable.attr,
	&dev_attr_flush.attr,
	NULL
};

static struct attribute_group sx9306_wifi_attribute_group = {
	.attrs = sx9306_wifi_attributes
};

static void sx9306_wifi_touch_process(struct sx9306_p *data, u8 flag)
{
	u8 status = 0;

	sx9306_wifi_i2c_read(data, SX9306_TCHCMPSTAT_REG, &status);
	SENSOR_INFO("(0x%x)\n", status);
	sx9306_wifi_get_data(data);

	if (data->state == IDLE) {
		if (status & (CSX_STATUS_REG << MAIN_SENSOR))
			send_event(data, ACTIVE);
		else
			SENSOR_INFO("already released.\n");
	} else {
		if (!(status & (CSX_STATUS_REG << MAIN_SENSOR)))
			send_event(data, IDLE);
		else
			SENSOR_INFO("still touched\n");
	}
}

static void sx9306_wifi_process_interrupt(struct sx9306_p *data)
{
	u8 flag = 0;

	/* since we are not in an interrupt don't need to disable irq. */
	flag = sx9306_wifi_read_irqstate(data);

	if (flag & IRQ_PROCESS_CONDITION)
		sx9306_wifi_touch_process(data, flag);
}

static void sx9306_wifi_init_work_func(struct work_struct *work)
{
	struct sx9306_p *data = container_of((struct delayed_work *)work,
		struct sx9306_p, init_work);

	sx9306_wifi_initialize_chip(data);
	sx9306_wifi_i2c_write(data, SX9306_CPS_CTRL6_REG, data->normal_th);
	sx9306_wifi_set_mode(data, SX9306_MODE_NORMAL);

	/* make sure no interrupts are pending since enabling irq
	 * will only work on next falling edge */
	sx9306_wifi_read_irqstate(data);
}

static void sx9306_wifi_irq_work_func(struct work_struct *work)
{
	struct sx9306_p *data = container_of((struct delayed_work *)work,
		struct sx9306_p, irq_work);

	sx9306_wifi_process_interrupt(data);
}

static void sx9306_wifi_debug_work_func(struct work_struct *work)
{
	struct sx9306_p *data = container_of((struct delayed_work *)work,
		struct sx9306_p, debug_work);

	if (atomic_read(&data->enable) == ON) {
		if (data->debug_count >= GRIP_LOG_TIME) {
			sx9306_wifi_get_data(data);
			data->debug_count = 0;
		} else {
			data->debug_count++;
		}
	}

	if (check_ta_state() > 1) {
		data->check_usb = true;
	} else if (data->check_usb == true) {
		data->check_usb = false;
		sx9306_wifi_set_offset_calibration(data);
		SENSOR_INFO("TA is removed\n");
	}

	schedule_delayed_work(&data->debug_work, msecs_to_jiffies(1000));
}

static irqreturn_t sx9306_wifi_interrupt_thread(int irq, void *pdata)
{
	struct sx9306_p *data = pdata;

	if (sx9306_wifi_get_nirq_state(data) == 1) {
		SENSOR_ERR("nirq read high\n");
	} else {
		wake_lock_timeout(&data->grip_wake_lock, 3 * HZ);
		schedule_delayed_work(&data->irq_work, msecs_to_jiffies(100));
	}

	return IRQ_HANDLED;
}

static int sx9306_wifi_input_init(struct sx9306_p *data)
{
	int ret = 0;
	struct input_dev *dev = NULL;

	/* Create the input device */
	dev = input_allocate_device();
	if (!dev)
		return -ENOMEM;

	dev->name = MODULE_NAME;
	dev->id.bustype = BUS_I2C;

	input_set_capability(dev, EV_REL, REL_MISC);
	input_set_capability(dev, EV_REL, REL_MAX);
	input_set_drvdata(dev, data);

	ret = input_register_device(dev);
	if (ret < 0) {
		input_free_device(dev);
		return ret;
	}

	ret = sensors_create_symlink(&dev->dev.kobj, dev->name);
	if (ret < 0) {
		input_unregister_device(dev);
		return ret;
	}

	ret = sysfs_create_group(&dev->dev.kobj, &sx9306_wifi_attribute_group);
	if (ret < 0) {
		sensors_remove_symlink(&dev->dev.kobj, dev->name);
		input_unregister_device(dev);
		return ret;
	}

	/* save the input pointer and finish initialization */
	data->input = dev;

	return 0;
}

static int sx9306_wifi_setup_pin(struct sx9306_p *data)
{
	int ret;

	ret = gpio_request(data->gpio_nirq, "SX9306_WIFI_nIRQ");
	if (ret < 0) {
		SENSOR_ERR("gpio %d request failed (%d)\n",
			data->gpio_nirq, ret);
		return ret;
	}

	ret = gpio_direction_input(data->gpio_nirq);
	if (ret < 0) {
		SENSOR_ERR("failed to set gpio %d(%d)\n",
			data->gpio_nirq, ret);
		gpio_free(data->gpio_nirq);
		return ret;
	}

	return 0;
}

static void sx9306_wifi_initialize_variable(struct sx9306_p *data)
{
	data->state = IDLE;
	data->skip_data = false;
	data->check_usb = false;
	data->freq = (setup_reg[3].val >> 3) & 0x03;
	data->debug_count = 0;
	atomic_set(&data->enable, OFF);
	data->normal_th = (u8)CONFIG_SENSORS_SX9306_WIFI_NORMAL_TOUCH_THRESHOLD;
	data->normal_th_buf = data->normal_th;

	SENSOR_INFO("Normal Touch Threshold : %u\n", data->normal_th);
}

static int sx9306_wifi_parse_dt(struct sx9306_p *data, struct device *dev)
{
	struct device_node *node = dev->of_node;
	enum of_gpio_flags flags;

	if (node == NULL)
		return -ENODEV;

	data->gpio_nirq = of_get_named_gpio_flags(node,
		"sx9306_wifi-i2c,nirq-gpio", 0, &flags);
	if (data->gpio_nirq < 0) {
		SENSOR_ERR("get gpio_nirq error\n");
		return -ENODEV;
	}

	return 0;
}

#if defined(CONFIG_SENSORS_SX9306_REGULATOR_ONOFF)
int sx9306_wifi_regulator_on(struct sx9306_p *data, bool onoff)
{
	struct regulator *reg_vdd, reg_ldo;
	int ret = 0;

	reg_vdd = devm_regulator_get(&data->client->dev, "vdd");
	if (!reg_vdd) {
		SENSOR_ERR("regulator pointer null vdd, rc=%d\n", ret);
		return ret;
	}
	ret = regulator_set_voltage(reg_vdd, 2850000, 2850000);
	if (ret) {
		SENSOR_ERR("set voltage failed on vdd, rc=%d\n", ret);
		return ret;
	}

	reg_ldo = devm_regulator_get(&data->client->dev, "vdd_ldo");
	if (!reg_ldo) {
		SENSOR_ERR("devm_regulator_get for vdd_ldo failed\n");
		return 0;
	}

	if (onoff) {
		ret = regulator_enable(reg_vdd);
		if (ret) {
			SENSOR_ERR("Failed to enable regulator vdd.\n");
			return ret;
		}

		ret = regulator_enable(reg_ldo);
		if (ret) {
			SENSOR_ERR("Failed to enable regulator ldo.\n");
			return ret;
		}
	} else {
		ret = regulator_disable(reg_vdd);
		if (ret) {
			SENSOR_ERR("Failed to disable regulator vdd.\n");
			return ret;
		}

		ret = regulator_disable(reg_ldo);
		if (ret) {
			SENSOR_ERR("Failed to disable regulator ldo.\n");
			return ret;
		}

	}

	msleep(30);
	return 0;
}
#endif

static int sx9306_wifi_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct sx9306_p *data = NULL;
	int ret = -ENODEV;

	SENSOR_INFO("start\n");
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		SENSOR_ERR("i2c_check_functionality error\n");
		goto exit;
	}

	data = kzalloc(sizeof(struct sx9306_p), GFP_KERNEL);
	if (data == NULL) {
		SENSOR_ERR("kzalloc error\n");
		ret = -ENOMEM;
		goto exit;
	}

	i2c_set_clientdata(client, data);
	data->client = client;
	data->factory_device = &client->dev;
	wake_lock_init(&data->grip_wake_lock, WAKE_LOCK_SUSPEND,
		"grip_wifi_wake_lock");
	mutex_init(&data->read_mutex);

	ret = sx9306_wifi_parse_dt(data, &client->dev);
	if (ret < 0) {
		SENSOR_ERR("of_node error\n");
		goto exit_of_node;
	}

	ret = sx9306_wifi_setup_pin(data);
	if (ret) {
		SENSOR_ERR("could not setup pin\n");
		goto exit_setup_pin;
	}

#if defined(CONFIG_SENSORS_SX9306_REGULATOR_ONOFF)
	sx9306_wifi_regulator_on(data, true);
#endif

	/* read chip id */
	ret = sx9306_wifi_i2c_write(data, SX9306_SOFTRESET_REG,
						SX9306_SOFTRESET);
	if (ret < 0) {
		SENSOR_ERR("chip reset failed %d\n", ret);
		goto exit_chip_reset;
	}

	sx9306_wifi_initialize_variable(data);

	INIT_DELAYED_WORK(&data->init_work, sx9306_wifi_init_work_func);
	INIT_DELAYED_WORK(&data->irq_work, sx9306_wifi_irq_work_func);
	INIT_DELAYED_WORK(&data->debug_work, sx9306_wifi_debug_work_func);

	data->irq = gpio_to_irq(data->gpio_nirq);
	ret = request_threaded_irq(data->irq, NULL,
			sx9306_wifi_interrupt_thread,
			IRQF_TRIGGER_FALLING | IRQF_ONESHOT | IRQF_NO_SUSPEND,
			"sx9306_wifi_irq", data);
	if (ret < 0) {
		SENSOR_ERR("failed to set request_threaded_irq %d (%d)\n",
			data->irq, ret);
		goto exit_request_threaded_irq;
	}
	disable_irq(data->irq);
	schedule_delayed_work(&data->init_work, msecs_to_jiffies(300));

	ret = sx9306_wifi_input_init(data);
	if (ret < 0)
		goto exit_input_init;

	ret = sensors_register(data->factory_device,
		data, sensor_attrs, MODULE_NAME);
	if (ret) {
		SENSOR_ERR("cound not register grip_sensor(%d)\n", ret);
		goto grip_sensor_register_failed;
	}

	SENSOR_INFO("done\n");

	return 0;

grip_sensor_register_failed:
	sysfs_remove_group(&data->input->dev.kobj,
		&sx9306_wifi_attribute_group);
	sensors_remove_symlink(&data->input->dev.kobj, data->input->name);
	input_unregister_device(data->input);
exit_input_init:
	free_irq(data->irq, data);
exit_request_threaded_irq:
	gpio_free(data->gpio_nirq);
exit_chip_reset:
#if defined(CONFIG_SENSORS_SX9306_REGULATOR_ONOFF)
	sx9306_wifi_regulator_on(data, false);
#endif
exit_setup_pin:
exit_of_node:
	mutex_destroy(&data->read_mutex);
	wake_lock_destroy(&data->grip_wake_lock);
	kfree(data);
exit:
	SENSOR_ERR("fail\n");
	return ret;
}

static int sx9306_wifi_remove(struct i2c_client *client)
{
	struct sx9306_p *data = (struct sx9306_p *)i2c_get_clientdata(client);

	if (atomic_read(&data->enable) == ON)
		sx9306_wifi_set_enable(data, OFF);

	sx9306_wifi_set_mode(data, SX9306_MODE_SLEEP);

	cancel_delayed_work_sync(&data->init_work);
	cancel_delayed_work_sync(&data->irq_work);
	cancel_delayed_work_sync(&data->debug_work);
	free_irq(data->irq, data);
	gpio_free(data->gpio_nirq);

	wake_lock_destroy(&data->grip_wake_lock);
	sensors_unregister(data->factory_device, sensor_attrs);
	sensors_remove_symlink(&data->input->dev.kobj, data->input->name);

	sysfs_remove_group(&data->input->dev.kobj,
		&sx9306_wifi_attribute_group);
	input_unregister_device(data->input);
	mutex_destroy(&data->read_mutex);

	kfree(data);

	return 0;
}

static int sx9306_wifi_suspend(struct device *dev)
{
	struct sx9306_p *data = dev_get_drvdata(dev);

	SENSOR_INFO("\n");
	sx9306_wifi_set_debug_work(data, OFF);

	return 0;
}

static int sx9306_wifi_resume(struct device *dev)
{
	struct sx9306_p *data = dev_get_drvdata(dev);

	SENSOR_INFO("\n");
	sx9306_wifi_set_debug_work(data, ON);

	return 0;
}

static void sx9306_wifi_shutdown(struct i2c_client *client)
{
	struct sx9306_p *data = i2c_get_clientdata(client);

	SENSOR_INFO("\n");
	sx9306_wifi_set_debug_work(data, OFF);
	if (atomic_read(&data->enable) == ON)
		sx9306_wifi_set_enable(data, OFF);
	sx9306_wifi_set_mode(data, SX9306_MODE_SLEEP);
}

static struct of_device_id sx9306_match_table[] = {
	{ .compatible = "sx9306_wifi-i2c",},
	{},
};

static const struct i2c_device_id sx9306_wifi_id[] = {
	{ "sx9306_match_table", 0 },
	{ }
};

static const struct dev_pm_ops sx9306_wifi_pm_ops = {
	.suspend = sx9306_wifi_suspend,
	.resume = sx9306_wifi_resume,
};

static struct i2c_driver sx9306_wifi_driver = {
	.driver = {
		.name	= MODEL_NAME,
		.owner	= THIS_MODULE,
		.of_match_table = sx9306_match_table,
		.pm = &sx9306_wifi_pm_ops
	},
	.probe		= sx9306_wifi_probe,
	.remove		= sx9306_wifi_remove,
	.shutdown	= sx9306_wifi_shutdown,
	.id_table	= sx9306_wifi_id,
};

static int __init sx9306_wifi_init(void)
{
	return i2c_add_driver(&sx9306_wifi_driver);
}

static void __exit sx9306_wifi_exit(void)
{
	i2c_del_driver(&sx9306_wifi_driver);
}

module_init(sx9306_wifi_init);
module_exit(sx9306_wifi_exit);

MODULE_DESCRIPTION("Semtech Corp. SX9306 Capacitive Touch Controller Driver");
MODULE_AUTHOR("Samsung Electronics");
MODULE_LICENSE("GPL");
