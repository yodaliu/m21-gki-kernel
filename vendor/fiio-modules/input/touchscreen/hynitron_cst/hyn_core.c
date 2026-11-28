#include "hyn_core.h"
#define HYN_DRIVER_NAME  "hyn_ts"

static struct hyn_ts_data *hyn_data = NULL;
static const struct hyn_ts_fuc* hyn_fun = NULL;
static const struct of_device_id hyn_of_match_table[] = {
    {.compatible = "hyn,66xx", .data = &cst66xx_fuc,},   /*suport 36xx 35xx 66xx 68xx 148E*/
	{.compatible = "hyn,36xxes", .data = &cst36xxes_fuc,}, /*suport 154es 3654es 3640es*/
    {.compatible = "hyn,3240", .data = &cst3240_fuc,},   /*suport 3240 */
    {.compatible = "hyn,923xx", .data = &cst923xx_fuc,},
    {.compatible = "hyn,3xx",  .data = &cst3xx_fuc,},    /*suport 340 348 328 128 140 148*/
    {.compatible = "hyn,7xx",  .data = &cst7xx_fuc,},    /*suport 726 826 836u*/
    {.compatible = "hyn,8xxt", .data = &cst8xxT_fuc,},   /*suport 816t 816d 820 08C*/
    {.compatible = "hyn,226se", .data = &cst226se_fuc,}, /*suport 226se 8922*/
    {.compatible = "hyn,76xx", .data = &cst76xx_fuc,},
    {.compatible = "hyn,840u", .data = &cst840u_fuc,},
    {},
};
MODULE_DEVICE_TABLE(of, hyn_of_match_table);

static int hyn_check_ic(struct hyn_ts_data *ts_data)
{
    const struct of_device_id *of_dev;
    of_dev = of_match_device(hyn_of_match_table,ts_data->dev);
	if (!of_dev){
        return -1;
    }
    hyn_fun = of_dev->data;
    if(IS_ERR_OR_NULL(hyn_fun) || IS_ERR_OR_NULL(hyn_fun->tp_chip_init) 
        || hyn_fun->tp_chip_init(ts_data)){
        return -2;
    }
    ts_data->hyn_fuc_used = hyn_fun;
    return 0;
}

static int hyn_parse_dt(struct hyn_ts_data *ts_data)
{
    int ret = 0;
    struct device *dev = ts_data->dev;
    struct hyn_plat_data* dt = &ts_data->plat_data;
    HYN_ENTER();
    if(dev->of_node){
        u32 buf[8];
        struct device_node *np = dev->of_node;
//vcc supply
        dt->vdd_i2c = NULL;
        dt->vdd_ana = NULL;
        if(of_property_read_bool(np,"vdd_ana-supply")){
           dt->vdd_ana = regulator_get(dev, "vdd_ana");
            if(IS_ERR_OR_NULL(dt->vdd_ana)){
                dt->vdd_ana = NULL;
                HYN_ERROR("regulator_get vdd_ana failed");
            }
        }
        else{
            HYN_INFO("vdd_ana not config");
        }

//iovcc supply
        if(of_property_read_bool(np,"vcc_i2c-supply")){
            dt->vdd_i2c = regulator_get(dev, "vcc_i2c");
            if(IS_ERR_OR_NULL(dt->vdd_i2c)){
                dt->vdd_i2c = NULL;
                HYN_ERROR("regulator_get vdd_i2c failed");
            }
        }
        else{
            HYN_INFO("vdd_i2c not config");
        }

//gpio info
        // dt->reset_gpio = of_get_named_gpio_flags(np, "reset-gpio", 0, &dt->reset_gpio_flags);
        // dt->irq_gpio = of_get_named_gpio_flags(np, "irq-gpio", 0, &dt->irq_gpio_flags);

        dt->reset_gpio = of_get_named_gpio_flags(np, "reset-gpios", 0, &dt->reset_gpio_flags);
        dt->irq_gpio = of_get_named_gpio_flags(np, "irq-gpios", 0, &dt->irq_gpio_flags);

        if(dt->reset_gpio < 0 || dt->irq_gpio < 0){
            HYN_ERROR("dts get gpio failed");
            return -ENODEV;
        }
        else{
            HYN_INFO("reset_gpio:%d irq_gpio:%d",dt->reset_gpio,dt->irq_gpio);
        }

//pin_ctl
        ret =-1;
        dt->pinctl = devm_pinctrl_get(dev);
        if (!IS_ERR_OR_NULL(dt->pinctl)){
            dt->pin_active = pinctrl_lookup_state(dt->pinctl, "ts_active");
            dt->pin_suspend= pinctrl_lookup_state(dt->pinctl, "ts_suspend");
            if(!IS_ERR_OR_NULL(dt->pin_active) && !IS_ERR_OR_NULL(dt->pin_suspend)){
                ret = 0;
            }
        }
        if(ret){
            HYN_INFO("pinctrl not config or can't find \"ts-active\" \"ts_suspend\"");
            dt->pinctl = NULL;
            dt->pin_active = NULL;
            dt->pin_suspend= NULL;
        }

// point info
        ret = of_property_read_u32(np, "max-touch-number", &dt->max_touch_num);
        ret |= of_property_read_u32(np, "pos-swap", &dt->swap_xy);
        ret |= of_property_read_u32(np, "posx-reverse", &dt->reverse_x);
        ret |= of_property_read_u32(np, "posy-reverse", &dt->reverse_y);
        ret |= of_property_read_u32_array(np, "display-coords", buf, 4);
        dt->x_resolution = buf[2];
        dt->y_resolution = buf[3];
        HYN_INFO("dts x_res = %d,y_res = %d,touch-number = %d swap_xy = %d reversex = %d reversey = %d",
                    dt->x_resolution,dt->y_resolution,dt->max_touch_num,dt->swap_xy,dt->reverse_x,dt->reverse_y);
        if(ret < 0){
            HYN_ERROR("dts get screen failed");
            return -EINVAL;
        }

// key info
        ret = of_property_read_u32(np, "key-number", &dt->key_num);
        if(ret>=0 && dt->key_num && dt->key_num<=8){
            ret |= of_property_read_u32(np, "key-y-coord", &dt->key_y_coords);
            ret |= of_property_read_u32_array(np, "key-x-coords", dt->key_x_coords, dt->key_num);
            ret |= of_property_read_u32_array(np, "keys", dt->key_code, dt->key_num);
            if(ret < 0){
                HYN_ERROR("dts get screen failed");
                return -EINVAL;
            }
        }
        else{
            HYN_INFO("key not config");
            dt->key_num = 0;
        }
        return 0;
    }
    else{
        HYN_ERROR("dts match failed");
        return -ENODEV;
    }
}

