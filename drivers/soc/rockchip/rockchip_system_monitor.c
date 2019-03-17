// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 Fuzhou Rockchip Electronics Co., Ltd
 * Author: Finley Xiao <finley.xiao@rock-chips.com>
 */

#include <dt-bindings/soc/rockchip-system-status.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/suspend.h>
#include <linux/thermal.h>
#include <linux/version.h>
#include <soc/rockchip/rockchip_system_monitor.h>
#include <soc/rockchip/rockchip-system-status.h>

#include "../../base/power/opp/opp.h"
#include "../../devfreq/governor.h"

#define VIDEO_1080P_SIZE	(1920 * 1080)
#define THERMAL_POLLING_DELAY	200 /* milliseconds */

#define devfreq_nb_to_monitor(nb) container_of(nb, struct monitor_dev_info, \
					       devfreq_nb)

struct video_info {
	unsigned int width;
	unsigned int height;
	unsigned int ishevc;
	unsigned int videoFramerate;
	unsigned int streamBitrate;
	struct list_head node;
};

struct system_monitor_attr {
	struct attribute attr;
	ssize_t (*show)(struct kobject *kobj, struct kobj_attribute *attr,
			char *buf);
	ssize_t (*store)(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t n);
};

struct system_monitor {
	struct device *dev;
	struct cpumask video_4k_offline_cpus;
	struct cpumask status_offline_cpus;
	struct cpumask temp_offline_cpus;
	struct cpumask offline_cpus;
	struct notifier_block status_nb;
	struct kobject *kobj;

	struct thermal_zone_device *tz;
	struct delayed_work thermal_work;
	int offline_cpus_temp;
	int temp_hysteresis;
	unsigned int delay;
	bool is_temp_offline;
};

static unsigned long system_status;
static unsigned long ref_count[32] = {0};

static DEFINE_MUTEX(system_status_mutex);
static DEFINE_MUTEX(video_info_mutex);
static DEFINE_MUTEX(cpu_on_off_mutex);

static DECLARE_RWSEM(mdev_list_sem);

static LIST_HEAD(video_info_list);
static LIST_HEAD(monitor_dev_list);
static struct system_monitor *system_monitor;
static atomic_t monitor_in_suspend;

static BLOCKING_NOTIFIER_HEAD(system_status_notifier_list);

int rockchip_register_system_status_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&system_status_notifier_list,
						nb);
}
EXPORT_SYMBOL(rockchip_register_system_status_notifier);

int rockchip_unregister_system_status_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&system_status_notifier_list,
						  nb);
}
EXPORT_SYMBOL(rockchip_unregister_system_status_notifier);

static int rockchip_system_status_notifier_call_chain(unsigned long val)
{
	int ret = blocking_notifier_call_chain(&system_status_notifier_list,
					       val, NULL);

	return notifier_to_errno(ret);
}

void rockchip_set_system_status(unsigned long status)
{
	unsigned long old_system_status;
	unsigned int single_status_offset;

	mutex_lock(&system_status_mutex);

	old_system_status = system_status;

	while (status) {
		single_status_offset = fls(status) - 1;
		status &= ~(1 << single_status_offset);
		if (ref_count[single_status_offset] == 0)
			system_status |= 1 << single_status_offset;
		ref_count[single_status_offset]++;
	}

	if (old_system_status != system_status)
		rockchip_system_status_notifier_call_chain(system_status);

	mutex_unlock(&system_status_mutex);
}
EXPORT_SYMBOL(rockchip_set_system_status);

void rockchip_clear_system_status(unsigned long status)
{
	unsigned long old_system_status;
	unsigned int single_status_offset;

	mutex_lock(&system_status_mutex);

	old_system_status = system_status;

	while (status) {
		single_status_offset = fls(status) - 1;
		status &= ~(1 << single_status_offset);
		if (ref_count[single_status_offset] == 0) {
			continue;
		} else {
			if (ref_count[single_status_offset] == 1)
				system_status &= ~(1 << single_status_offset);
			ref_count[single_status_offset]--;
		}
	}

	if (old_system_status != system_status)
		rockchip_system_status_notifier_call_chain(system_status);

	mutex_unlock(&system_status_mutex);
}
EXPORT_SYMBOL(rockchip_clear_system_status);

unsigned long rockchip_get_system_status(void)
{
	return system_status;
}
EXPORT_SYMBOL(rockchip_get_system_status);

