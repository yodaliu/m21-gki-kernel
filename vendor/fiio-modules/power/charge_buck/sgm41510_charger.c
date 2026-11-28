// SPDX-License-Identifier: GPL-2.0
/*
 * SGM41510 charger driver (derived from the TI BQ25890 driver), as
 * modified by FiiO for the M21. Reconstructed from the stock module.
 */
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/types.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/usb/phy.h>
#include <linux/acpi.h>
#include <linux/of.h>
#include <linux/list.h>
#include <linux/workqueue.h>
#include <fiio/fiio_device.h>
#include <fiio/fiio_i2c.h>
#include <fiio/fiio_notify.h>
#include <fiio/charge_chip_common.h>
#include <fiio/fiio_power_supply.h>

#define sgm_printk(fmt, arg...) printk(KERN_ERR "[SGM41510_CHARGE_MANAGER]: " fmt, ##arg)

#define SGM41510_MANUFACTURER		"SG Micro"
#define SGM41510_IRQ_PIN		"sgm41510_irq"

#define SGM41510_ID			0

static LIST_HEAD(sgm41510_device_list);

enum sgm41510_fields {
	F_EN_HIZ, F_EN_ILIM, F_IILIM,				     /* Reg00 */
	F_BHOT, F_BCOLD, F_VINDPM_OFS,				     /* Reg01 */
	F_CONV_START, F_CONV_RATE, F_BOOSTF, F_ICO_EN,
	F_HVDCP_EN, F_MAXC_EN, F_FORCE_DPM, F_AUTO_DPDM_EN,	     /* Reg02 */
	F_BAT_LOAD_EN, F_WD_RST, F_OTG_CFG, F_CHG_CFG, F_SYSVMIN,
	F_MIN_BAT_SEL,						     /* Reg03 */
	F_PUMPX_EN, F_ICHG,					     /* Reg04 */
	F_IPRECHG, F_ITERM,					     /* Reg05 */
	F_VREG, F_BATLOWV, F_VRECHG,				     /* Reg06 */
	F_TERM_EN, F_STAT_DIS, F_WD, F_TMR_EN, F_CHG_TMR,
	F_JEITA_ISET,						     /* Reg07 */
	F_BATCMP, F_VCLAMP, F_TREG,				     /* Reg08 */
	F_FORCE_ICO, F_TMR2X_EN, F_BATFET_DIS, F_JEITA_VSET,
	F_BATFET_DLY, F_BATFET_RST_EN, F_PUMPX_UP, F_PUMPX_DN,	     /* Reg09 */
	F_BOOSTV, F_PFM_OTG_DIS, F_BOOSTI,			     /* Reg0A */
	F_VBUS_STAT, F_CHG_STAT, F_PG_STAT, F_SDP_STAT, F_VSYS_STAT, /* Reg0B */
	F_WD_FAULT, F_BOOST_FAULT, F_CHG_FAULT, F_BAT_FAULT,
	F_NTC_FAULT,						     /* Reg0C */
	F_FORCE_VINDPM, F_VINDPM,				     /* Reg0D */
	F_THERM_STAT, F_BATV,					     /* Reg0E */
	F_SYSV,							     /* Reg0F */
	F_TSPCT,						     /* Reg10 */
	F_VBUS_GD, F_VBUSV,					     /* Reg11 */
	F_ICHGR,						     /* Reg12 */
	F_VDPM_STAT, F_IDPM_STAT, F_IDPM_LIM,			     /* Reg13 */
	F_REG_RST, F_ICO_OPTIMIZED, F_PN, F_TS_PROFILE, F_DEV_REV,   /* Reg14 */
	F_CM_OUT, F_VBUS_OV, F_SP,				     /* Reg15 */

	F_MAX_FIELDS
};

/* initial field values, converted to register values */
struct sgm41510_init_data {
	u8 ichg;	/* charge current		*/
	u8 vreg;	/* regulation voltage		*/
	u8 iterm;	/* termination current		*/
	u8 iprechg;	/* precharge current		*/
	u8 sysvmin;	/* minimum system voltage limit */
	u8 boostv;	/* boost regulation voltage	*/
	u8 boosti;	/* boost current limit		*/
	u8 boostf;	/* boost frequency		*/
	u8 ilim_en;	/* enable ILIM pin		*/
	u8 treg;	/* thermal regulation threshold */
};

struct sgm41510_state {
	u8 online;
	u8 chrg_status;
	u8 chrg_fault;
	u8 vsys_status;
	u8 boost_fault;
	u8 bat_fault;
};

struct sgm41510_device {
	struct i2c_client *client;
	struct device *dev;
	struct power_supply *charger;
	struct power_supply *usb;

	struct usb_phy *usb_phy;
	struct notifier_block usb_nb;
	struct notifier_block fiio_vbus_nb;
	struct work_struct usb_work;
	struct delayed_work update_charge_status_work;
	struct delayed_work usb_limit_work;
	unsigned long usb_event;

	struct regmap *rmap;
	struct regmap_field *rmap_fields[F_MAX_FIELDS];

	int chip_id;
	struct sgm41510_init_data init_data;
	struct sgm41510_state state;

	struct mutex lock; /* protect state data */
	struct mutex i2c_rw_lock;

	struct list_head list;

	unsigned int last_limit_current;

	struct power_supply *cw2217_psy;

	bool otg_enabled;
	bool power_in_vbus;
	bool usb_limit_flag;

	int chg_en_gpio;
	int usb_icl_value;
	bool is_uac_mode;
};

struct sgm41510_device *g_sgm;

static const struct regmap_range sgm41510_readonly_reg_ranges[] = {
	regmap_reg_range(0x0b, 0x0c),
	regmap_reg_range(0x0e, 0x13),
};

static const struct regmap_access_table sgm41510_writeable_regs = {
	.no_ranges = sgm41510_readonly_reg_ranges,
	.n_no_ranges = ARRAY_SIZE(sgm41510_readonly_reg_ranges),
};

static const struct regmap_range sgm41510_volatile_reg_ranges[] = {
	regmap_reg_range(0x00, 0x00),
	regmap_reg_range(0x09, 0x09),
	regmap_reg_range(0x0b, 0x0c),
	regmap_reg_range(0x0e, 0x14),
};

static const struct regmap_access_table sgm41510_volatile_regs = {
	.yes_ranges = sgm41510_volatile_reg_ranges,
	.n_yes_ranges = ARRAY_SIZE(sgm41510_volatile_reg_ranges),
};

static const struct regmap_config sgm41510_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,

	.max_register = 0x15,
	.cache_type = REGCACHE_RBTREE,

	.wr_table = &sgm41510_writeable_regs,
	.volatile_table = &sgm41510_volatile_regs,
};