static int hyn_poweron(struct hyn_ts_data *ts_data)
{
    int ret = 0;
    struct hyn_plat_data* dt = &ts_data->plat_data;
    if(!IS_ERR_OR_NULL(hyn_data->plat_data.pinctl)){
        if(pinctrl_select_state(dt->pinctl, dt->pin_active)){
            HYN_ERROR("pin active set failed");
        }
    }

    ret = gpio_request(dt->irq_gpio, "hyn_irq_gpio");
    ret |= gpio_request(dt->reset_gpio, "hyn_reset_gpio");
    if(ret < 0){
        HYN_ERROR("gpio_request failed");
        goto GPIO_SET_FAILE;
    }
    
    ret = gpio_direction_input(dt->irq_gpio);
    ret |= gpio_direction_output(dt->reset_gpio, 0);
    if(ret < 0){
        HYN_ERROR("set gpio_direction failed");
        goto GPIO_SET_FAILE;
    }

    if(!IS_ERR_OR_NULL(dt->vdd_ana)){
        if(regulator_count_voltages(dt->vdd_ana) > 0){
            if(regulator_set_voltage(dt->vdd_ana, 2800000, 3300000)){
                HYN_ERROR("set voltage vdd_ana failed");
            }
        }
    }

    if(!IS_ERR_OR_NULL(dt->vdd_i2c)){
        if(regulator_count_voltages(dt->vdd_i2c) > 0){
            if(regulator_set_voltage(dt->vdd_i2c, 1800000, 1800000)){
                HYN_ERROR("set voltage vdd_i2c failed");
            }
        }
    }
    if(hyn_power_source_ctrl(hyn_data, 1)){
        HYN_ERROR("enable power failed");
    }
    mdelay(5);
    gpio_set_value(dt->reset_gpio, 1);
    return 0;
GPIO_SET_FAILE:
    return ret;
}

static int hyn_input_dev_init(struct hyn_ts_data *ts_data)
{
    int key_num = 0;//,ret =0;
    struct hyn_plat_data *dt = &ts_data->plat_data;
    struct input_dev *input_dev;

    HYN_ENTER(); 
    input_dev = input_allocate_device();
    if (!input_dev) {
        HYN_ERROR("Failed to allocate memory for input device");
        return -ENOMEM;
    }
    input_dev->name = HYN_DRIVER_NAME;
    input_dev->id.bustype = ts_data->bus_type;
    input_dev->dev.parent = ts_data->dev;
    input_set_drvdata(input_dev, ts_data);

    __set_bit(EV_SYN, input_dev->evbit);
    __set_bit(EV_ABS, input_dev->evbit);
    __set_bit(EV_KEY, input_dev->evbit);
    __set_bit(BTN_TOUCH, input_dev->keybit);
    __set_bit(INPUT_PROP_DIRECT, input_dev->propbit);

    // INPUT_PROP_POINTER
    // __set_bit(INPUT_PROP_POINTER, input_dev->propbit);

    for (key_num = 0; key_num < dt->key_num; key_num++)
			input_set_capability(input_dev, EV_KEY, dt->key_code[key_num]);
#if HYN_MT_PROTOCOL_B_EN
    set_bit(BTN_TOOL_FINGER,input_dev->keybit);
    //input_mt_init_slots(input_dev, dt->max_touch_num+1);
    input_mt_init_slots(input_dev, dt->max_touch_num+1, INPUT_MT_DIRECT);
#else
    input_dev->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH);
