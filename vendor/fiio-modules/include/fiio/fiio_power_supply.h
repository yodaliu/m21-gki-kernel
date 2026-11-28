/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The stock FiiO M21 kernel's enum power_supply_property differs from GKI:
 * it inserts POWER_SUPPLY_PROP_CHARGING_ENABLED (68) before
 * POWER_SUPPLY_PROP_CALIBRATE, shifting every later property by one, and
 * appends QUICK_CHARGE_TYPE, TX_ADAPTER, SIGNAL_STRENGTH and
 * REVERSE_CHG_MODE. The stock modules were built against that numbering,
 * so the reconstructed ones use these values for properties >= 68.
 */
#ifndef _FIIO_POWER_SUPPLY_H
#define _FIIO_POWER_SUPPLY_H

#include <linux/power_supply.h>

#define FIIO_PSP(n)				((enum power_supply_property)(n))
#define FIIO_PSP_CHARGING_ENABLED		FIIO_PSP(68)
#define FIIO_PSP_CALIBRATE			FIIO_PSP(69)
#define FIIO_PSP_MANUFACTURE_YEAR		FIIO_PSP(70)
#define FIIO_PSP_MANUFACTURE_MONTH		FIIO_PSP(71)
#define FIIO_PSP_MANUFACTURE_DAY		FIIO_PSP(72)
#define FIIO_PSP_MODEL_NAME			FIIO_PSP(73)
#define FIIO_PSP_MANUFACTURER			FIIO_PSP(74)
#define FIIO_PSP_SERIAL_NUMBER			FIIO_PSP(75)
#define FIIO_PSP_QUICK_CHARGE_TYPE		FIIO_PSP(76)
#define FIIO_PSP_TX_ADAPTER			FIIO_PSP(77)
#define FIIO_PSP_SIGNAL_STRENGTH		FIIO_PSP(78)
#define FIIO_PSP_REVERSE_CHG_MODE		FIIO_PSP(79)

#endif