static const struct reg_field sgm41510_reg_fields[] = {
	/* REG00 */
	[F_EN_HIZ]		= REG_FIELD(0x00, 7, 7),
	[F_EN_ILIM]		= REG_FIELD(0x00, 6, 6),
	[F_IILIM]		= REG_FIELD(0x00, 0, 5),
	/* REG01 */
	[F_BHOT]		= REG_FIELD(0x01, 6, 7),
	[F_BCOLD]		= REG_FIELD(0x01, 5, 5),
	[F_VINDPM_OFS]		= REG_FIELD(0x01, 0, 4),
	/* REG02 */
	[F_CONV_START]		= REG_FIELD(0x02, 7, 7),
	[F_CONV_RATE]		= REG_FIELD(0x02, 6, 6),
	[F_BOOSTF]		= REG_FIELD(0x02, 5, 5),
	[F_ICO_EN]		= REG_FIELD(0x02, 4, 4),
	[F_HVDCP_EN]		= REG_FIELD(0x02, 3, 3),
	[F_MAXC_EN]		= REG_FIELD(0x02, 2, 2),
	[F_FORCE_DPM]		= REG_FIELD(0x02, 1, 1),
	[F_AUTO_DPDM_EN]	= REG_FIELD(0x02, 0, 0),
	/* REG03 */
	[F_BAT_LOAD_EN]		= REG_FIELD(0x03, 7, 7),
	[F_WD_RST]		= REG_FIELD(0x03, 6, 6),
	[F_OTG_CFG]		= REG_FIELD(0x03, 5, 5),
	[F_CHG_CFG]		= REG_FIELD(0x03, 4, 4),
	[F_SYSVMIN]		= REG_FIELD(0x03, 1, 3),
	[F_MIN_BAT_SEL]		= REG_FIELD(0x03, 0, 0),
	/* REG04 */
	[F_PUMPX_EN]		= REG_FIELD(0x04, 7, 7),
	[F_ICHG]		= REG_FIELD(0x04, 0, 6),
	/* REG05 */
	[F_IPRECHG]		= REG_FIELD(0x05, 4, 7),
	[F_ITERM]		= REG_FIELD(0x05, 0, 3),
	/* REG06 */
	[F_VREG]		= REG_FIELD(0x06, 2, 7),
	[F_BATLOWV]		= REG_FIELD(0x06, 1, 1),
	[F_VRECHG]		= REG_FIELD(0x06, 0, 0),
	/* REG07 */
	[F_TERM_EN]		= REG_FIELD(0x07, 7, 7),
	[F_STAT_DIS]		= REG_FIELD(0x07, 6, 6),
	[F_WD]			= REG_FIELD(0x07, 4, 5),
	[F_TMR_EN]		= REG_FIELD(0x07, 3, 3),
	[F_CHG_TMR]		= REG_FIELD(0x07, 1, 2),
	[F_JEITA_ISET]		= REG_FIELD(0x07, 0, 0),
	/* REG08 */
	[F_BATCMP]		= REG_FIELD(0x08, 5, 7),
	[F_VCLAMP]		= REG_FIELD(0x08, 2, 4),
	[F_TREG]		= REG_FIELD(0x08, 0, 1),
	/* REG09 */
	[F_FORCE_ICO]		= REG_FIELD(0x09, 7, 7),
	[F_TMR2X_EN]		= REG_FIELD(0x09, 6, 6),
	[F_BATFET_DIS]		= REG_FIELD(0x09, 5, 5),
	[F_JEITA_VSET]		= REG_FIELD(0x09, 4, 4),
	[F_BATFET_DLY]		= REG_FIELD(0x09, 3, 3),
	[F_BATFET_RST_EN]	= REG_FIELD(0x09, 2, 2),
	[F_PUMPX_UP]		= REG_FIELD(0x09, 1, 1),
	[F_PUMPX_DN]		= REG_FIELD(0x09, 0, 0),
	/* REG0A */
	[F_BOOSTV]		= REG_FIELD(0x0A, 4, 7),
	[F_PFM_OTG_DIS]		= REG_FIELD(0x0A, 3, 3),
	[F_BOOSTI]		= REG_FIELD(0x0A, 0, 2),
	/* REG0B */
	[F_VBUS_STAT]		= REG_FIELD(0x0B, 5, 7),
	[F_CHG_STAT]		= REG_FIELD(0x0B, 3, 4),
	[F_PG_STAT]		= REG_FIELD(0x0B, 2, 2),
	[F_SDP_STAT]		= REG_FIELD(0x0B, 1, 1),
	[F_VSYS_STAT]		= REG_FIELD(0x0B, 0, 0),
	/* REG0C */
	[F_WD_FAULT]		= REG_FIELD(0x0C, 7, 7),
	[F_BOOST_FAULT]		= REG_FIELD(0x0C, 6, 6),
	[F_CHG_FAULT]		= REG_FIELD(0x0C, 4, 5),
	[F_BAT_FAULT]		= REG_FIELD(0x0C, 3, 3),
	[F_NTC_FAULT]		= REG_FIELD(0x0C, 0, 2),
	/* REG0D */
	[F_FORCE_VINDPM]	= REG_FIELD(0x0D, 7, 7),
	[F_VINDPM]		= REG_FIELD(0x0D, 0, 6),
	/* REG0E */
	[F_THERM_STAT]		= REG_FIELD(0x0E, 7, 7),
	[F_BATV]		= REG_FIELD(0x0E, 0, 6),
	/* REG0F */
	[F_SYSV]		= REG_FIELD(0x0F, 0, 6),
	/* REG10 */
	[F_TSPCT]		= REG_FIELD(0x10, 0, 6),
	/* REG11 */
	[F_VBUS_GD]		= REG_FIELD(0x11, 7, 7),
	[F_VBUSV]		= REG_FIELD(0x11, 0, 6),
	/* REG12 */
	[F_ICHGR]		= REG_FIELD(0x12, 0, 7),
	/* REG13 */
	[F_VDPM_STAT]		= REG_FIELD(0x13, 7, 7),
	[F_IDPM_STAT]		= REG_FIELD(0x13, 6, 6),
	[F_IDPM_LIM]		= REG_FIELD(0x13, 0, 5),
	/* REG14 */
	[F_REG_RST]		= REG_FIELD(0x14, 7, 7),
	[F_ICO_OPTIMIZED]	= REG_FIELD(0x14, 6, 6),
	[F_PN]			= REG_FIELD(0x14, 3, 5),
	[F_TS_PROFILE]		= REG_FIELD(0x14, 2, 2),
	[F_DEV_REV]		= REG_FIELD(0x14, 0, 1),
	/* REG15 */
	[F_CM_OUT]		= REG_FIELD(0x15, 5, 7),
	[F_VBUS_OV]		= REG_FIELD(0x15, 2, 4),
	[F_SP]			= REG_FIELD(0x15, 0, 1),
};