int rockchip_add_system_status_interface(struct device *dev)
{
	if (!system_monitor || !system_monitor->kobj) {
		pr_err("failed to get system status kobj\n");
		return -EINVAL;
	}

	return __compat_only_sysfs_link_entry_to_kobj(&dev->kobj,
						      system_monitor->kobj,
						      "system_status");
}
EXPORT_SYMBOL(rockchip_add_system_status_interface);

static unsigned long rockchip_get_video_param(char **str)
{
	char *p;
	unsigned long val = 0;

	strsep(str, "=");
	p = strsep(str, ",");
	if (p) {
		if (kstrtoul(p, 10, &val))
			return 0;
	}

	return val;
}

/*
 * format:
 * 0,width=val,height=val,ishevc=val,videoFramerate=val,streamBitrate=val
 * 1,width=val,height=val,ishevc=val,videoFramerate=val,streamBitrate=val
 */
static struct video_info *rockchip_parse_video_info(const char *buf)
{
	struct video_info *video_info;
	const char *cp = buf;
	char *str;
	int ntokens = 0;

	while ((cp = strpbrk(cp + 1, ",")))
		ntokens++;
	if (ntokens != 5)
		return NULL;

	video_info = kzalloc(sizeof(*video_info), GFP_KERNEL);
	if (!video_info)
		return NULL;

	INIT_LIST_HEAD(&video_info->node);

	str = kstrdup(buf, GFP_KERNEL);
	strsep(&str, ",");
	video_info->width = rockchip_get_video_param(&str);
	video_info->height = rockchip_get_video_param(&str);
	video_info->ishevc = rockchip_get_video_param(&str);
	video_info->videoFramerate = rockchip_get_video_param(&str);
	video_info->streamBitrate = rockchip_get_video_param(&str);
	pr_debug("%c,width=%d,height=%d,ishevc=%d,videoFramerate=%d,streamBitrate=%d\n",
		 buf[0],
		 video_info->width,
		 video_info->height,
		 video_info->ishevc,
		 video_info->videoFramerate,
		 video_info->streamBitrate);
	kfree(str);

	return video_info;
}

static struct video_info *rockchip_find_video_info(const char *buf)
{
	struct video_info *info, *video_info;

	video_info = rockchip_parse_video_info(buf);

	if (!video_info)
		return NULL;

	mutex_lock(&video_info_mutex);
	list_for_each_entry(info, &video_info_list, node) {
		if (info->width == video_info->width &&
		    info->height == video_info->height &&
		    info->ishevc == video_info->ishevc &&
		    info->videoFramerate == video_info->videoFramerate &&
		    info->streamBitrate == video_info->streamBitrate) {
			mutex_unlock(&video_info_mutex);
			kfree(video_info);
			return info;
		}
	}

	mutex_unlock(&video_info_mutex);
	kfree(video_info);

	return NULL;
}

static void rockchip_add_video_info(struct video_info *video_info)
{
	if (video_info) {
		mutex_lock(&video_info_mutex);
		list_add(&video_info->node, &video_info_list);
		mutex_unlock(&video_info_mutex);
	}
}

static void rockchip_del_video_info(struct video_info *video_info)
{
	if (video_info) {
		mutex_lock(&video_info_mutex);
		list_del(&video_info->node);
		mutex_unlock(&video_info_mutex);
		kfree(video_info);
	}
}

static void rockchip_update_video_info(void)
{
	struct video_info *video_info;
	unsigned int max_res = 0, max_stream_bitrate = 0, res = 0;

	mutex_lock(&video_info_mutex);
	if (list_empty(&video_info_list)) {
		mutex_unlock(&video_info_mutex);
		rockchip_clear_system_status(SYS_STATUS_VIDEO);
		return;
	}

	list_for_each_entry(video_info, &video_info_list, node) {
		res = video_info->width * video_info->height;
		if (res > max_res)
			max_res = res;
		if (video_info->streamBitrate > max_stream_bitrate)
			max_stream_bitrate = video_info->streamBitrate;
	}
	mutex_unlock(&video_info_mutex);

	if (max_res <= VIDEO_1080P_SIZE) {
		rockchip_set_system_status(SYS_STATUS_VIDEO_1080P);
	} else {
		if (max_stream_bitrate == 10)
			rockchip_set_system_status(SYS_STATUS_VIDEO_4K_10B);
		else
			rockchip_set_system_status(SYS_STATUS_VIDEO_4K);
	}
}

