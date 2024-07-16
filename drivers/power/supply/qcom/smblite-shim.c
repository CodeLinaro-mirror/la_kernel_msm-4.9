/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright 2023 Google LLC */

#define pr_fmt(fmt) "smblite-shim:%s: " fmt, __func__

#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pmic-voter.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>
#include "smblite-shim.h"

struct icl_check_data {
	struct gvotable_election *fake_psy_online_votable;
	int icl_min;
};

struct vote_match_data {
	const char *reason;
	bool matches;
};

static struct power_supply_desc usb_psy_desc;

static int vote_reason_match(void *data, const char *reason, void *vote)
{
	struct vote_match_data *match_data = (struct vote_match_data *)data;
	match_data->matches =
		strncmp(match_data->reason, reason,
			GVOTABLE_MAX_REASON_LEN) == 0;

	/* Return -1 to stop processing the rest of the reasons */
	return match_data->matches ? -1 : 0;
}

static int real_usb_icl_vote_cb(void *data, const char *reason, void *vote)
{
	struct icl_check_data *icl_check_data = (struct icl_check_data *)data;
	struct vote_match_data match_data = {
		.reason = reason,
		.matches = false
	};
	int icl = GVOTABLE_PTR_TO_INT(vote);

	gvotable_election_for_each(icl_check_data->fake_psy_online_votable,
				vote_reason_match, &match_data);

	/* Vote reason is not participating in requesting faking PSY online,
	 * probably legitimate
	 */
	if (!match_data.matches) {
		icl_check_data->icl_min = min(icl_check_data->icl_min, icl);
	}

	return 0;
}

/*
 * "real ICL" means the USB ICL excluding the active vote reasons for the faked
 * "online" status votable, as those reasons might have set an ICL in the PMIC
 * that needs to be masked from upper software stack layers.
 */
static int get_real_icl(struct smblite_shim *shim)
{
	int ret;
	struct smb_charger *chg = shim->chg;
	union power_supply_propval present;
	const struct power_supply_desc *real_usb_desc = chg->usb_psy->desc;
	struct gvotable_election *icl_votable;
	struct icl_check_data icl_check_data = {
		.fake_psy_online_votable = shim->fake_psy_online_votable,
		.icl_min = __INT_MAX__
	};

	icl_votable = gvotable_election_get_handle("USB_ICL");

	ret = real_usb_desc->get_property(chg->usb_psy,
					POWER_SUPPLY_PROP_PRESENT, &present);

	if ((ret != 0) || !present.intval || !icl_votable) {
		return 0;
	}

	gvotable_election_for_each(icl_votable, real_usb_icl_vote_cb,
				&icl_check_data);

	return icl_check_data.icl_min;
}

static int smblite_shim_usb_get_prop(struct power_supply *psy,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	struct smblite_shim *shim = power_supply_get_drvdata(psy);
	struct smb_charger *chg = shim->chg;
	const struct power_supply_desc *real_usb_desc = chg->usb_psy->desc;
	bool online_vote;
	int real_icl;

	online_vote =
		gvotable_get_current_int_vote(shim->fake_psy_online_votable);

	if (online_vote)
		real_icl = get_real_icl(shim);

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		/* We'll actually only fake the online property if there was no
		 * real reason for the ICL to be 0. If a real vote reason set
		 * 0 ICL, then the PSY should already be reporting offline and
		 * we'll leave things as-is.
		 */
		if (online_vote && real_icl > 0) {
			val->intval = true;
			return 0;
		}
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		if (online_vote) {
			val->intval = real_icl;
			return 0;
		}
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		val->intval = gvotable_get_current_int_vote(shim->vmax_votable);
		if (val->intval > 0) {
			return 0;
		}
		break;
	default:
		break;
	}
	return real_usb_desc->get_property(chg->usb_psy, psp, val);
}

static int smblite_shim_usb_set_prop(struct power_supply *psy,
				enum power_supply_property psp,
				const union power_supply_propval *val)
{
	struct smblite_shim *shim = power_supply_get_drvdata(psy);
	struct smb_charger *chg = shim->chg;
	const struct power_supply_desc *real_usb_desc = chg->usb_psy->desc;

	switch (psp) {
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		vote(chg->usb_icl_votable, EXTERNAL_CONTROL_VOTER, true,
		val->intval);
		vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, false, 0);
		vote(chg->usb_icl_votable, EXTERNAL_CONTROL_INTERIM_VOTER,
			false, 0);
		return 0;
	default:
		break;
	}

	return real_usb_desc->set_property(chg->usb_psy, psp, val);
}

static int smblite_shim_usb_prop_is_writeable(struct power_supply *psy,
					enum power_supply_property psp)
{
	struct smblite_shim *shim = power_supply_get_drvdata(psy);
	struct smb_charger *chg = shim->chg;
	const struct power_supply_desc *real_usb_desc = chg->usb_psy->desc;

	return real_usb_desc->property_is_writeable(chg->usb_psy, psp);
}

static void smblite_shim_external_power_changed(struct power_supply *psy)
{
	power_supply_changed(psy);
}

static int vote_cb_notify_psy_changed(struct gvotable_election *el,
				const char *reason,
				void *vote)
{
	struct smblite_shim *shim =
		(struct smblite_shim *)gvotable_get_data(el);
	power_supply_changed(shim->psy);
	return 0;
}

void smblite_shim_deinit(struct smb_charger *chg)
{
	struct smblite_shim *shim = chg->shim;

	gvotable_destroy_election(shim->vmax_votable);
	gvotable_destroy_election(shim->fake_psy_online_votable);
	mutex_destroy(&shim->lock);
}