/*
 * Most of the val -> idx conversions can be computed, given the minimum,
 * maximum and the step between values. For the rest of conversions, we use
 * lookup tables.
 */
enum sgm41510_table_ids {
	/* range tables */
	TBL_IINLIM,
	TBL_VINDPM,
	TBL_ICHG,
	TBL_ITERM,
	TBL_IPRECHG,
	TBL_VREG,
	TBL_BATCMP,
	TBL_VCLAMP,
	TBL_BOOSTV,
	TBL_SYSVMIN,

	/* lookup tables */
	TBL_TREG,
	TBL_BOOSTI,
};

/* Thermal Regulation Threshold lookup table, in degrees Celsius */
static const u32 sgm41510_treg_tbl[] = { 60, 80, 100, 120 };

#define SGM41510_TREG_TBL_SIZE		ARRAY_SIZE(sgm41510_treg_tbl)

/* Boost mode current limit lookup table, in uA */
static const u32 sgm41510_boosti_tbl[] = {
	1200000, 1600000, 2000000, 2400000, 2800000, 3200000, 3600000, 4000000
};

#define SGM41510_BOOSTI_TBL_SIZE		ARRAY_SIZE(sgm41510_boosti_tbl)

struct sgm41510_range {
	u32 min;
	u32 max;
	u32 step;
};

struct sgm41510_lookup {
	const u32 *tbl;
	u32 size;
};

static const union {
	struct sgm41510_range  rt;
	struct sgm41510_lookup lt;
} sgm41510_tables[] = {
	/* range tables */
	[TBL_IINLIM] =	{ .rt = {70000,    4900000, 70000} },	  /* uA */
	[TBL_VINDPM] =	{ .rt = {3000000,  6150000, 50000} },	  /* uV */
	[TBL_ICHG] =	{ .rt = {0,	   5056000, 64000} },	  /* uA */
	[TBL_ITERM] =	{ .rt = {64000,    1024000, 64000} },	  /* uA */
	[TBL_IPRECHG] =	{ .rt = {64000,    1024000, 64000} },	  /* uA */
	[TBL_VREG] =	{ .rt = {3840000,  4608000, 16000} },	  /* uV */
	[TBL_BATCMP] =	{ .rt = {0,	   140,     20} },	  /* mOhm */
	[TBL_VCLAMP] =	{ .rt = {0,	   224000,  32000} },	  /* uV */
	[TBL_BOOSTV] =	{ .rt = {4550000,  5510000, 64000} },	  /* uV */
	[TBL_SYSVMIN] = { .rt = {3000000,  3700000, 100000} },	  /* uV */

	/* lookup tables */
	[TBL_TREG] =	{ .lt = {sgm41510_treg_tbl, SGM41510_TREG_TBL_SIZE} },
	[TBL_BOOSTI] =	{ .lt = {sgm41510_boosti_tbl, SGM41510_BOOSTI_TBL_SIZE} }
};

static int sgm41510_field_read(struct sgm41510_device *sgm,
			       enum sgm41510_fields field_id)
{
	int ret;
	int val = 0;

	ret = fiio_regmap_field_read(sgm->client->name, sgm->rmap_fields[field_id], &val);
	if (ret < 0)
		return ret;

	return val;
}

static int sgm41510_field_write(struct sgm41510_device *sgm,
				enum sgm41510_fields field_id, u8 val)
{
	return fiio_regmap_field_wirte(sgm->client->name, sgm->rmap_fields[field_id], val);
}

static u8 sgm41510_find_idx(u32 value, enum sgm41510_table_ids id)
{
	u8 idx;

	if (id >= TBL_TREG) {
		const u32 *tbl = sgm41510_tables[id].lt.tbl;
		u32 tbl_size = sgm41510_tables[id].lt.size;

		for (idx = 1; idx < tbl_size && tbl[idx] <= value; idx++)
			;
	} else {
		const struct sgm41510_range *rtbl = &sgm41510_tables[id].rt;
		u8 rtbl_size;

		rtbl_size = (rtbl->max - rtbl->min) / rtbl->step + 1;

		for (idx = 1;
		     idx < rtbl_size && (idx * rtbl->step + rtbl->min <= value);
		     idx++)
			;
	}

	return idx - 1;
}

static u32 sgm41510_find_val(u8 idx, enum sgm41510_table_ids id)
{
	const struct sgm41510_range *rtbl;

	/* lookup table? */
	if (id >= TBL_TREG)
		return sgm41510_tables[id].lt.tbl[idx];

	/* range table */
	rtbl = &sgm41510_tables[id].rt;

	return (rtbl->min + idx * rtbl->step);
}

enum sgm41510_status {
	STATUS_NOT_CHARGING,
	STATUS_PRE_CHARGING,
	STATUS_FAST_CHARGING,
	STATUS_TERMINATION_DONE,
};

enum sgm41510_chrg_fault {
	CHRG_FAULT_NORMAL,
	CHRG_FAULT_INPUT,
	CHRG_FAULT_THERMAL_SHUTDOWN,
	CHRG_FAULT_TIMER_EXPIRED,
};

static void sgm41510_charger_set_uac_mode(bool enable_uac)
{
	if (g_sgm)
		g_sgm->is_uac_mode = enable_uac;
}

static int sgm41510_charger_set_limit_current(unsigned int limit_cur)
{
	int ret;
	struct sgm41510_device *sgm = g_sgm;

	if (!sgm)
		return 0;

	if (limit_cur >= sgm41510_tables[TBL_IINLIM].rt.max)
		limit_cur = sgm41510_tables[TBL_IINLIM].rt.max;
	if (sgm->is_uac_mode || limit_cur <= sgm41510_tables[TBL_IINLIM].rt.min)
		limit_cur = sgm41510_tables[TBL_IINLIM].rt.min;

	ret = sgm41510_field_write(sgm, F_IILIM, sgm41510_find_idx(limit_cur, TBL_IINLIM));
	if (ret < 0) {
		sgm_printk("%s set sgm41510 limit cur failed\n", __func__);
	} else {
		sgm->last_limit_current = limit_cur;
		sgm_printk("%s limit_cur = %d\n", __func__, limit_cur);
	}

	return ret;
}

static int sgm41510_charger_set_charge_current(unsigned int cur)
{
	int ret;
	struct list_head *pos;
	struct sgm41510_device *sgm = NULL;

	list_for_each(pos, &sgm41510_device_list) {
		sgm = list_entry(pos, struct sgm41510_device, list);
		if (!sgm) {
			printk(KERN_ERR "sgm41510 device is not ready\n");
			return -1;
		}
	}

	if (cur <= sgm41510_tables[TBL_ICHG].rt.min)
		cur = sgm41510_tables[TBL_ICHG].rt.min;
	else if (cur >= sgm41510_tables[TBL_ICHG].rt.max)
		cur = sgm41510_tables[TBL_ICHG].rt.max;

	ret = sgm41510_field_write(sgm, F_ICHG, sgm41510_find_idx(cur, TBL_ICHG));
	if (ret < 0)
		dev_err(sgm->dev, "set sgm41510 charge cur failed\n");
	else
		sgm_printk("%s cur = %d\n", __func__, cur);

	return ret;
}