void rockchip_update_system_status(const char *buf)
{
	struct video_info *video_info;

	if (!buf)
		return;

	switch (buf[0]) {
	case '0':
		/* clear video flag */
		video_info = rockchip_find_video_info(buf);
		if (video_info) {
			rockchip_del_video_info(video_info);
			rockchip_update_video_info();
		}
		break;
	case '1':
		/* set video flag */
		video_info = rockchip_parse_video_info(buf);
		if (video_info) {
			rockchip_add_video_info(video_info);
			rockchip_update_video_info();
		}
		break;
	case 'L':
		/* clear low power flag */
		rockchip_clear_system_status(SYS_STATUS_LOW_POWER);
		break;
	case 'l':
		/* set low power flag */
		rockchip_set_system_status(SYS_STATUS_LOW_POWER);
		break;
	case 'p':
		/* set performance flag */
		rockchip_set_system_status(SYS_STATUS_PERFORMANCE);
		break;
	case 'n':
		/* clear performance flag */
		rockchip_clear_system_status(SYS_STATUS_PERFORMANCE);
		break;
	default:
		break;
	}
}
EXPORT_SYMBOL(rockchip_update_system_status);

static ssize_t status_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	unsigned int status = rockchip_get_system_status();

	return sprintf(buf, "0x%x\n", status);
}

static ssize_t status_store(struct kobject *kobj, struct kobj_attribute *attr,
			    const char *buf, size_t n)
{
	if (!n)
		return -EINVAL;

	rockchip_update_system_status(buf);

	return n;
}

static struct system_monitor_attr status =
	__ATTR(system_status, 0644, status_show, status_store);

static int rockchip_get_adjust_volt_table(struct device_node *np,
					  char *porp_name,
					  struct volt_adjust_table **table)
{
	struct volt_adjust_table *volt_table;
	const struct property *prop;
	int count, i;

	prop = of_find_property(np, porp_name, NULL);
	if (!prop)
		return -EINVAL;

	if (!prop->value)
		return -ENODATA;

	count = of_property_count_u32_elems(np, porp_name);
	if (count < 0)
		return -EINVAL;

	if (count % 3)
		return -EINVAL;

	volt_table = kzalloc(sizeof(*volt_table) * (count / 3 + 1), GFP_KERNEL);
	if (!volt_table)
		return -ENOMEM;

	for (i = 0; i < count / 3; i++) {
		of_property_read_u32_index(np, porp_name, 3 * i,
					   &volt_table[i].min);
		of_property_read_u32_index(np, porp_name, 3 * i + 1,
					   &volt_table[i].max);
		of_property_read_u32_index(np, porp_name, 3 * i + 2,
					   &volt_table[i].volt);
	}
	volt_table[i].min = 0;
	volt_table[i].max = 0;
	volt_table[i].volt = INT_MAX;

	*table = volt_table;

	return 0;
}

static int rockchip_get_low_temp_volt(struct monitor_dev_info *info,
				      unsigned long rate, int *delta_volt)
{
	int i, ret = -EINVAL;
	unsigned int _rate = (unsigned int)(rate / 1000000);

	if (!info->low_temp_adjust_table)
		return ret;

	for (i = 0; info->low_temp_adjust_table[i].volt != INT_MAX; i++) {
		if (_rate >= info->low_temp_adjust_table[i].min &&
		    _rate <= info->low_temp_adjust_table[i].max) {
			*delta_volt = info->low_temp_adjust_table[i].volt;
			ret = 0;
		}
	}

	return ret;
}