#endif

	input_set_abs_params(input_dev, ABS_MT_TRACKING_ID, 0,  dt->max_touch_num, 0, 0); 
	input_set_abs_params(input_dev, ABS_MT_POSITION_X, 0, dt->x_resolution,0, 0);
	input_set_abs_params(input_dev, ABS_MT_POSITION_Y, 0, dt->y_resolution,0, 0);
	input_set_abs_params(input_dev, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_WIDTH_MAJOR, 0, 200, 0, 0);
    input_set_abs_params(input_dev, ABS_MT_PRESSURE, 0, 0xFF, 0, 0);

    ts_data->input_dev = input_dev;

    return 0;
}

static void release_all_finger(struct hyn_ts_data *ts_data)
{
    HYN_ENTER();
    if(ts_data->report_id_flg){
#if HYN_MT_PROTOCOL_B_EN
        u8 i;
        for(i=0; i< ts_data->plat_data.max_touch_num; i++) {
            if(ts_data->report_id_flg & (1UL<<i)){
                // HYN_INFO("release %d",i);
                input_mt_slot(ts_data->input_dev, i);
                input_report_abs(ts_data->input_dev, ABS_MT_TRACKING_ID, -1);
                input_mt_report_slot_state(ts_data->input_dev, MT_TOOL_FINGER, false);
            }	
        }
        input_report_key(ts_data->input_dev, BTN_TOUCH, 0);
#else
        input_report_key(ts_data->input_dev, BTN_TOUCH, 0);
        input_mt_sync(ts_data->input_dev);
#endif
    }
    ts_data->report_id_flg = 0;
}

static void touch_updata(u8 idx,u8 event)
{
    struct ts_frame *rep_frame = &hyn_data->rp_buf;
    struct input_dev *dev = hyn_data->input_dev;
    u16 zpress = rep_frame->pos_info[idx].pres_z;

    if(event){
        if(zpress < 10){
            zpress += (10+(rep_frame->pos_info[idx].pos_x&0x03));
        }
        hyn_data->report_id_flg |= (1UL<< rep_frame->pos_info[idx].pos_id);
    }
    else{
        hyn_data->report_id_flg &= ~(1UL<< rep_frame->pos_info[idx].pos_id);
    }
#if HYN_MT_PROTOCOL_B_EN
    if(event){
        input_report_key(dev, BTN_TOUCH, 1);
        input_mt_slot(dev, rep_frame->pos_info[idx].pos_id);
        input_mt_report_slot_state(dev, MT_TOOL_FINGER, 1);
        input_report_abs(dev, ABS_MT_TRACKING_ID, rep_frame->pos_info[idx].pos_id);
        input_report_abs(dev, ABS_MT_POSITION_X, rep_frame->pos_info[idx].pos_x);
        input_report_abs(dev, ABS_MT_POSITION_Y, rep_frame->pos_info[idx].pos_y);
        input_report_abs(dev, ABS_MT_TOUCH_MAJOR, zpress>>3);
        input_report_abs(dev, ABS_MT_WIDTH_MAJOR, zpress>>3);
        input_report_abs(dev, ABS_MT_PRESSURE, zpress);
    }
    else{
        input_mt_slot(dev, rep_frame->pos_info[idx].pos_id);
        input_report_abs(dev, ABS_MT_TRACKING_ID, -1);
        input_mt_report_slot_state(dev, MT_TOOL_FINGER, 0);
    }
#else
    if(event){
        input_report_key(dev, BTN_TOUCH, 1);
        input_report_abs(dev, ABS_MT_PRESSURE, zpress);
        input_report_abs(dev, ABS_MT_TRACKING_ID, rep_frame->pos_info[idx].pos_id);
        input_report_abs(dev, ABS_MT_TOUCH_MAJOR, zpress>>3);
        // input_report_abs(dev, ABS_MT_WIDTH_MAJOR, zpress>>3);
        input_report_abs(dev, ABS_MT_POSITION_X, rep_frame->pos_info[idx].pos_x);
        input_report_abs(dev, ABS_MT_POSITION_Y, rep_frame->pos_info[idx].pos_y);
        input_mt_sync(dev);
    }
#endif
}