static int sgm41510_charger_enabled(bool enable)
{
	int ret;
	struct list_head *pos;
	struct sgm41510_device *sgm = NULL;

	list_for_each(pos, &sgm41510_device_list) {
		sgm = list_entry(pos, struct sgm41510_device, list);
		if (!sgm) {
			printk(KERN_ERR "sgm41510 device is not ready\n");
			return -1;
		}
	}

	if (enable) {
		ret = sgm41510_field_write(sgm, F_CHG_CFG, 1);
		if (ret < 0) {
			dev_err(sgm->dev, "failed to enable charger\n");
			return ret;
		}
		sgm_printk("%s enabled sgm41510 charger\n", __func__);
	} else {
		ret = sgm41510_field_write(sgm, F_CHG_CFG, 0);
		if (ret < 0) {
			dev_err(sgm->dev, "failed to disable charger\n");
			return ret;
		}
		sgm_printk("%s disabled sgm41510 charger\n", __func__);
	}

	return ret;
}

static int sgm41510_charger_start_charge(bool enable)
{
	int ret;
	struct list_head *pos;
	struct sgm41510_device *sgm = NULL;

	list_for_each(pos, &sgm41510_device_list) {
		sgm = list_entry(pos, struct sgm41510_device, list);
		if (!sgm) {
			printk(KERN_ERR "sgm41510 device is not ready\n");
			return -1;
		}
	}

	if (enable) {
		ret = sgm41510_field_write(sgm, F_EN_HIZ, 0);
		if (ret)
			sgm_printk("%s disable HIZ mode failed\n", __func__);
		ret = sgm41510_charger_enabled(true);
		if (ret)
			sgm_printk("%s failed to enable charger\n", __func__);
	} else {
		ret = sgm41510_charger_enabled(false);
		if (ret)
			sgm_printk("%s failed to disable charger\n", __func__);
	}

	return ret;
}

int sgm41510_get_prop_from_cw221x(struct sgm41510_device *sgm,
				  enum power_supply_property psp,
				  union power_supply_propval *val)
{
	int rc;

	sgm->cw2217_psy = power_supply_get_by_name("cw221X-bat");
	if (!sgm->cw2217_psy)
		return -EINVAL;

	rc = power_supply_get_property(sgm->cw2217_psy, psp, val);

	return rc;
}

static int sgm41510_power_supply_get_property(struct power_supply *psy,
					      enum power_supply_property psp,
					      union power_supply_propval *val)
{
	int ret;
	struct sgm41510_device *sgm = power_supply_get_drvdata(psy);
	struct sgm41510_state state;

	val->intval = -ENODATA;
	if (!sgm)
		return -1;

	mutex_lock(&sgm->lock);
	state = sgm->state;
	mutex_unlock(&sgm->lock);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		if (!state.online)
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		else if (state.chrg_status == STATUS_NOT_CHARGING)
			val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		else if (state.chrg_status == STATUS_PRE_CHARGING ||
			 state.chrg_status == STATUS_FAST_CHARGING)
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		else if (state.chrg_status == STATUS_TERMINATION_DONE)
			val->intval = POWER_SUPPLY_STATUS_FULL;
		else
			val->intval = POWER_SUPPLY_STATUS_UNKNOWN;

		break;

	case FIIO_PSP_MANUFACTURER:
		val->strval = SGM41510_MANUFACTURER;
		break;

	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = state.online;
		break;

	case POWER_SUPPLY_PROP_HEALTH:
		if (!state.chrg_fault && !state.bat_fault && !state.boost_fault)
			val->intval = POWER_SUPPLY_HEALTH_GOOD;
		else if (state.bat_fault)
			val->intval = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
		else if (state.chrg_fault == CHRG_FAULT_TIMER_EXPIRED)
			val->intval = POWER_SUPPLY_HEALTH_SAFETY_TIMER_EXPIRE;
		else
			val->intval = POWER_SUPPLY_HEALTH_UNSPEC_FAILURE;
		break;

	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		ret = sgm41510_field_read(sgm, F_ICHG);
		if (ret < 0)
			return ret;

		val->intval = sgm41510_find_val(ret, TBL_ICHG);
		break;

	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		val->intval = sgm41510_tables[TBL_ICHG].rt.max;
		break;

	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
		break;

	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX:
		val->intval = sgm41510_tables[TBL_VREG].rt.max;
		break;

	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		ret = sgm41510_field_read(sgm, F_IILIM);
		if (ret < 0)
			return ret;

		val->intval = sgm41510_find_val(ret, TBL_IINLIM);
		break;

	case POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT:
		ret = sgm41510_field_read(sgm, F_ITERM);
		if (ret < 0)
			return ret;

		val->intval = sgm41510_find_val(ret, TBL_ITERM);
		break;

	case FIIO_PSP_CHARGING_ENABLED:
		val->intval = sgm41510_field_read(sgm, F_CHG_CFG);
		break;

	case POWER_SUPPLY_PROP_CAPACITY:
		ret = sgm41510_get_prop_from_cw221x(sgm, POWER_SUPPLY_PROP_CAPACITY, val);
		if (ret < 0)
			return ret;
		break;

	case POWER_SUPPLY_PROP_TEMP:
		ret = sgm41510_get_prop_from_cw221x(sgm, POWER_SUPPLY_PROP_TEMP, val);
		if (ret < 0)
			return ret;
		break;

	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = sgm41510_get_prop_from_cw221x(sgm, POWER_SUPPLY_PROP_VOLTAGE_NOW, val);
		if (ret < 0)
			return ret;
		break;

	case POWER_SUPPLY_PROP_CURRENT_NOW:
		ret = sgm41510_get_prop_from_cw221x(sgm, POWER_SUPPLY_PROP_CURRENT_NOW, val);
		if (ret < 0)
			return ret;
		break;

	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		ret = sgm41510_get_prop_from_cw221x(sgm, POWER_SUPPLY_PROP_CYCLE_COUNT, val);
		if (ret < 0)
			return ret;
		break;

	case POWER_SUPPLY_PROP_TECHNOLOGY:
		ret = sgm41510_get_prop_from_cw221x(sgm, POWER_SUPPLY_PROP_TECHNOLOGY, val);
		if (ret < 0)
			return ret;
		break;


	default:
		return -EINVAL;
	}

	return 0;
}

static int sgm41510_property_is_writeable(struct power_supply *psy,
					  enum power_supply_property prop)
{
	switch (prop) {
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
	case POWER_SUPPLY_PROP_PRECHARGE_CURRENT:
	case POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT:
	case FIIO_PSP_CHARGING_ENABLED:
		return 1;
	default:
		break;
	}