static int rockchip_init_temp_opp_table(struct monitor_dev_info *info)
{
	struct device *dev = info->dev;
	struct dev_pm_opp *opp;
	int delta_volt = 0;
	int i, max_count, ret = 0;
	unsigned long rate;
	bool reach_max_volt = false;
	bool reach_high_temp_max_volt = false;

	max_count = dev_pm_opp_get_opp_count(dev);
	if (max_count <= 0) {
		ret = max_count ? max_count : -ENODATA;
		goto out;
	}
	info->opp_table = kzalloc(sizeof(*info->opp_table) * max_count,
				  GFP_KERNEL);
	if (!info->opp_table) {
		ret = -ENOMEM;
		goto out;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 11, 0)
	rcu_read_lock();
#endif
	for (i = 0, rate = 0; i < max_count; i++, rate++) {
		/* find next rate */
		opp = dev_pm_opp_find_freq_ceil(dev, &rate);
		if (IS_ERR(opp)) {
			ret = PTR_ERR(opp);
			kfree(info->opp_table);
			goto unlock;
		}
		info->opp_table[i].rate = opp->rate;
		info->opp_table[i].volt = opp->u_volt;
		info->opp_table[i].max_volt = opp->u_volt_max;

		if (opp->u_volt <= info->high_temp_max_volt) {
			if (!reach_high_temp_max_volt)
				info->high_limit = opp->rate;
			if (opp->u_volt == info->high_temp_max_volt)
				reach_high_temp_max_volt = true;
		}

		if (rockchip_get_low_temp_volt(info, opp->rate, &delta_volt))
			delta_volt = 0;
		if ((opp->u_volt + delta_volt) <= info->max_volt) {
			info->opp_table[i].low_temp_volt =
				opp->u_volt + delta_volt;
			if (info->opp_table[i].low_temp_volt <
			    info->low_temp_min_volt)
				info->opp_table[i].low_temp_volt =
					info->low_temp_min_volt;
			if (!reach_max_volt)
				info->low_limit = opp->rate;
			if (info->opp_table[i].low_temp_volt == info->max_volt)
				reach_max_volt = true;
		} else {
			info->opp_table[i].low_temp_volt = info->max_volt;
		}
		dev_dbg(dev, "rate=%lu, volt=%lu, low_temp_volt=%lu\n",
			info->opp_table[i].rate, info->opp_table[i].volt,
			info->opp_table[i].low_temp_volt);
	}
	if (info->low_limit == opp->rate)
		info->low_limit = 0;
	if (info->high_limit == opp->rate)
		info->high_limit = 0;
unlock:
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 11, 0)
	rcu_read_unlock();
#endif
out:
	return ret;
}

static int monitor_device_parse_dt(struct device *dev,
				   struct monitor_dev_info *info)
{
	struct device_node *np;
	unsigned long high_temp_max_freq;
	int ret = 0;
	u32 value;

	np = of_parse_phandle(dev->of_node, "operating-points-v2", 0);
	if (!np)
		return -EINVAL;

	if (of_property_read_u32(np, "rockchip,max-volt", &value))
		info->max_volt = ULONG_MAX;
	else
		info->max_volt = value;
	of_property_read_u32(np, "rockchip,temp-hysteresis",
			     &info->temp_hysteresis);
	if (of_property_read_u32(np, "rockchip,low-temp", &info->low_temp))
		info->low_temp = INT_MIN;
	rockchip_get_adjust_volt_table(np, "rockchip,low-temp-adjust-volt",
				       &info->low_temp_adjust_table);
	if (!of_property_read_u32(np, "rockchip,low-temp-min-volt", &value))
		info->low_temp_min_volt = value;
	if (of_property_read_u32(np, "rockchip,high-temp", &info->high_temp))
		info->high_temp = INT_MAX;
	if (of_property_read_u32(np, "rockchip,high-temp-max-volt",
				 &value))
		info->high_temp_max_volt = ULONG_MAX;
	else
		info->high_temp_max_volt = value;
	rockchip_init_temp_opp_table(info);
	if (!of_property_read_u32(np, "rockchip,high-temp-max-freq", &value)) {
		high_temp_max_freq = value * 1000;
		if (info->high_limit)
			info->high_limit = min(high_temp_max_freq,
					       info->high_limit);
		else
			info->high_limit = high_temp_max_freq;
	}
	dev_info(dev, "l=%d h=%d hyst=%d l_limit=%lu h_limit=%lu\n",
		 info->low_temp, info->high_temp, info->temp_hysteresis,
		 info->low_limit, info->high_limit);

	if ((info->low_temp + info->temp_hysteresis) > info->high_temp) {
		dev_err(dev, "Invalid temperature, low=%d high=%d hyst=%d\n",
			info->low_temp, info->high_temp,
			info->temp_hysteresis);
		ret = -EINVAL;
		goto out;
	}
	if (!info->low_temp_adjust_table && !info->low_temp_min_volt &&
	    !info->low_limit && !info->high_limit) {
		ret = -EINVAL;
		goto out;
	}
	if (info->low_temp_adjust_table || info->low_temp_min_volt)
		info->is_low_temp_enabled = true;

out:
	of_node_put(np);
	if (ret) {
		kfree(info->low_temp_adjust_table);
		kfree(info->opp_table);
	}

	return ret;
}