struct smblite_shim *smblite_shim_init(struct smb_charger *chg)
{
	struct smblite_shim *shim;

	shim = devm_kzalloc(chg->dev, sizeof(*shim), GFP_KERNEL);

	if (!shim)
		return NULL;

	mutex_init(&shim->lock);
	shim->chg = chg;

	shim->fake_psy_online_votable =
		gvotable_create_bool_election("SHIM_FAKE_OLN",
					vote_cb_notify_psy_changed,
					shim);
	shim->vmax_votable =
		gvotable_create_int_election("SHIM_VMAX",
					gvotable_comparator_uint_min,
					vote_cb_notify_psy_changed,
					shim);

	BLOCKING_INIT_NOTIFIER_HEAD(&shim->hvdcp_req_nh);
	BLOCKING_INIT_NOTIFIER_HEAD(&shim->usbin_plugin_nh);
	BLOCKING_INIT_NOTIFIER_HEAD(&shim->boost_nh);

	return shim;
}

int smblite_shim_on_usb_psy_created(struct smblite_shim *shim,
				struct power_supply_desc *existing_usb_desc)
{
	struct power_supply_config usb_cfg = {};

	memcpy(&usb_psy_desc, existing_usb_desc, sizeof(usb_psy_desc));

	usb_psy_desc.name = "usb";
	usb_psy_desc.get_property = smblite_shim_usb_get_prop;
	usb_psy_desc.set_property = smblite_shim_usb_set_prop;
	usb_psy_desc.property_is_writeable = smblite_shim_usb_prop_is_writeable;
	usb_psy_desc.external_power_changed = smblite_shim_external_power_changed;

	usb_cfg.drv_data = shim;
	usb_cfg.of_node = shim->chg->dev->of_node;
	shim->psy = devm_power_supply_register(shim->chg->dev,
						&usb_psy_desc, &usb_cfg);
	if (IS_ERR(shim->psy)) {
		pr_err("Couldn't register smblite shim USB power supply\n");
		return PTR_ERR(shim->psy);
	}

	return 0;
}

void smblite_shim_on_usb_type_updated(struct smblite_shim *shim,
				enum power_supply_type type)
{
	usb_psy_desc.type = type;
}

int smblite_shim_update_sw_icl_max(struct smblite_shim *shim, int type)
{
	if (smblite_shim_is_icl_ext_ctrl_active(shim)) {
		/* Disable SW_ICL_MAX_VOTER since it gets re-set on unplug */
		vote(shim->chg->usb_icl_votable, SW_ICL_MAX_VOTER, false, 0);
		return 0;
	}

	if (type == POWER_SUPPLY_TYPE_USB) {
		/* Force 500mA for USB port type */
		vote(shim->chg->usb_icl_votable, SW_ICL_MAX_VOTER,
		true, SDP_CURRENT_UA);
		return 0;
	}

	return -ENOSYS;
}

bool smblite_shim_is_icl_ext_ctrl_active(struct smblite_shim *shim)
{
	return is_client_vote_enabled(shim->chg->usb_icl_votable,
				EXTERNAL_CONTROL_VOTER);
}

void smblite_shim_set_interim_ext_ctrl_icl_if_needed(struct smblite_shim *shim)
{
	int ret;
	u32 interim_icl;

	ret = of_property_read_u32(shim->chg->dev->of_node,
				"google,ext-ctl-interim-icl-ua", &interim_icl);

	vote(shim->chg->usb_icl_votable, EXTERNAL_CONTROL_INTERIM_VOTER,
		!ret, interim_icl);
}

void smblite_shim_notify_hvdcp_req(struct smblite_shim *shim)
{
	blocking_notifier_call_chain(&shim->hvdcp_req_nh, 0, shim);
}

void smblite_shim_notify_plugin(struct smblite_shim *shim,
				enum smblite_shim_plug_sts plugged)
{
	blocking_notifier_call_chain(&shim->usbin_plugin_nh, plugged, shim);
}

void smblite_shim_notify_boost_sw(struct smblite_shim *shim,
				enum smblite_shim_boost_sts boost)
{
	blocking_notifier_call_chain(&shim->boost_nh, boost, shim);
}

int smblite_shim_hvdcp_req_register_notifier(struct smblite_shim *shim,
					struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&shim->hvdcp_req_nh, nb);
}
EXPORT_SYMBOL_GPL(smblite_shim_hvdcp_req_register_notifier);

int smblite_shim_hvdcp_req_unregister_notifier(struct smblite_shim *shim,
					struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&shim->hvdcp_req_nh, nb);
}
EXPORT_SYMBOL_GPL(smblite_shim_hvdcp_req_unregister_notifier);

int smblite_shim_plugin_register_notifier(struct smblite_shim *shim,
					struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&shim->usbin_plugin_nh, nb);
}
EXPORT_SYMBOL_GPL(smblite_shim_plugin_register_notifier);

int smblite_shim_plugin_unregister_notifier(struct smblite_shim *shim,
					struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&shim->usbin_plugin_nh, nb);

}
EXPORT_SYMBOL_GPL(smblite_shim_plugin_unregister_notifier);

int smblite_shim_boost_register_notifier(struct smblite_shim *shim,
					struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&shim->boost_nh, nb);
}
EXPORT_SYMBOL_GPL(smblite_shim_boost_register_notifier);

int smblite_shim_boost_unregister_notifier(struct smblite_shim *shim,
					struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&shim->boost_nh, nb);

}
EXPORT_SYMBOL_GPL(smblite_shim_boost_unregister_notifier);