	return 0;
}

static int sgm41510_charger_set_property(struct power_supply *psy,
					 enum power_supply_property prop,
					 const union power_supply_propval *val)
{
	struct sgm41510_device *sgm = power_supply_get_drvdata(psy);
	int ret;

	if (prop == FIIO_PSP_CHARGING_ENABLED) {
		if (val->intval)
			ret = sgm41510_field_write(sgm, F_CHG_CFG, 1);
		else
			ret = sgm41510_field_write(sgm, F_CHG_CFG, 0);
	} else if (prop == POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT) {
		ret = sgm41510_field_write(sgm, F_IILIM, sgm41510_find_idx(val->intval, TBL_IINLIM));
	} else {
		return -EINVAL;
	}

	return ret;
}

static int sgm41510_get_chip_state(struct sgm41510_device *sgm,
				   struct sgm41510_state *state)
{
	int i, ret;

	struct {
		enum sgm41510_fields id;
		u8 *data;
	} state_fields[] = {
		{F_CHG_STAT,	&state->chrg_status},
		{F_PG_STAT,	&state->online},
		{F_VSYS_STAT,	&state->vsys_status},
		{F_BOOST_FAULT, &state->boost_fault},
		{F_BAT_FAULT,	&state->bat_fault},
		{F_CHG_FAULT,	&state->chrg_fault}
	};

	for (i = 0; i < ARRAY_SIZE(state_fields); i++) {
		ret = sgm41510_field_read(sgm, state_fields[i].id);
		if (ret < 0)
			return ret;

		*state_fields[i].data = ret;
	}

	return 0;
}

static bool sgm41510_state_changed(struct sgm41510_device *sgm,
				   struct sgm41510_state *new_state)
{
	struct sgm41510_state old_state;

	mutex_lock(&sgm->lock);
	old_state = sgm->state;
	mutex_unlock(&sgm->lock);

	return (old_state.chrg_status != new_state->chrg_status ||
		old_state.chrg_fault != new_state->chrg_fault	||
		old_state.online != new_state->online		||
		old_state.bat_fault != new_state->bat_fault	||
		old_state.boost_fault != new_state->boost_fault ||
		old_state.vsys_status != new_state->vsys_status);
}

static void sgm41510_handle_state_change(struct sgm41510_device *sgm,
					 struct sgm41510_state *new_state)
{
	int ret;
	struct sgm41510_state old_state;

	mutex_lock(&sgm->lock);
	old_state = sgm->state;
	mutex_unlock(&sgm->lock);

	if (!new_state->online && old_state.online) {	     /* power removed */
		/* disable ADC */
		ret = sgm41510_field_write(sgm, F_CONV_START, 0);
		cancel_delayed_work(&sgm->update_charge_status_work);
		sgm_printk("%s power removed\n", __func__);
		if (ret < 0)
			goto error;
	} else if (new_state->online && !old_state.online) { /* power inserted */
		/* enable ADC, to have control of charge current/voltage */
		ret = sgm41510_field_write(sgm, F_CONV_START, 1);
		queue_delayed_work(system_wq, &sgm->update_charge_status_work,
				   msecs_to_jiffies(200));
		sgm_printk("%s power inserted\n", __func__);
		if (ret < 0)
			goto error;
	}

	power_supply_changed(sgm->charger);
	return;

error:
	dev_err(sgm->dev, "Error communicating with the chip.\n");
}

static irqreturn_t sgm41510_irq_handler_thread(int irq, void *private)
{
	struct sgm41510_device *sgm = private;
	int ret;
	struct sgm41510_state state;

	ret = sgm41510_get_chip_state(sgm, &state);
	if (ret < 0)
		goto handled;

	if (!sgm41510_state_changed(sgm, &state))
		goto handled;

	sgm41510_handle_state_change(sgm, &state);

	mutex_lock(&sgm->lock);
	sgm->state = state;
	mutex_unlock(&sgm->lock);

	power_supply_changed(sgm->charger);

handled:
	return IRQ_HANDLED;
}

static int sgm41510_chip_reset(struct sgm41510_device *sgm)
{
	int ret;
	int rst_check_counter = 10;

	ret = sgm41510_field_write(sgm, F_REG_RST, 1);
	if (ret < 0)
		return ret;

	do {
		ret = sgm41510_field_read(sgm, F_REG_RST);
		if (ret < 0)
			return ret;

		usleep_range(5, 10);
	} while (ret == 1 && --rst_check_counter);

	if (!rst_check_counter)
		return -ETIMEDOUT;

	return 0;
}

static int sgm41510_hw_init(struct sgm41510_device *sgm)
{
	int ret;
	int i;
	struct sgm41510_state state;

	const struct {
		enum sgm41510_fields id;
		u32 value;
	} init_data[] = {
		{F_ICHG,	 sgm->init_data.ichg},
		{F_VREG,	 sgm->init_data.vreg},
		{F_ITERM,	 sgm->init_data.iterm},
		{F_IPRECHG,	 sgm->init_data.iprechg},
		{F_SYSVMIN,	 sgm->init_data.sysvmin},
		{F_BOOSTV,	 sgm->init_data.boostv},
		{F_BOOSTI,	 sgm->init_data.boosti},
		{F_BOOSTF,	 sgm->init_data.boostf},
		{F_EN_ILIM,	 sgm->init_data.ilim_en},
		{F_TREG,	 sgm->init_data.treg}
	};

	ret = sgm41510_chip_reset(sgm);
	if (ret < 0)
		return ret;

	/* disable watchdog */
	ret = sgm41510_field_write(sgm, F_WD, 0);
	if (ret < 0)
		return ret;

	ret = sgm41510_field_write(sgm, F_CHG_CFG, 0);
	if (ret < 0)
		return ret;

	/* initialize currents/voltages and other parameters */
	for (i = 0; i < ARRAY_SIZE(init_data); i++) {
		ret = sgm41510_field_write(sgm, init_data[i].id,
					   init_data[i].value);
		if (ret < 0)
			return ret;
	}

	ret = sgm41510_field_write(sgm, F_TERM_EN, 1);
	if (ret < 0)
		return ret;

	ret = sgm41510_field_write(sgm, F_VBUS_OV, 0);
	if (ret < 0)
		return ret;

	ret = sgm41510_get_chip_state(sgm, &state);
	if (ret < 0)
		return ret;

	mutex_lock(&sgm->lock);
	sgm->state = state;
	mutex_unlock(&sgm->lock);

	return 0;
}

static enum power_supply_property sgm41510_power_supply_props[] = {
	FIIO_PSP_MANUFACTURER,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	FIIO_PSP_CHARGING_ENABLED,
};

static char *sgm41510_charger_supplied_to[] = {
	"main-battery",
};