int rockchip_monitor_cpu_low_temp_adjust(struct monitor_dev_info *info,
					 bool is_low)
{
	struct device *dev = info->dev;
	struct cpufreq_policy *policy;
	unsigned int cpu = cpumask_any(&info->devp->allowed_cpus);

	if (info->low_limit) {
		if (is_low)
			info->wide_temp_limit = info->low_limit;
		else
			info->wide_temp_limit = 0;
		cpufreq_update_policy(cpu);
	}

	policy = cpufreq_cpu_get(cpu);
	if (!policy)
		return -ENODEV;
	down_write(&policy->rwsem);
	dev_pm_opp_check_rate_volt(dev, false);
	up_write(&policy->rwsem);
	cpufreq_cpu_put(policy);

	return 0;
}
EXPORT_SYMBOL(rockchip_monitor_cpu_low_temp_adjust);

int rockchip_monitor_cpu_high_temp_adjust(struct monitor_dev_info *info,
					  bool is_high)
{
	unsigned int cpu = cpumask_any(&info->devp->allowed_cpus);

	if (info->high_limit) {
		if (is_high)
			info->wide_temp_limit = info->high_limit;
		else
			info->wide_temp_limit = 0;
		cpufreq_update_policy(cpu);
	}

	return 0;
}
EXPORT_SYMBOL(rockchip_monitor_cpu_high_temp_adjust);

int rockchip_monitor_dev_low_temp_adjust(struct monitor_dev_info *info,
					 bool is_low)
{
	struct devfreq *df;

	if (info->low_limit) {
		if (is_low)
			info->wide_temp_limit = info->low_limit;
		else
			info->wide_temp_limit = 0;
	}

	if (info->devp && info->devp->data) {
		df = (struct devfreq *)info->devp->data;
		mutex_lock(&df->lock);
		update_devfreq(df);
		mutex_unlock(&df->lock);
	}

	return 0;
}
EXPORT_SYMBOL(rockchip_monitor_dev_low_temp_adjust);

int rockchip_monitor_dev_high_temp_adjust(struct monitor_dev_info *info,
					  bool is_high)
{
	struct devfreq *df;

	if (info->high_limit) {
		if (is_high)
			info->wide_temp_limit = info->high_limit;
		else
			info->wide_temp_limit = 0;
	}

	if (info->devp && info->devp->data) {
		df = (struct devfreq *)info->devp->data;
		mutex_lock(&df->lock);
		update_devfreq(df);
		mutex_unlock(&df->lock);
	}

	return 0;
}
EXPORT_SYMBOL(rockchip_monitor_dev_high_temp_adjust);

static int rockchip_adjust_low_temp_opp_volt(struct monitor_dev_info *info,
					     bool is_low_temp)
{
	struct device *dev = info->dev;
	struct dev_pm_opp *opp;
	unsigned long rate;
	int i, count, ret = 0;

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 11, 0)
	rcu_read_lock();
#endif
	count = dev_pm_opp_get_opp_count(dev);
	if (count <= 0) {
		ret = count ? count : -ENODATA;
		goto out;
	}

	for (i = 0, rate = 0; i < count; i++, rate++) {
		/* find next rate */
		opp = dev_pm_opp_find_freq_ceil(dev, &rate);
		if (IS_ERR(opp)) {
			ret = PTR_ERR(opp);
			goto out;
		}
		if (is_low_temp) {
			if (opp->u_volt_max < info->opp_table[i].low_temp_volt)
				opp->u_volt_max =
					info->opp_table[i].low_temp_volt;
			opp->u_volt = info->opp_table[i].low_temp_volt;
			opp->u_volt_min = opp->u_volt;
		} else {
			opp->u_volt_min = info->opp_table[i].volt;
			opp->u_volt = opp->u_volt_min;
			opp->u_volt_max = info->opp_table[i].max_volt;
		}
	}

out:
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 11, 0)
	rcu_read_unlock();
#endif
	return ret;
}

static void rockchip_low_temp_adjust(struct monitor_dev_info *info,
				     bool is_low)
{
	struct monitor_dev_profile *devp = info->devp;
	int ret = 0;

	dev_dbg(info->dev, "low_temp %d\n", is_low);

	if (info->opp_table)
		rockchip_adjust_low_temp_opp_volt(info, is_low);

	if (devp->low_temp_adjust)
		ret = devp->low_temp_adjust(info, is_low);
	if (!ret)
		info->is_low_temp = is_low;
}

static void rockchip_high_temp_adjust(struct monitor_dev_info *info,
				      bool is_high)
{
	struct monitor_dev_profile *devp = info->devp;
	int ret = 0;

	dev_dbg(info->dev, "high_temp %d\n", is_high);
	if (devp->high_temp_adjust)
		ret = devp->high_temp_adjust(info, is_high);
	if (!ret)
		info->is_high_temp = is_high;
}