static void hyn_irq_report_work(struct work_struct *work)
{
    struct hyn_ts_data *ts_data = hyn_data;
    struct ts_frame *rep_frame = &hyn_data->rp_buf;
    struct input_dev *dev = hyn_data->input_dev;
    struct hyn_plat_data *dt = &hyn_data->plat_data;
    u16 xpos,ypos;
    int reprot_state_clr = 0;
    int ret, retry;
    for(retry = 0; retry < 3; retry++){
        ret = hyn_fun->tp_report();
        if(ret == 0)
            break;
        HYN_INFO("[GESTURE_DBG] tp_report failed ret=%d retry=%d state_suspend=%d gesture_enable=%d work_mode=%d irq_gpio=%d reset_gpio=%d power=%d",
                 ret, retry, ts_data->state_is_sunpend, ts_data->gesture_is_enable, ts_data->work_mode,
                 gpio_is_valid(dt->irq_gpio) ? gpio_get_value(dt->irq_gpio) : -1,
                 gpio_is_valid(dt->reset_gpio) ? gpio_get_value(dt->reset_gpio) : -1,
                 ts_data->power_is_on);
        if(!(ts_data->state_is_sunpend && ts_data->gesture_is_enable))
            break;
        msleep(5);
    }
    if(ret){
        HYN_INFO("Ignore illegal data");
        return;
    }
    mutex_lock(&ts_data->mutex_report);
    if(rep_frame->report_need & REPORT_KEY){ //key
#if KEY_USED_POS_REPORT
        rep_frame->pos_info[0].pos_id = 0;
        rep_frame->pos_info[0].pos_x = dt->key_x_coords[rep_frame->key_id];
        rep_frame->pos_info[0].pos_y = dt->key_y_coords;
        rep_frame->pos_info[0].pres_z = 100;
        touch_updata(0,rep_frame->key_state ? 1:0);
#else
        input_report_key(dev,dt->key_code[rep_frame->key_id],rep_frame->key_state ? 1:0); 
#endif
        if(rep_frame->key_state==0){
            reprot_state_clr = 1;
        } 
        input_sync(dev);
        HYN_INFO2("report keyid:%d keycode:%d",rep_frame->key_id,dt->key_code[rep_frame->key_id]);
    }

    if(rep_frame->report_need & REPORT_POS){ //pos
        u8 i;
        if(rep_frame->rep_num == 0){
            release_all_finger(ts_data);
            reprot_state_clr = 1;
        }
        else{
            u8 touch_down = 0;
            for(i = 0; i < rep_frame->rep_num; i++){
                HYN_INFO2("id,%d,xy,%d,%d",rep_frame->pos_info[i].pos_id,rep_frame->pos_info[i].pos_x,rep_frame->pos_info[i].pos_y);
                if(dt->swap_xy){
                    xpos = rep_frame->pos_info[i].pos_y;
                    ypos = rep_frame->pos_info[i].pos_x;
                }
                else{
                    xpos = rep_frame->pos_info[i].pos_x;
                    ypos = rep_frame->pos_info[i].pos_y;
                }
                if(ypos > dt->y_resolution || xpos > dt->x_resolution){
                    HYN_ERROR("Please check dts or FW config,maybe resolution or origin issue!!!");
                }
                if(rep_frame->pos_info[i].pos_id >= ts_data->plat_data.max_touch_num){
                    continue;
                }
                if(dt->reverse_x){
                    xpos = dt->x_resolution-xpos;
                }
                if(dt->reverse_y){
                    ypos = dt->y_resolution-ypos;
                }
                rep_frame->pos_info[i].pos_x = xpos;
                rep_frame->pos_info[i].pos_y = ypos;
                touch_updata(i,rep_frame->pos_info[i].event? 1:0);
                if(rep_frame->pos_info[i].event) touch_down++;
            }
            if(touch_down==0){
                reprot_state_clr = 1;
                input_report_key(dev, BTN_TOUCH, 0);
#if HYN_MT_PROTOCOL_B_EN==0
                input_mt_sync(dev);
#endif
            }
        }
        input_sync(dev);
    }
    if(rep_frame->report_need & REPORT_PROX){
        hyn_proximity_report(ts_data->prox_state);
        reprot_state_clr = 1;
    }
#if (HYN_GESTURE_EN)
    if(rep_frame->report_need & REPORT_GES){
        hyn_gesture_report(ts_data);
        reprot_state_clr = 1;
    }
#endif 
    if(reprot_state_clr){
        rep_frame->report_need = REPORT_NONE;
    }
    mutex_unlock(&ts_data->mutex_report);
}

static int hyn_restore_scene(void)
{
    int ret = 0;
    HYN_ENTER();
    if(hyn_data->prox_is_enable){
        ret |= hyn_fun->tp_prox_handle(1);
    }
    else if(hyn_data->gesture_is_enable && hyn_data->state_is_sunpend){
        ret |= hyn_fun->tp_set_workmode(GESTURE_MODE,1);
    }
    if(hyn_data->charge_is_enable){
        ret |= hyn_fun->tp_set_workmode(CHARGE_ENTER,1);
    }
    if(hyn_data->glove_is_enable){
        ret |= hyn_fun->tp_set_workmode(GLOVE_ENTER,1);
    }
    return ret;
}

static void hyn_esdcheck_work(struct work_struct *work)
{
#if ESD_CHECK_EN
    int ret;
    HYN_ENTER(); 
    if(hyn_data->esd_block_cnt==0){
        ret = hyn_fun->tp_check_esd();
        HYN_INFO("esd:%04x",ret);
        if(hyn_data->esd_last_value != ret){
            hyn_data->esd_fail_cnt = 0;
            hyn_data->esd_last_value = ret;
        }
        else{
            hyn_data->esd_fail_cnt++;
            if(hyn_data->esd_fail_cnt > 2){
                hyn_data->esd_fail_cnt = 0;
                hyn_power_source_ctrl(hyn_data,0);
                mdelay(1);
                hyn_power_source_ctrl(hyn_data,1);
                hyn_fun->tp_rest();
                hyn_restore_scene();
            }
        }
    }
    else{
        hyn_data->esd_block_cnt--;
    }
    queue_delayed_work(hyn_data->hyn_workqueue, &hyn_data->esdcheck_work,
                           msecs_to_jiffies(1000));
#endif
}

