/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright 2024 Google LLC */


#define pr_fmt(fmt) "smblite-shim-plug-debounce:%s: " fmt, __func__

#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/of.h>
#include <linux/workqueue.h>
#include "misc/gvotable.h"
#include "smblite-shim-plug-debounce.h"
#include "smblite-shim.h"

#define PLUG_DEBOUNCE_VOTER "PLUG_DEBOUNCE_VOTER"

struct smblite_shim_plug_debounce {
	struct gvotable_election *awake_votable;
	struct gvotable_election *fake_psy_present_votable;
	struct gvotable_election *fake_psy_online_votable;
	struct delayed_work unplug_debounce_work;
	struct notifier_block plugin_nb;
	u16 unplug_debounce_ms;
};

static ssize_t unplug_debounce_ms_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct power_supply *psy = container_of(dev, struct power_supply, dev);
	struct smblite_shim *shim = power_supply_get_drvdata(psy);
	u16 debounce_ms;
	int ret;

	ret = kstrtou16(buf, 10, &debounce_ms);
	if (ret < 0)
		return ret;

	shim->debounce->unplug_debounce_ms = debounce_ms;

	return count;
}

static ssize_t unplug_debounce_ms_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct power_supply *psy = container_of(dev, struct power_supply, dev);
	struct smblite_shim *shim = power_supply_get_drvdata(psy);

	return sysfs_emit(buf, "%u\n", shim->debounce->unplug_debounce_ms);
}
static const DEVICE_ATTR_RW(unplug_debounce_ms);

static void vote_psy_fake_plugged(struct smblite_shim_plug_debounce *debounce,
				bool plugged)
{
	gvotable_cast_bool_vote(debounce->fake_psy_present_votable,
				PLUG_DEBOUNCE_VOTER, plugged);
	gvotable_cast_bool_vote(debounce->fake_psy_online_votable,
				PLUG_DEBOUNCE_VOTER, plugged);
}

static void unplug_debounce_work(struct work_struct *work)
{
	struct smblite_shim_plug_debounce *debounce =
		container_of(to_delayed_work(work),
			struct smblite_shim_plug_debounce,
			unplug_debounce_work);

	vote_psy_fake_plugged(debounce, false);
	gvotable_cast_bool_vote(debounce->awake_votable,
				PLUG_DEBOUNCE_VOTER, false);
}

static int plugin_notify(struct notifier_block *nb, unsigned long plugged,
			void *unused)
{
	struct smblite_shim_plug_debounce *debounce
		= container_of(nb, struct smblite_shim_plug_debounce,
				plugin_nb);
	bool is_plugged =
		((enum smblite_shim_plug_sts)plugged == SMBLITE_SHIM_PLUGGED_IN);
	u16 unplug_debounce_ms = debounce->unplug_debounce_ms;

	cancel_delayed_work(&debounce->unplug_debounce_work);

	vote_psy_fake_plugged(debounce, (unplug_debounce_ms != 0));

	if (!is_plugged && (unplug_debounce_ms != 0)) {
		/*
		 * If unplugged, stay awake for the delayed work
		 * to trigger at the right time
		 */
		gvotable_cast_bool_vote(debounce->awake_votable,
				PLUG_DEBOUNCE_VOTER, true);
		schedule_delayed_work(&debounce->unplug_debounce_work,
				msecs_to_jiffies(unplug_debounce_ms));
	}

	return NOTIFY_OK;
}

struct smblite_shim_plug_debounce *
smblite_shim_plug_debounce_init(struct smblite_shim *shim)
{
	int ret;
	u16 unplug_debounce_ms;
	struct gvotable_election *awake_votable;
	struct gvotable_election *fake_psy_present_votable;
	struct gvotable_election *fake_psy_online_votable;
	struct smblite_shim_plug_debounce *debounce;

	ret = of_property_read_u16(shim->chg->dev->of_node,
				"google,unplug-debounce-ms",
				&unplug_debounce_ms);
	if (ret < 0)
		return NULL;

	fake_psy_present_votable =
		gvotable_election_get_handle("SHIM_FAKE_PRES");
	if (!fake_psy_present_votable) {
		pr_err("Could not find fake-present votable\b");
		return NULL;
	}

	fake_psy_online_votable =
		gvotable_election_get_handle("SHIM_FAKE_OLN");
	if (!fake_psy_online_votable) {
		pr_err("Could not find fake-online votable\n");
		return NULL;
	}

	awake_votable = gvotable_election_get_handle("AWAKE");
	if (!awake_votable) {
		pr_err("Could not find awake votable\n");
		return NULL;
	}

	debounce = devm_kzalloc(shim->chg->dev, sizeof(*debounce), GFP_KERNEL);
	INIT_DELAYED_WORK(&debounce->unplug_debounce_work,
			unplug_debounce_work);

	if (!debounce)
		return NULL;

	debounce->awake_votable = awake_votable;
	debounce->fake_psy_present_votable = fake_psy_present_votable;
	debounce->fake_psy_online_votable = fake_psy_online_votable;

	debounce->unplug_debounce_ms = unplug_debounce_ms;

	debounce->plugin_nb.notifier_call = plugin_notify;
	ret = smblite_shim_plugin_register_notifier(shim, &debounce->plugin_nb);
	if (ret < 0) {
		return NULL;
	}

	device_create_file(&shim->psy->dev, &dev_attr_unplug_debounce_ms);

	return debounce;
}

void smblite_shim_plug_debounce_deinit(struct smblite_shim *shim)
{
	struct smblite_shim_plug_debounce *debounce = shim->debounce;
	if (!debounce)
		return;

	if (shim->psy)
		device_remove_file(&shim->psy->dev,
				&dev_attr_unplug_debounce_ms);

	smblite_shim_plugin_unregister_notifier(shim, &debounce->plugin_nb);
	gvotable_cast_bool_vote(debounce->awake_votable,
				PLUG_DEBOUNCE_VOTER, false);
	vote_psy_fake_plugged(debounce, false);
}