int rockchip_monitor_suspend_low_temp_adjust(struct monitor_dev_info *info)
{
	if (!info || !info->is_low_temp_enabled)
		return 0;

	if (info->is_high_temp)
		rockchip_high_temp_adjust(info, false);
	if (!info->is_low_temp)
		rockchip_low_temp_adjust(info, true);

	return 0;
}
EXPORT_SYMBOL(rockchip_monitor_suspend_low_temp_adjust);

static int
rockchip_system_monitor_wide_temp_adjust(struct monitor_dev_info *info,
					 int temp)
{
	if (temp < info->low_temp) {
		if (info->is_high_temp)
			rockchip_high_temp_adjust(info, false);
		if (!info->is_low_temp)
			rockchip_low_temp_adjust(info, true);
	} else if (temp > (info->low_temp + info->temp_hysteresis)) {
		if (info->is_low_temp)
			rockchip_low_temp_adjust(info, false);
	}

	if (temp > info->high_temp) {
		if (info->is_low_temp)
			rockchip_low_temp_adjust(info, false);
		if (!info->is_high_temp)
			rockchip_high_temp_adjust(info, true);
	} else if (temp < (info->high_temp - info->temp_hysteresis)) {
		if (info->is_high_temp)
			rockchip_high_temp_adjust(info, false);
	}

	return 0;
}

static void
rockchip_system_monitor_wide_temp_init(struct monitor_dev_info *info)
{
	int ret, temp;

	ret = thermal_zone_get_temp(system_monitor->tz, &temp);
	if (ret || temp == THERMAL_TEMP_INVALID) {
		dev_err(info->dev,
			"failed to read out thermal zone (%d)\n", ret);
		return;
	}
	if (temp < info->low_temp) {
		if (info->opp_table)
			rockchip_adjust_low_temp_opp_volt(info, true);
		info->wide_temp_limit = info->low_limit;
	} else if (temp > info->high_temp) {
		info->wide_temp_limit = info->high_limit;
	}
}

static int system_monitor_devfreq_notifier_call(struct notifier_block *nb,
						unsigned long event,
						void *data)
{
	struct monitor_dev_info *info = devfreq_nb_to_monitor(nb);
	struct devfreq_policy *policy = data;

	if (event != DEVFREQ_ADJUST)
		return NOTIFY_DONE;

	if (info->wide_temp_limit && info->wide_temp_limit < policy->max)
		devfreq_verify_within_limits(policy, 0, info->wide_temp_limit);

	return NOTIFY_OK;
}

struct monitor_dev_info *
rockchip_system_monitor_register(struct device *dev,
				 struct monitor_dev_profile *devp)
{
	struct monitor_dev_info *info;
	struct devfreq *devfreq;
	int ret;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return ERR_PTR(-ENOMEM);
	info->dev = dev;
	info->devp = devp;

	ret = monitor_device_parse_dt(dev, info);
	if (ret)
		goto free_info;

	if (info->devp->type == MONITOR_TPYE_DEV) {
		info->devfreq_nb.notifier_call =
			system_monitor_devfreq_notifier_call;
		devfreq = (struct devfreq *)info->devp->data;
		devm_devfreq_register_notifier(dev, devfreq, &info->devfreq_nb,
					       DEVFREQ_POLICY_NOTIFIER);
	}

	rockchip_system_monitor_wide_temp_init(info);

	down_write(&mdev_list_sem);
	list_add(&info->node, &monitor_dev_list);
	up_write(&mdev_list_sem);

	return info;

free_info:
	kfree(info);
	return ERR_PTR(-EINVAL);
}
EXPORT_SYMBOL_GPL(rockchip_system_monitor_register);

void rockchip_system_monitor_unregister(struct monitor_dev_info *info)
{
	struct devfreq *devfreq;

	if (!info)
		return;

	down_write(&mdev_list_sem);
	list_del(&info->node);
	up_write(&mdev_list_sem);

	devfreq = (struct devfreq *)info->devp->data;
	if (info->devp->type == MONITOR_TPYE_DEV)
		devm_devfreq_unregister_notifier(info->dev, devfreq,
						 &info->devfreq_nb,
						 DEVFREQ_TRANSITION_NOTIFIER);

	kfree(info->devp);
	kfree(info->low_temp_adjust_table);
	kfree(info->opp_table);
	kfree(info);
}
EXPORT_SYMBOL(rockchip_system_monitor_unregister);