static void hyn_irq_wake_set(struct hyn_ts_data *ts_data, u8 enable)
{
    int ret;
    if(ts_data->irq_wake_enabled == enable){
        return;
    }
    ret = irq_set_irq_wake(ts_data->gpio_irq, enable);
    if(ret){
        HYN_ERROR("set irq wake %d failed, ret=%d", enable, ret);
        return;
    }
    ts_data->irq_wake_enabled = enable;
    HYN_INFO("irq wake %s", enable ? "enabled" : "disabled");
}

static void hyn_gesture_debug_dump(const char *tag)
{
    int irq_val, rst_val;
    struct hyn_ts_data *ts_data = hyn_data;
    if(IS_ERR_OR_NULL(ts_data)){
        return;
    }
    irq_val = gpio_is_valid(ts_data->plat_data.irq_gpio) ? gpio_get_value(ts_data->plat_data.irq_gpio) : -1;
    rst_val = gpio_is_valid(ts_data->plat_data.reset_gpio) ? gpio_get_value(ts_data->plat_data.reset_gpio) : -1;
    pr_debug("[HYN][GESTURE_DBG] %s irq_gpio=%d reset_gpio=%d\n", tag, irq_val, rst_val);
    if(!IS_ERR_OR_NULL(hyn_fun) && !IS_ERR_OR_NULL(hyn_fun->tp_gesture_debug)){
        hyn_fun->tp_gesture_debug(tag);
    }
}

static void hyn_cancel_gesture_debug_work(struct hyn_ts_data *ts_data)
{
    if(IS_ERR_OR_NULL(ts_data->hyn_workqueue)){
        return;
    }
    cancel_delayed_work_sync(&ts_data->gesture_debug_work_1s);
    cancel_delayed_work_sync(&ts_data->gesture_debug_work_3s);
}

static void hyn_queue_gesture_debug_work(struct hyn_ts_data *ts_data)
{
    if(IS_ERR_OR_NULL(ts_data) || IS_ERR_OR_NULL(ts_data->hyn_workqueue)
        || IS_ERR_OR_NULL(hyn_fun) || IS_ERR_OR_NULL(hyn_fun->tp_gesture_debug)){
        return;
    }
    cancel_delayed_work(&ts_data->gesture_debug_work_1s);
    cancel_delayed_work(&ts_data->gesture_debug_work_3s);
    queue_delayed_work(ts_data->hyn_workqueue, &ts_data->gesture_debug_work_1s, msecs_to_jiffies(1000));
    queue_delayed_work(ts_data->hyn_workqueue, &ts_data->gesture_debug_work_3s, msecs_to_jiffies(3000));
}

static void hyn_gesture_debug_work_1s(struct work_struct *work)
{
    hyn_gesture_debug_dump("1s");
}

static void hyn_gesture_debug_work_3s(struct work_struct *work)
{
    hyn_gesture_debug_dump("3s");
}

static void hyn_resum(struct device *dev)
{
    struct ts_frame *rep_frame;
    struct hyn_plat_data *dt;
    HYN_ENTER();
    if(IS_ERR_OR_NULL(hyn_data)){
        return;
    }
    hyn_cancel_gesture_debug_work(hyn_data);
    hyn_irq_wake_set(hyn_data, 0);
    hyn_data->state_is_sunpend = 0;
    rep_frame = &hyn_data->rp_buf;
    dt = &hyn_data->plat_data;
#if (HYN_WAKE_LOCK_EN==1)
    wake_unlock(&hyn_data->tp_wakelock);
#endif
    if(!IS_ERR_OR_NULL(dt->pinctl)){
      pinctrl_select_state(dt->pinctl, dt->pin_active);
    }
    hyn_power_source_ctrl(hyn_data, 1);
    hyn_fun->tp_resum();
    //restore_scene
    hyn_restore_scene();
    rep_frame->report_need = REPORT_NONE;
    hyn_irq_set(hyn_data,ENABLE);
}

static void hyn_suspend(struct device *dev)
{
    int ret = 0;
    HYN_ENTER();
    if(IS_ERR_OR_NULL(hyn_data)){
        return;
    }
    hyn_data->state_is_sunpend = 1;
#if (HYN_WAKE_LOCK_EN==1)
    wake_lock(&hyn_data->tp_wakelock);
#endif
    if(hyn_data->prox_is_enable ==1){
        hyn_irq_wake_set(hyn_data, 1);
    }
    else if(hyn_data->gesture_is_enable){
        hyn_irq_set(hyn_data,DISABLE);
        ret = hyn_fun->tp_set_workmode(GESTURE_MODE,1);
        if(ret == 0){
            hyn_queue_gesture_debug_work(hyn_data);
        }
        hyn_irq_wake_set(hyn_data, 1);
        hyn_irq_set(hyn_data,ENABLE);
    }
    else{
        hyn_irq_wake_set(hyn_data, 0);
        hyn_irq_set(hyn_data,DISABLE);
        hyn_fun->tp_supend();
        if(!IS_ERR_OR_NULL(hyn_data->plat_data.pinctl)){
            pinctrl_select_state(hyn_data->plat_data.pinctl, hyn_data->plat_data.pin_suspend);
        }
    }
    release_all_finger(hyn_data);
    input_sync(hyn_data->input_dev);
}