static const struct power_supply_desc sgm41510_power_supply_desc = {
	.name = "sgm41510-charger",
	.type = POWER_SUPPLY_TYPE_MAINS,
	.properties = sgm41510_power_supply_props,
	.num_properties = ARRAY_SIZE(sgm41510_power_supply_props),
	.get_property = sgm41510_power_supply_get_property,
	.set_property = sgm41510_charger_set_property,
	.property_is_writeable = sgm41510_property_is_writeable,
};

static int sgm_usb_get_prop(struct power_supply *psy,
			    enum power_supply_property psp,
			    union power_supply_propval *val)
{
	struct sgm41510_device *sgm = power_supply_get_drvdata(psy);
	struct sgm41510_state state;
	int ret = 0;

	val->intval = -ENODATA;

	mutex_lock(&sgm->lock);
	state = sgm->state;
	mutex_unlock(&sgm->lock);

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = state.online;
		break;
	case POWER_SUPPLY_PROP_TYPE:
		val->intval = POWER_SUPPLY_TYPE_USB;
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		ret = sgm41510_field_read(sgm, F_IILIM);
		if (ret < 0)
			return ret;

		val->intval = sgm41510_find_val(ret, TBL_IINLIM);
		fallthrough;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int sgm_usb_set_prop(struct power_supply *psy,
			    enum power_supply_property psp,
			    const union power_supply_propval *val)
{
	struct sgm41510_device *sgm = power_supply_get_drvdata(psy);
	int rc = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		sgm->usb_icl_value = val->intval;
		if (!sgm->power_in_vbus && !sgm->usb_limit_flag) {
			sgm->usb_limit_flag = true;
			queue_delayed_work(system_wq, &sgm->usb_limit_work,
					   msecs_to_jiffies(500));
		}
		break;
	default:
		rc = -EINVAL;
		break;
	}

	return rc;
}

static int sgm_usb_is_writeable(struct power_supply *psy,
				enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		return 1;
	default:
		break;
	}

	return 0;
}

static enum power_supply_property sgm_usb_main_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
	POWER_SUPPLY_PROP_TYPE,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
};

static char *sgm41510_usb_supplied_to[] = {
	"main-battery",
};

static struct power_supply_desc sgm41510_usb_supply_desc = {
	.name = "usb",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = sgm_usb_main_props,
	.num_properties = ARRAY_SIZE(sgm_usb_main_props),
	.get_property = sgm_usb_get_prop,
	.set_property = sgm_usb_set_prop,
	.property_is_writeable = sgm_usb_is_writeable,
};

static int sgm41510_usb_supply_init(struct sgm41510_device *sgm,
				    struct device *dev)
{
	struct power_supply_config usb_cfg = {};

	usb_cfg.of_node = (struct device_node *)dev;
	usb_cfg.drv_data = sgm;
	usb_cfg.supplied_to = sgm41510_usb_supplied_to;
	usb_cfg.num_supplicants = ARRAY_SIZE(sgm41510_usb_supplied_to);

	sgm->usb = devm_power_supply_register(sgm->dev, &sgm41510_usb_supply_desc, &usb_cfg);
	if (IS_ERR(sgm->usb)) {
		printk(KERN_ERR "Couldn't register usb power supply\n");
		return PTR_ERR(sgm->usb);
	}

	return 0;
}

static int sgm41510_power_supply_init(struct sgm41510_device *sgm)
{
	struct power_supply_config psy_cfg = { .drv_data = sgm, };

	psy_cfg.supplied_to = sgm41510_charger_supplied_to;
	psy_cfg.num_supplicants = ARRAY_SIZE(sgm41510_charger_supplied_to);

	sgm->charger = power_supply_register(sgm->dev, &sgm41510_power_supply_desc,
					     &psy_cfg);

	return PTR_ERR_OR_ZERO(sgm->charger);
}

static void sgm41510_usb_work(struct work_struct *data)
{
	int ret;
	struct sgm41510_device *sgm =
			container_of(data, struct sgm41510_device, usb_work);

	switch (sgm->usb_event) {
	case USB_EVENT_ID:
		/* Enable boost mode */
		ret = sgm41510_field_write(sgm, F_OTG_CFG, 1);
		if (ret < 0)
			goto error;
		break;

	case USB_EVENT_NONE:
		/* Disable boost mode */
		ret = sgm41510_field_write(sgm, F_OTG_CFG, 0);
		if (ret < 0)
			goto error;

		power_supply_changed(sgm->charger);
		break;
	}

	return;

error:
	dev_err(sgm->dev, "Error switching to boost/charger mode.\n");
}

static int sgm41510_otg_en(struct sgm41510_device *sgm, bool enable)
{
	int ret;

	ret = sgm41510_field_write(sgm, F_OTG_CFG, enable);

	return ret;
}

static void sgm_update_charge_status_work(struct work_struct *data)
{
	struct sgm41510_device *sgm = container_of(data, struct sgm41510_device,
						   update_charge_status_work.work);
	struct sgm41510_state state, old_state;
	int ret;

	ret = sgm41510_get_chip_state(sgm, &state);
	if (ret < 0)
		goto OUT;

	old_state = sgm->state;
	mutex_lock(&sgm->lock);
	sgm->state = state;
	mutex_unlock(&sgm->lock);

	if (state.online && sgm->state.chrg_status != old_state.chrg_status)
		power_supply_changed(sgm->charger);

	queue_delayed_work(system_wq, &sgm->update_charge_status_work,
			   msecs_to_jiffies(5000));
OUT:
	return;
}

static void sgm41510_usb_limit_work(struct work_struct *data)
{
	struct sgm41510_device *sgm = container_of(data, struct sgm41510_device,
						   usb_limit_work.work);

	sgm_printk("%s enter\n", __func__);
	sgm41510_charger_set_limit_current(sgm->usb_icl_value);
	sgm->usb_limit_flag = false;
}

static int sgm41510_enable_vbus(bool enable)
{
	struct sgm41510_device *sgm = g_sgm;
	int ret;

	sgm_printk("%s enter\n", __func__);
	ret = sgm41510_otg_en(sgm, enable);
	if (ret < 0)
		sgm_printk("%s enable otg failed.\n", __func__);

	return ret;
}

static bool sgm41510_check_charging(void)
{
	struct sgm41510_device *sgm = g_sgm;
	int ret;

	ret = sgm41510_field_read(sgm, F_CHG_CFG);

	return ret < 0 || ret == 1;
}

static int sgm41510_usb_notifier(struct notifier_block *nb, unsigned long val,
				 void *priv)
{
	struct sgm41510_device *sgm =
			container_of(nb, struct sgm41510_device, usb_nb);

	sgm->usb_event = val;
	queue_work(system_power_efficient_wq, &sgm->usb_work);

	return NOTIFY_OK;
}