static int rockchip_system_monitor_parse_dt(struct system_monitor *monitor)
{
	struct device_node *np = monitor->dev->of_node;
	const char *tz_name, *buf = NULL;

	if (of_property_read_string(np, "rockchip,video-4k-offline-cpus", &buf))
		cpumask_clear(&system_monitor->video_4k_offline_cpus);
	else
		cpulist_parse(buf, &monitor->video_4k_offline_cpus);

	if (of_property_read_string(np, "rockchip,thermal-zone", &tz_name))
		goto out;
	monitor->tz = thermal_zone_get_zone_by_name(tz_name);
	if (IS_ERR(monitor->tz)) {
		monitor->tz = NULL;
		goto out;
	}
	if (of_property_read_u32(np, "rockchip,polling-delay",
				 &monitor->delay))
		monitor->delay = THERMAL_POLLING_DELAY;

	if (of_property_read_string(np, "rockchip,temp-offline-cpus",
				    &buf))
		cpumask_clear(&system_monitor->temp_offline_cpus);
	else
		cpulist_parse(buf, &system_monitor->temp_offline_cpus);

	if (of_property_read_u32(np, "rockchip,offline-cpu-temp",
				 &system_monitor->offline_cpus_temp))
		system_monitor->offline_cpus_temp = INT_MAX;
	of_property_read_u32(np, "rockchip,temp-hysteresis",
			     &system_monitor->temp_hysteresis);
out:
	return 0;
}

static void rockchip_system_monitor_cpu_on_off(void)
{
#ifdef CONFIG_HOTPLUG_CPU
	struct cpumask online_cpus, offline_cpus;
	unsigned int cpu;

	mutex_lock(&cpu_on_off_mutex);

	cpumask_clear(&offline_cpus);
	if (system_monitor->is_temp_offline) {
		cpumask_or(&offline_cpus, &system_monitor->status_offline_cpus,
			   &system_monitor->temp_offline_cpus);
	} else {
		cpumask_copy(&offline_cpus,
			     &system_monitor->status_offline_cpus);
	}
	if (cpumask_equal(&offline_cpus, &system_monitor->offline_cpus))
		goto out;
	cpumask_copy(&system_monitor->offline_cpus, &offline_cpus);
	for_each_cpu(cpu, &system_monitor->offline_cpus) {
		if (cpu_online(cpu))
			cpu_down(cpu);
	}

	cpumask_clear(&online_cpus);
	cpumask_andnot(&online_cpus, cpu_possible_mask,
		       &system_monitor->offline_cpus);
	cpumask_xor(&online_cpus, cpu_online_mask, &online_cpus);
	if (cpumask_empty(&online_cpus))
		goto out;
	for_each_cpu(cpu, &online_cpus)
		cpu_up(cpu);

out:
	mutex_unlock(&cpu_on_off_mutex);
#endif
}

static void rockchip_system_monitor_temp_cpu_on_off(int temp)
{
	bool is_temp_offline;

	if (cpumask_empty(&system_monitor->temp_offline_cpus))
		return;

	if (temp > system_monitor->offline_cpus_temp)
		is_temp_offline = true;
	else if (temp < system_monitor->offline_cpus_temp -
		 system_monitor->temp_hysteresis)
		is_temp_offline = false;
	else
		return;

	if (system_monitor->is_temp_offline == is_temp_offline)
		return;
	system_monitor->is_temp_offline = is_temp_offline;
	rockchip_system_monitor_cpu_on_off();
}

static void rockchip_system_monitor_thermal_update(void)
{
	int temp, ret;
	struct monitor_dev_info *info;

	ret = thermal_zone_get_temp(system_monitor->tz, &temp);
	if (ret || temp == THERMAL_TEMP_INVALID)
		goto out;

	dev_dbg(system_monitor->dev, "temperature=%d\n", temp);

	down_read(&mdev_list_sem);
	list_for_each_entry(info, &monitor_dev_list, node)
		rockchip_system_monitor_wide_temp_adjust(info, temp);
	up_read(&mdev_list_sem);

	rockchip_system_monitor_temp_cpu_on_off(temp);

out:
	mod_delayed_work(system_freezable_wq, &system_monitor->thermal_work,
			 msecs_to_jiffies(system_monitor->delay));
}

static void rockchip_system_monitor_thermal_check(struct work_struct *work)
{
	if (atomic_read(&monitor_in_suspend))
		return;

	rockchip_system_monitor_thermal_update();
}