static void hyn_updata_fw_work(struct work_struct *work)
{
    int ret = 0;
    if(!IS_ERR_OR_NULL(hyn_data->fw_updata_addr)){
        ret = hyn_fun->tp_updata_fw(hyn_data->fw_updata_addr,hyn_data->fw_updata_len);
    }
    else{
        HYN_ERROR("fw_updata_addr is erro");
    }
}

static void hyn_resum_work(struct work_struct *work)
{
    if(IS_ERR_OR_NULL(hyn_data)){
        return;
    }
    hyn_resum(hyn_data->dev);
}

static irqreturn_t hyn_irq_handler(int irq, void *data)
{
	atomic_set(&hyn_data->hyn_irq_flg,1);
    HYN_INFO("[GESTURE_DBG] irq=%d state_suspend=%d gesture_enable=%d work_mode=%d irq_gpio=%d",
             irq, hyn_data->state_is_sunpend, hyn_data->gesture_is_enable, hyn_data->work_mode,
             gpio_is_valid(hyn_data->plat_data.irq_gpio) ? gpio_get_value(hyn_data->plat_data.irq_gpio) : -1);
    if(hyn_data->state_is_sunpend && hyn_data->gesture_is_enable){
        HYN_INFO("[GESTURE_DBG] pm_wakeup_event 500ms");
        pm_wakeup_event(hyn_data->dev, 500);
    }
    if(hyn_data->work_mode < DIFF_MODE){
        // queue_work(hyn_data->hyn_workqueue,&hyn_data->work_report);
        hyn_data->esd_block_cnt = 2;
        flush_workqueue(hyn_data->hyn_workqueue);
        hyn_irq_report_work(NULL);
    }
    else{
        wake_up(&hyn_data->wait_irq);
    }
    return IRQ_HANDLED;
}

/* FiiO: the panel is picked and powered through ts_core, which also drives suspend/resume */
static struct ts_device hyn_ts_device;

static int hyn_ts_remove(struct i2c_client *client);
static int hyn_ts_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
    int ret = 0;
    u16 bus_type;
    struct hyn_ts_data *ts_data = 0;

    HYN_ENTER();
    HYN_INFO(HYN_DRIVER_VERSION);
    if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
        HYN_ERROR("I2C not supported");
        return -ENODEV;
    }
    bus_type = BUS_I2C;
    if (try_to_ping(client, &hyn_ts_device)) {
        HYN_INFO("not this touchscreen");
        return -ENXIO;
    }
    if(!IS_ERR_OR_NULL(hyn_data)){
        HYN_ERROR("other dev is insmode");
        return -ENOMEM;
    }
    ts_data = kzalloc(sizeof(*ts_data), GFP_KERNEL);
    if (!ts_data) {
        HYN_ERROR("alloc ts data failed");
        return -ENOMEM;
    }
    ts_data->bus_type = bus_type;
    ts_data->rp_buf.key_id = 0xFF;
    ts_data->work_mode = NOMAL_MODE;
    ts_data->log_level = 0;

    hyn_data = ts_data;
    ts_data->client = client;
    ts_data->dev = &client->dev;
    dev_set_drvdata(ts_data->dev, ts_data);

    ret = hyn_parse_dt(ts_data);
    if(ret){
        HYN_ERROR("hyn_parse_dt failed");
        goto FREE_RESOURCE;
    }

    ret = hyn_poweron(ts_data);
    if(ret){
        HYN_ERROR("hyn_poweron failed");
        goto FREE_RESOURCE;
    }
    // spin_lock_init(&ts_data->irq_lock);
    mutex_init(&ts_data->mutex_report);
    mutex_init(&ts_data->mutex_bus);
    mutex_init(&ts_data->mutex_fs);
    init_waitqueue_head(&ts_data->wait_irq);

    ret = hyn_check_ic(ts_data);
    if(ret){
        HYN_ERROR("hyn_check_ic failed, retry after 3s");
        msleep(3000);
        ret = hyn_check_ic(ts_data);
        if(ret){
            HYN_ERROR("hyn_check_ic failed again, retry after 5s");
            msleep(5000);
            ret = hyn_check_ic(ts_data);
            if(ret){
                HYN_ERROR("hyn_check_ic failed finally");
                goto FREE_RESOURCE;
            }
        }
    }

    INIT_WORK(&ts_data->work_report,hyn_irq_report_work);
    INIT_WORK(&ts_data->work_updata_fw,hyn_updata_fw_work);
    INIT_WORK(&ts_data->work_resume,hyn_resum_work);
    INIT_DELAYED_WORK(&ts_data->esdcheck_work,hyn_esdcheck_work);
    INIT_DELAYED_WORK(&ts_data->gesture_debug_work_1s,hyn_gesture_debug_work_1s);
    INIT_DELAYED_WORK(&ts_data->gesture_debug_work_3s,hyn_gesture_debug_work_3s);

    ts_data->hyn_workqueue = create_singlethread_workqueue("hyn_wq");
    if (IS_ERR_OR_NULL(ts_data->hyn_workqueue)){
        HYN_ERROR("create work queue failed");
        goto FREE_RESOURCE;
    }

    ret = hyn_input_dev_init(ts_data);
    if(ret){
        if(!IS_ERR_OR_NULL(ts_data->input_dev)){
            input_set_drvdata(ts_data->input_dev, NULL);
            input_free_device(ts_data->input_dev);
            ts_data->input_dev = NULL;
        }
        HYN_ERROR("hyn_input_dev_init failed");
        goto FREE_RESOURCE;
    }
    ret = input_register_device(ts_data->input_dev);
    if(ret){
        HYN_ERROR("input_register_device failed");
        goto FREE_RESOURCE;
    }