static int sgm41510_vbus_fiio_notifier(struct notifier_block *nb, unsigned long val,
				       void *priv)
{
	struct sgm41510_device *sgm =
			container_of(nb, struct sgm41510_device, fiio_vbus_nb);
	int ret = 0;

	switch (val) {
	case 0:
		sgm->power_in_vbus = true;
		break;
	case 1:
		sgm->power_in_vbus = false;
		break;
	default:
		break;
	}

	return ret;
}

static ssize_t charger_en_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct sgm41510_device *sgm = dev_get_drvdata(dev);
	int val;

	val = sgm41510_field_read(sgm, F_CHG_CFG);

	return sprintf(buf, "%d\n", val);
}

static ssize_t charger_en_store(struct device *dev,
				struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret)
		return count;

	switch (val) {
	case 1:
		ret = sgm41510_charger_enabled(true);
		break;
	case 0:
		ret = sgm41510_charger_enabled(false);
		break;
	default:
		sgm_printk("store parameter error.\n");
		break;
	}
	if (ret < 0)
		return ret;

	return count;
}

static DEVICE_ATTR_RW(charger_en);

static struct attribute *sgm41510_attributes[] = {
	&dev_attr_charger_en.attr,
	NULL,
};

static const struct attribute_group sgm41510_attr_group = {
	.attrs = sgm41510_attributes,
};

static int sgm41510_parse_dt(struct sgm41510_device *sgm)
{
	int ret;
	u32 property;
	int i;
	int irqn, irq_gpio;
	struct sgm41510_init_data *init = &sgm->init_data;
	struct {
		char *name;
		bool optional;
		enum sgm41510_table_ids tbl_id;
		u8 *conv_data; /* holds converted value from given property */
	} props[] = {
		/* required properties */
		{"sgm,charge-current", false, TBL_ICHG, &init->ichg},
		{"sgm,battery-regulation-voltage", false, TBL_VREG, &init->vreg},
		{"sgm,termination-current", false, TBL_ITERM, &init->iterm},
		{"sgm,precharge-current", false, TBL_ITERM, &init->iprechg},
		{"sgm,minimum-sys-voltage", false, TBL_SYSVMIN, &init->sysvmin},
		{"sgm,boost-voltage", false, TBL_BOOSTV, &init->boostv},
		{"sgm,boost-max-current", false, TBL_BOOSTI, &init->boosti},

		/* optional properties */
		{"sgm,thermal-regulation-threshold", true, TBL_TREG, &init->treg}
	};

	/* initialize data for optional properties */
	init->treg = 3; /* 120 degrees Celsius */

	for (i = 0; i < ARRAY_SIZE(props); i++) {
		ret = device_property_read_u32(sgm->dev, props[i].name,
					       &property);
		if (ret < 0) {
			if (props[i].optional)
				continue;

			return ret;
		}

		*props[i].conv_data = sgm41510_find_idx(property,
						       props[i].tbl_id);
	}

	irq_gpio = of_get_named_gpio(sgm->dev->of_node, "sgm,hvc_int", 0);
	if (!gpio_is_valid(irq_gpio)) {
		dev_err(sgm->dev, "%s: %d gpio get failed\n", __func__, irq_gpio);
		return -EINVAL;
	}
	ret = gpio_request(irq_gpio, "sgm41510 irq pin");
	if (ret) {
		dev_err(sgm->dev, "%s: %d gpio request failed\n", __func__, irq_gpio);
		return ret;
	}
	gpio_direction_input(irq_gpio);

	irqn = gpio_to_irq(irq_gpio);
	if (irqn < 0) {
		dev_err(sgm->dev, "%s:%d gpio_to_irq failed\n", __func__, irqn);
		return irqn;
	}
	sgm->client->irq = irqn;
	enable_irq_wake(irqn);

	sgm->chg_en_gpio = of_get_named_gpio(sgm->dev->of_node, "sgm,chg-en-gpio", 0);
	if (!gpio_is_valid(sgm->chg_en_gpio)) {
		dev_err(sgm->dev, "%s: %d gpio get failed\n", __func__, sgm->chg_en_gpio);
		return -EINVAL;
	}
	ret = gpio_request(sgm->chg_en_gpio, "sgm chg en pin");
	if (ret) {
		dev_err(sgm->dev, "%s: %d gpio request failed\n", __func__, sgm->chg_en_gpio);
		return ret;
	}
	gpio_direction_output(sgm->chg_en_gpio, 0);

	return 0;
}

static int sgm41510_iic_test(void)
{
	struct sgm41510_device *sgm = g_sgm;
	int ret;

	ret = sgm41510_field_write(sgm, F_WD, 0);

	return ret;
}

static struct buck_charger_data sgm41510_charer_ops = {
	.specific_buck_charger_start_charge = sgm41510_charger_start_charge,
	.specific_buck_charger_enabled = sgm41510_charger_enabled,
	.specific_buck_charger_set_charge_current = sgm41510_charger_set_charge_current,
	.specific_buck_charger_set_limit_current = sgm41510_charger_set_limit_current,
	.specific_buck_otg_enable = sgm41510_enable_vbus,
	.specific_buck_charger_iic_test = sgm41510_iic_test,
	.specific_buck_charger_set_uac_mode = sgm41510_charger_set_uac_mode,
	.specific_buck_check_charging = sgm41510_check_charging,
};

static struct fiio_device_driver buck_charge_driver = {
	.name = "buck_charger module",
	.private_data = &sgm41510_charer_ops,
};