static void rockchip_system_status_cpu_on_off(unsigned long status)
{
	struct cpumask offline_cpus;

	if (cpumask_empty(&system_monitor->video_4k_offline_cpus))
		return;

	cpumask_clear(&offline_cpus);
	if (status & SYS_STATUS_VIDEO_4K)
		cpumask_copy(&offline_cpus,
			     &system_monitor->video_4k_offline_cpus);
	if (cpumask_equal(&offline_cpus, &system_monitor->status_offline_cpus))
		return;
	cpumask_copy(&system_monitor->status_offline_cpus, &offline_cpus);
	rockchip_system_monitor_cpu_on_off();
}

static int rockchip_system_status_notifier(struct notifier_block *nb,
					   unsigned long status,
					   void *ptr)
{
	rockchip_system_status_cpu_on_off(status);

	return NOTIFY_OK;
}

static int monitor_pm_notify(struct notifier_block *nb,
			     unsigned long mode, void *_unused)
{
	switch (mode) {
	case PM_HIBERNATION_PREPARE:
	case PM_RESTORE_PREPARE:
	case PM_SUSPEND_PREPARE:
		atomic_set(&monitor_in_suspend, 1);
		break;
	case PM_POST_HIBERNATION:
	case PM_POST_RESTORE:
	case PM_POST_SUSPEND:
		rockchip_system_monitor_thermal_update();
		atomic_set(&monitor_in_suspend, 0);
		break;
	default:
		break;
	}
	return 0;
}

static struct notifier_block monitor_pm_nb = {
	.notifier_call = monitor_pm_notify,
};

static int rockchip_monitor_cpufreq_policy_notifier(struct notifier_block *nb,
						    unsigned long event,
						    void *data)
{
	struct monitor_dev_info *info;
	struct cpufreq_policy *policy = data;
	int cpu = policy->cpu;
	unsigned int target_freq;

	if (event != CPUFREQ_ADJUST)
		return NOTIFY_OK;

	down_read(&mdev_list_sem);
	list_for_each_entry(info, &monitor_dev_list, node) {
		if (info->devp->type != MONITOR_TPYE_CPU)
			continue;
		if (!cpumask_test_cpu(cpu, &info->devp->allowed_cpus))
			continue;
		if (info->wide_temp_limit) {
			target_freq = info->wide_temp_limit / 1000;
			if (target_freq < policy->max)
				cpufreq_verify_within_limits(policy, 0,
							     target_freq);
		}
	}
	up_read(&mdev_list_sem);

	return NOTIFY_OK;
}

static struct notifier_block rockchip_monitor_cpufreq_policy_nb = {
	.notifier_call = rockchip_monitor_cpufreq_policy_notifier,
};

static int rockchip_system_monitor_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	system_monitor = devm_kzalloc(dev, sizeof(struct system_monitor),
				      GFP_KERNEL);
	if (!system_monitor)
		return -ENOMEM;
	system_monitor->dev = dev;

	system_monitor->kobj = kobject_create_and_add("system_monitor", NULL);
	if (!system_monitor->kobj)
		return -ENOMEM;
	if (sysfs_create_file(system_monitor->kobj, &status.attr))
		dev_err(dev, "failed to create system status sysfs\n");

	cpumask_clear(&system_monitor->status_offline_cpus);
	cpumask_clear(&system_monitor->offline_cpus);

	rockchip_system_monitor_parse_dt(system_monitor);
	if (system_monitor->tz) {
		INIT_DELAYED_WORK(&system_monitor->thermal_work,
				  rockchip_system_monitor_thermal_check);
		mod_delayed_work(system_freezable_wq,
				 &system_monitor->thermal_work,
				 msecs_to_jiffies(system_monitor->delay));
	}

	system_monitor->status_nb.notifier_call =
		rockchip_system_status_notifier;
	rockchip_register_system_status_notifier(&system_monitor->status_nb);

	if (register_pm_notifier(&monitor_pm_nb))
		dev_err(dev, "failed to register suspend notifier\n");

	cpufreq_register_notifier(&rockchip_monitor_cpufreq_policy_nb,
				  CPUFREQ_POLICY_NOTIFIER);

	return 0;
}

static const struct of_device_id rockchip_system_monitor_of_match[] = {
	{
		.compatible = "rockchip,system-monitor",
	},
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, rockchip_system_monitor_of_match);

static struct platform_driver rockchip_system_monitor_driver = {
	.probe	= rockchip_system_monitor_probe,
	.driver = {
		.name	= "rockchip-system-monitor",
		.of_match_table = rockchip_system_monitor_of_match,
	},
};
module_platform_driver(rockchip_system_monitor_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Finley Xiao <finley.xiao@rock-chips.com>");
MODULE_DESCRIPTION("rockchip system monitor driver");
