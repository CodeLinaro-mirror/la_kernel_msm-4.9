/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright 2024 Google LLC */

#ifndef __SMBLITE_SHIM_PLUG_DEBOUNCE_H__
#define __SMBLITE_SHIM_PLUG_DEBOUNCE_H__

#include <linux/device.h>
#include "misc/gvotable.h"
#include "smblite-shim.h"

struct smblite_shim_plug_debounce *
smblite_shim_plug_debounce_init(struct smblite_shim *shim);

void smblite_shim_plug_debounce_deinit(struct smblite_shim *shim);
#endif