#if (HYN_WAKE_LOCK_EN==1)
    wake_lock_init(&ts_data->tp_wakelock, WAKE_LOCK_SUSPEND, "suspend_tp_lock");
#endif

#if (HYN_GESTURE_EN)
    ret = hyn_gesture_init(ts_data);
    if(ret){
        HYN_ERROR("gesture_init failed");
        goto FREE_RESOURCE;
    }
#endif

    ts_data->gpio_irq =  gpio_to_irq(ts_data->plat_data.irq_gpio);
    ts_data->client->irq = ts_data->gpio_irq;
    ts_data->plat_data.irq_gpio_flags = (IRQF_TRIGGER_FALLING | IRQF_ONESHOT);
    ret = request_threaded_irq(ts_data->gpio_irq, NULL, hyn_irq_handler,
                                hyn_data->plat_data.irq_gpio_flags, HYN_DRIVER_NAME, ts_data);
    if(ret){
        HYN_ERROR("request_threaded_irq failed");
        goto FREE_RESOURCE;
    }
    atomic_set(&ts_data->irq_is_disable,ENABLE);
    hyn_irq_set(ts_data , DISABLE);
    ts_data->irq_wake_enabled = 0;
    device_init_wakeup(ts_data->dev, 1);

    hyn_create_sysfs(ts_data);
#if (HYN_APK_DEBUG_EN)
    hyn_tool_fs_int(ts_data);
#endif
    hyn_proximity_int(ts_data);
    hyn_irq_set(ts_data,ENABLE);
    hyn_esdcheck_switch(ts_data,ENABLE);

#if HYN_POWER_ON_UPDATA
    if(ts_data->need_updata_fw){
        queue_work(ts_data->hyn_workqueue,&ts_data->work_updata_fw);
    }
#endif
    return 0;
FREE_RESOURCE:
    hyn_ts_remove(client);
    return -1;
}


static int hyn_ts_remove(struct i2c_client *client)
{
    struct hyn_ts_data *ts_data = hyn_data;
    HYN_ENTER();
    if(!IS_ERR_OR_NULL(ts_data)){
        if (ts_data->hyn_workqueue){
            hyn_cancel_gesture_debug_work(ts_data);
            flush_workqueue(ts_data->hyn_workqueue);
            hyn_esdcheck_switch(ts_data,DISABLE);
            destroy_workqueue(ts_data->hyn_workqueue);
        }
        HYN_INFO("ts_remove1");
#if (HYN_APK_DEBUG_EN)
        hyn_tool_fs_exit();
#endif
        hyn_proximity_exit();
#if (HYN_GESTURE_EN)
        hyn_gesture_exit(ts_data);
#endif
        hyn_release_sysfs(ts_data);
        HYN_INFO("ts_remove2");
        if(ts_data->gpio_irq != 0){
            hyn_irq_wake_set(ts_data, 0);
            device_init_wakeup(ts_data->dev, 0);
            free_irq(ts_data->gpio_irq, ts_data);
        }
        HYN_INFO("ts_remove3");
        if(!IS_ERR_OR_NULL(ts_data->input_dev)){
            input_unregister_device(ts_data->input_dev);
        }
        HYN_INFO("ts_remove4");
        if(gpio_is_valid(ts_data->plat_data.irq_gpio))
            gpio_free(ts_data->plat_data.irq_gpio);
        if(gpio_is_valid(ts_data->plat_data.reset_gpio))
            gpio_free(ts_data->plat_data.reset_gpio);
        HYN_INFO("ts_remove5");
        hyn_power_source_ctrl(hyn_data, 0);
        if(!IS_ERR_OR_NULL(ts_data->plat_data.vdd_ana)){
            regulator_put(ts_data->plat_data.vdd_ana);
        }
        if(!IS_ERR_OR_NULL(ts_data->plat_data.vdd_i2c)){
            regulator_put(ts_data->plat_data.vdd_i2c);
        }
        HYN_INFO("ts_remove6");
        kfree(ts_data);
        hyn_data = NULL;
        if(hyn_ts_device.used){
            ts_core_set_vaild_tp(&hyn_ts_device, false);
            hyn_ts_device.used = false;
        }
        HYN_INFO("ts_remove7");
    }
    return 0;
}