static int sgm41510_probe(struct i2c_client *client,
			  const struct i2c_device_id *id)
{
	struct i2c_adapter *adapter = to_i2c_adapter(client->dev.parent);
	struct device *dev = &client->dev;
	struct sgm41510_device *sgm;
	int ret;
	int i;

	sgm_printk("%s probe enter!\n", __func__);

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE_DATA)) {
		dev_err(dev, "No support for SMBUS_BYTE_DATA\n");
		return -ENODEV;
	}

	sgm = devm_kzalloc(dev, sizeof(*sgm), GFP_KERNEL);
	if (!sgm)
		return -ENOMEM;

	sgm->client = client;
	sgm->dev = dev;
	g_sgm = sgm;

	mutex_init(&sgm->lock);
	mutex_init(&sgm->i2c_rw_lock);

	sgm->rmap = devm_regmap_init_i2c(client, &sgm41510_regmap_config);
	if (IS_ERR(sgm->rmap)) {
		dev_err(dev, "failed to allocate register map\n");
		return PTR_ERR(sgm->rmap);
	}

	for (i = 0; i < ARRAY_SIZE(sgm41510_reg_fields); i++) {
		const struct reg_field *reg_fields = sgm41510_reg_fields;

		sgm->rmap_fields[i] = devm_regmap_field_alloc(dev, sgm->rmap,
							      reg_fields[i]);
		if (IS_ERR(sgm->rmap_fields[i])) {
			dev_err(dev, "cannot allocate regmap field\n");
			return PTR_ERR(sgm->rmap_fields[i]);
		}
	}

	i2c_set_clientdata(client, sgm);
	list_add(&sgm->list, &sgm41510_device_list);

	sgm->chip_id = sgm41510_field_read(sgm, F_PN);
	if (sgm->chip_id < 0) {
		dev_err(dev, "Cannot read chip ID.\n");
		return sgm->chip_id;
	}

	if (sgm->chip_id != SGM41510_ID) {
		dev_err(dev, "Chip with ID=%d, not supported!\n", sgm->chip_id);
		return -ENODEV;
	}

	if (!dev->platform_data) {
		ret = sgm41510_parse_dt(sgm);
		if (ret < 0) {
			dev_err(dev, "Cannot read device properties.\n");
			return ret;
		}
	} else {
		return -ENODEV;
	}

	ret = sgm41510_hw_init(sgm);
	if (ret < 0) {
		dev_err(dev, "Cannot initialize the chip.\n");
		return ret;
	}

	if (client->irq < 0) {
		dev_err(dev, "No irq resource found.\n");
		return client->irq;
	}

	/* OTG reporting */
	sgm->usb_phy = devm_usb_get_phy(dev, USB_PHY_TYPE_USB2);
	if (!IS_ERR_OR_NULL(sgm->usb_phy)) {
		INIT_WORK(&sgm->usb_work, sgm41510_usb_work);
		sgm->usb_nb.notifier_call = sgm41510_usb_notifier;
		usb_register_notifier(sgm->usb_phy, &sgm->usb_nb);
	}

	ret = devm_request_threaded_irq(dev, client->irq, NULL,
					sgm41510_irq_handler_thread,
					IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					dev_name(dev), sgm);
	if (ret)
		goto irq_fail;

	ret = sgm41510_power_supply_init(sgm);
	if (ret < 0) {
		dev_err(dev, "Failed to register power supply\n");
		goto irq_fail;
	}

	ret = sgm41510_usb_supply_init(sgm, (struct device *)client->dev.of_node);
	if (ret)
		dev_err(dev, "Failed to register usb supply\n");

	fiio_device_register(&buck_charge_driver);

	INIT_DELAYED_WORK(&sgm->update_charge_status_work, sgm_update_charge_status_work);
	INIT_DELAYED_WORK(&sgm->usb_limit_work, sgm41510_usb_limit_work);

	ret = sysfs_create_group(&sgm->dev->kobj, &sgm41510_attr_group);
	if (ret < 0) {
		sgm_printk("failed to register sysfs. err: %d\n", ret);
		return ret;
	}

	sgm->power_in_vbus = false;
	sgm->usb_limit_flag = false;
	queue_delayed_work(system_wq, &sgm->update_charge_status_work,
			   msecs_to_jiffies(10000));

	sgm->fiio_vbus_nb.notifier_call = sgm41510_vbus_fiio_notifier;
	fiio_register_client(notifier_vbus_detection_fiio, &sgm->fiio_vbus_nb);

	sgm_printk("%s probe sucessful!\n", __func__);

	return 0;

irq_fail:
	if (!IS_ERR_OR_NULL(sgm->usb_phy))
		usb_unregister_notifier(sgm->usb_phy, &sgm->usb_nb);

	return ret;
}

static int sgm41510_remove(struct i2c_client *client)
{
	struct sgm41510_device *sgm = i2c_get_clientdata(client);

	power_supply_unregister(sgm->charger);

	if (!IS_ERR_OR_NULL(sgm->usb_phy))
		usb_unregister_notifier(sgm->usb_phy, &sgm->usb_nb);

	/* reset all registers to default values */
	sgm41510_chip_reset(sgm);

	cancel_delayed_work(&sgm->update_charge_status_work);
	cancel_delayed_work(&sgm->usb_limit_work);
	sysfs_remove_group(&sgm->dev->kobj, &sgm41510_attr_group);

	return 0;
}

static void sgm41510_shutdown(struct i2c_client *client)
{
	struct sgm41510_device *sgm = i2c_get_clientdata(client);
	int ret;

	ret = sgm41510_otg_en(sgm, false);
	if (ret < 0)
		sgm_printk("%s enable otg failed.\n", __func__);
}

#ifdef CONFIG_PM_SLEEP
static int sgm41510_suspend(struct device *dev)
{
	struct sgm41510_device *sgm = dev_get_drvdata(dev);

	/*
	 * If charger is removed, while in suspend, make sure ADC is diabled
	 * since it consumes slightly more power.
	 */
	return sgm41510_field_write(sgm, F_CONV_START, 0);
}

static int sgm41510_resume(struct device *dev)
{
	int ret;
	struct sgm41510_state state;
	struct sgm41510_device *sgm = dev_get_drvdata(dev);

	ret = sgm41510_get_chip_state(sgm, &state);
	if (ret < 0)
		return ret;

	mutex_lock(&sgm->lock);
	sgm->state = state;
	mutex_unlock(&sgm->lock);

	/* Re-enable ADC only if charger is plugged in. */
	if (state.online) {
		ret = sgm41510_field_write(sgm, F_CONV_START, 1);
		if (ret < 0)
			return ret;
	}

	/* signal userspace, maybe state changed while suspended */
	power_supply_changed(sgm->charger);

	return 0;
}
#endif

static const struct dev_pm_ops sgm41510_pm = {
	SET_SYSTEM_SLEEP_PM_OPS(sgm41510_suspend, sgm41510_resume)
};

static const struct i2c_device_id sgm41510_i2c_ids[] = {
	{ "sgm41510", 0 },
	{},
};
MODULE_DEVICE_TABLE(i2c, sgm41510_i2c_ids);

static const struct of_device_id sgm41510_of_match[] = {
	{ .compatible = "sgm,sgm41510", },
	{ },
};
MODULE_DEVICE_TABLE(of, sgm41510_of_match);

static const struct acpi_device_id sgm41510_acpi_match[] = {
	{"SGM41510", 0},
	{},
};
MODULE_DEVICE_TABLE(acpi, sgm41510_acpi_match);

static struct i2c_driver sgm41510_driver = {
	.driver = {
		.name = "sgm41510-charger",
		.of_match_table = of_match_ptr(sgm41510_of_match),
		.acpi_match_table = ACPI_PTR(sgm41510_acpi_match),
		.pm = &sgm41510_pm,
	},
	.probe = sgm41510_probe,
	.remove = sgm41510_remove,
	.shutdown = sgm41510_shutdown,
	.id_table = sgm41510_i2c_ids,
};
module_i2c_driver(sgm41510_driver);

MODULE_AUTHOR("Allen <allen_qin@sg-micro.com>");
MODULE_DESCRIPTION("sgm41510 charger driver");
MODULE_LICENSE("GPL");