static int hyn_device_init(struct ts_device *_ts_device)
{
    int ret = 0;
    _ts_device->vcc_i2c = regulator_get(_ts_device->dev, _ts_device->tp_vcc_name);
    if(IS_ERR_OR_NULL(_ts_device->vcc_i2c)){
        HYN_ERROR("regulator_get %s failed", _ts_device->tp_vcc_name);
        _ts_device->vcc_i2c = NULL;
    }
    else if(regulator_count_voltages(_ts_device->vcc_i2c) > 0){
        ret = regulator_set_voltage(_ts_device->vcc_i2c, 1800000, 1800000);
        if(ret){
            HYN_ERROR("set voltage %s failed, ret=%d", _ts_device->tp_vcc_name, ret);
        }
    }
    if(gpio_is_valid(_ts_device->reset_gpio)){
        ret = gpio_direction_output(_ts_device->reset_gpio, 0);
        if(ret){
            HYN_ERROR("reset gpio direction failed, ret=%d", ret);
        }
    }
    return ret;
}

static void hyn_detect_reset_ctrl(struct ts_device *_ts_device, int enable)
{
    if(!gpio_is_valid(_ts_device->reset_gpio)){
        return;
    }
    gpio_direction_output(_ts_device->reset_gpio, 0);
    if(enable){
        msleep(5);
        gpio_direction_output(_ts_device->reset_gpio, 1);
        msleep(80);
    }
}

static int hyn_ping_device(struct ts_device *_ts_device)
{
    struct i2c_adapter *adapter;
    u8 boot_cmd[3] = {0xA0, 0x01, 0xA8};
    u8 id_reg[2] = {0xA0, 0x02};
    u8 id_buf[2] = {0};
    struct i2c_msg boot_msg = {
        .addr = _ts_device->addr,
        .flags = 0,
        .len = 3,
        .buf = boot_cmd,
    };
    struct i2c_msg id_msgs[2] = {
        {
            .addr = _ts_device->addr,
            .flags = 0,
            .len = 2,
            .buf = id_reg,
        },
        {
            .addr = _ts_device->addr,
            .flags = I2C_M_RD,
            .len = 2,
            .buf = id_buf,
        },
    };
    int i, j;

    adapter = i2c_get_adapter(_ts_device->i2c_adapter);
    if(!adapter){
        HYN_ERROR("i2c_get_adapter %d failed", _ts_device->i2c_adapter);
        return -ENODEV;
    }
    for(i = 0; i < 20; i++){
        if(gpio_is_valid(_ts_device->reset_gpio)){
            gpio_direction_output(_ts_device->reset_gpio, 0);
            msleep(8);
            gpio_direction_output(_ts_device->reset_gpio, 1);
        }
        msleep(12 + i);
        if(i2c_transfer(adapter, &boot_msg, 1) != 1){
            continue;
        }
        for(j = 0; j < 10; j++){
            if(i2c_transfer(adapter, id_msgs, 2) == 2 && id_buf[0] == 0x22 && id_buf[1] == 0xDD){
                HYN_INFO("cst66xx boot ping success");
                i2c_put_adapter(adapter);
                return 0;
            }
            msleep(2);
        }
    }
    i2c_put_adapter(adapter);
    return -ENODEV;
}

static void hyn_ts_gesture_enable(bool enable)
{
    if(hyn_data){
        hyn_data->gesture_is_enable = enable;
    }
}

static void hyn_ts_screen_off(bool is_off)
{
    static bool already_in;
    if(already_in || !hyn_data){
        return;
    }
    already_in = true;
    if(is_off){
        hyn_suspend(NULL);
    }
    else{
        hyn_resum(NULL);
    }
    already_in = false;
}

static bool hyn_get_gesture_enable(void)
{
    if(!hyn_data){
        return false;
    }
    return hyn_data->gesture_is_enable;
}

static struct ts_device hyn_ts_device = {
    .device_init = hyn_device_init,
    .reset_ctrl = hyn_detect_reset_ctrl,
    .ping_device = hyn_ping_device,
    .gesture_enable = hyn_ts_gesture_enable,
    .screen_off = hyn_ts_screen_off,
    .get_gesture_enable = hyn_get_gesture_enable,
};

static const struct i2c_device_id hyn_id_table[] = {
    {.name = HYN_DRIVER_NAME, .driver_data = 0,},
    {},
};

static struct i2c_driver hyn_ts_driver = {
    .probe = hyn_ts_probe,
    .remove = hyn_ts_remove,
    .driver = {
        .name = HYN_DRIVER_NAME,
        .owner = THIS_MODULE,
        .of_match_table = hyn_of_match_table,
    },
    .id_table = hyn_id_table,
};

static int __init hyn_ts_init(void)
{
    int ret = 0;
    HYN_ENTER();
    ret = i2c_add_driver(&hyn_ts_driver);
    if (ret) {
        HYN_ERROR("add i2c driver failed");
        return -ENODEV;
    }
    return 0;
}

static void __exit hyn_ts_exit(void)
{
    HYN_ENTER();
    i2c_del_driver(&hyn_ts_driver);
}

//late_initcall(hyn_ts_init);
module_init(hyn_ts_init);
module_exit(hyn_ts_exit);

MODULE_AUTHOR("Hynitron Driver Team");
MODULE_DESCRIPTION("Hynitron Touchscreen Driver");
MODULE_LICENSE("GPL v2");
MODULE_SOFTDEP("pre: fiio_primary_modules ts_core");
/* filp_open/kernel_read (copy_for_updata) are namespaced in 5.15.220, not in the stock 5.15.41 */
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
