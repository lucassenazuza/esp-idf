// Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include <esp_types.h>
#include "esp_intr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/xtensa_api.h"
#include "soc/dport_reg.h"
#include "soc/gpio_sig_map.h"
#include "driver/ledc.h"

//TODO: Move these debug options to menuconfig
#define LEDC_DBG_WARING_ENABLE (0)
#define LEDC_DBG_ERROR_ENABLE  (0)
#define LEDC_INFO_ENABLE       (0)
#define LEDC_DBG_ENABLE        (0)

//DBG INFOR
#if LEDC_DBG_ENABLE
#define LEDC_DBG(format,...) do{\
        ets_printf("[dbg][%s#%u]",__FUNCTION__,__LINE__);\
        ets_printf(format,##__VA_ARGS__);\
}while(0)
#else
#define LEDC_DBG(...)
#endif

#if LEDC_INFO_ENABLE
#define LEDC_INFO(format,...) do{\
        ets_printf("[info][%s#%u]",__FUNCTION__,__LINE__);\
        ets_printf(format,##__VA_ARGS__);\
}while(0)
#else
#define LEDC_INFO(...)
#endif

#if LEDC_DBG_WARING_ENABLE
#define LEDC_WARING(format,...) do{\
        ets_printf("[waring][%s#%u]",__FUNCTION__,__LINE__);\
        ets_printf(format,##__VA_ARGS__);\
}while(0)
#else
#define LEDC_WARING(...)
#endif
#if LEDC_DBG_ERROR_ENABLE
#define LEDC_ERROR(format,...) do{\
        ets_printf("[error][%s#%u]",__FUNCTION__,__LINE__);\
        ets_printf(format,##__VA_ARGS__);\
}while(0)
#else
#define LEDC_ERROR(...)
#endif

static portMUX_TYPE ledc_spinlock = portMUX_INITIALIZER_UNLOCKED;

static int ledc_is_valid_channel(uint32_t channel)
{
    if(channel > LEDC_CHANNEL_7) {
        LEDC_ERROR("LEDC CHANNEL ERR: %d\n",channel);
        return 0;
    }
    return 1;
}

static int ledc_is_valid_mode(uint32_t mode)
{
    if(mode >= LEDC_SPEED_MODE_MAX) {
        LEDC_ERROR("LEDC MODE ERR: %d\n",mode);
        return 0;
    }
    return 1;
}

static int ledc_is_valid_timer(int timer)
{
    if(timer > LEDC_TIMER3) {
        LEDC_ERROR("LEDC TIMER ERR: %d\n", timer);
        return 0;
    }
    return 1;
}

esp_err_t ledc_timer_config(ledc_mode_t speed_mode, ledc_timer_t timer_sel, uint32_t div_num, uint32_t bit_num, ledc_clk_src_t clk_src)
{
    if(!ledc_is_valid_mode(speed_mode)) {
        return ESP_ERR_INVALID_ARG;
    }
    if(!ledc_is_valid_timer(timer_sel)) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&ledc_spinlock);
    LEDC.timer_group[speed_mode].timer[timer_sel].conf.div_num = div_num;
    LEDC.timer_group[speed_mode].timer[timer_sel].conf.tick_sel = clk_src;
    LEDC.timer_group[speed_mode].timer[timer_sel].conf.bit_num = bit_num;
    if(speed_mode == LEDC_HIGH_SPEED_MODE) {
        LEDC.timer_group[speed_mode].timer[timer_sel].conf.low_speed_update = 1;
    }
    portEXIT_CRITICAL(&ledc_spinlock);
    return ESP_OK;
}

static esp_err_t ledc_duty_config(ledc_mode_t speed_mode, uint32_t channel_num, uint32_t hpoint_val, uint32_t duty_val,
    uint32_t duty_direction, uint32_t duty_num, uint32_t duty_cycle, uint32_t duty_scale)
{
    portENTER_CRITICAL(&ledc_spinlock);
    LEDC.channel_group[speed_mode].channel[channel_num].hpoint.hpoint = hpoint_val;
    LEDC.channel_group[speed_mode].channel[channel_num].duty.duty = duty_val;
    LEDC.channel_group[speed_mode].channel[channel_num].conf1.val = ((duty_direction & LEDC_DUTY_INC_HSCH0_V) << LEDC_DUTY_INC_HSCH0_S) |
                                                                    ((duty_num & LEDC_DUTY_NUM_HSCH0_V) << LEDC_DUTY_NUM_HSCH0_S) |
                                                                    ((duty_cycle & LEDC_DUTY_CYCLE_HSCH0_V) << LEDC_DUTY_CYCLE_HSCH0_S) |
                                                                    ((duty_scale & LEDC_DUTY_SCALE_HSCH0_V) << LEDC_DUTY_SCALE_HSCH0_S);
    portEXIT_CRITICAL(&ledc_spinlock);
    return ESP_OK;
}

esp_err_t ledc_bind_channel_timer(ledc_mode_t speed_mode, uint32_t channel, uint32_t timer_idx)
{
    if(!ledc_is_valid_mode(speed_mode)) {
        return ESP_ERR_INVALID_ARG;
    }
    if(!ledc_is_valid_timer(timer_idx)) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&ledc_spinlock);
    LEDC.channel_group[speed_mode].channel[channel].conf0.timer_sel = timer_idx;
    portEXIT_CRITICAL(&ledc_spinlock);
    return ESP_OK;
}

esp_err_t ledc_timer_rst(ledc_mode_t speed_mode, uint32_t timer_sel)
{
    if(!ledc_is_valid_mode(speed_mode)) {
        return ESP_ERR_INVALID_ARG;
    }
    if(!ledc_is_valid_timer(timer_sel)) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&ledc_spinlock);
    LEDC.timer_group[speed_mode].timer[timer_sel].conf.rst = 1;
    LEDC.timer_group[speed_mode].timer[timer_sel].conf.rst = 0;
    portEXIT_CRITICAL(&ledc_spinlock);
    return ESP_OK;
}

esp_err_t ledc_timer_pause(ledc_mode_t speed_mode, uint32_t timer_sel)
{
    if(!ledc_is_valid_mode(speed_mode)) {
        return ESP_ERR_INVALID_ARG;
    }
    if(!ledc_is_valid_timer(timer_sel)) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&ledc_spinlock);
    LEDC.timer_group[speed_mode].timer[timer_sel].conf.pause = 1;
    portEXIT_CRITICAL(&ledc_spinlock);
    return ESP_OK;
}

esp_err_t ledc_timer_resume(ledc_mode_t speed_mode, uint32_t timer_sel)
{
    if(!ledc_is_valid_mode(speed_mode)) {
        return ESP_ERR_INVALID_ARG;
    }
    if(!ledc_is_valid_timer(timer_sel)) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&ledc_spinlock);
    LEDC.timer_group[speed_mode].timer[timer_sel].conf.pause = 0;
    portEXIT_CRITICAL(&ledc_spinlock);
    return ESP_OK;
}

static esp_err_t ledc_enable_intr_type(ledc_mode_t speed_mode, uint32_t channel, ledc_intr_type_t type)
{
    if(!ledc_is_valid_mode(speed_mode)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t value;
    uint32_t intr_type = type;
    portENTER_CRITICAL(&ledc_spinlock);
    value = LEDC.int_ena.val;
    if(intr_type == LEDC_INTR_FADE_END) {
        LEDC.int_ena.val = value | BIT(LEDC_DUTY_CHNG_END_HSCH0_INT_ENA_S + channel);
    } else {
        LEDC.int_ena.val = (value & (~(BIT(LEDC_DUTY_CHNG_END_HSCH0_INT_ENA_S + channel))));
    }
    portEXIT_CRITICAL(&ledc_spinlock);
    return ESP_OK;
}

esp_err_t ledc_isr_register(uint32_t ledc_intr_num, void (*fn)(void*), void * arg)
{
    if(fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&ledc_spinlock);
    ESP_INTR_DISABLE(ledc_intr_num);
    intr_matrix_set(xPortGetCoreID(), ETS_LEDC_INTR_SOURCE, ledc_intr_num);
    xt_set_interrupt_handler(ledc_intr_num, fn, arg);
    ESP_INTR_ENABLE(ledc_intr_num);
    portEXIT_CRITICAL(&ledc_spinlock);
    return ESP_OK;
}

esp_err_t ledc_config(ledc_config_t* ledc_conf)
{
    SET_PERI_REG_MASK(DPORT_PERIP_CLK_EN_REG, DPORT_LEDC_CLK_EN);
    CLEAR_PERI_REG_MASK(DPORT_PERIP_RST_EN_REG, DPORT_LEDC_RST);

    uint32_t speed_mode = ledc_conf->speed_mode;
    uint32_t gpio_num = ledc_conf->gpio_num;
    uint32_t ledc_channel = ledc_conf->channel;
    uint32_t freq_hz = ledc_conf->freq_hz;
    uint32_t timer_select = ledc_conf->timer_sel;
    uint32_t bit_num = ledc_conf->bit_num;
    uint32_t intr_type = ledc_conf->intr_type;
    uint32_t duty = ledc_conf->duty;
    uint32_t div_param = 0;
    uint32_t precision = 0;
    int timer_clk_src = 0;

    if(!ledc_is_valid_channel(ledc_channel)) {
        return ESP_ERR_INVALID_ARG;
    }
    if(!ledc_is_valid_mode(speed_mode)) {
        return ESP_ERR_INVALID_ARG;
    }
    if(!GPIO_IS_VALID_OUTPUT_GPIO(gpio_num)) {
        LEDC_ERROR("GPIO number error: IO%d\n ", gpio_num);
        return ESP_ERR_INVALID_ARG;
    }
    if(freq_hz == 0 || bit_num == 0 || bit_num > LEDC_TIMER_15_BIT) {
        LEDC_ERROR("freq_hz=%u bit_num=%u\n", div_param, bit_num);
        return ESP_ERR_INVALID_ARG;
    }
    if(timer_select > LEDC_TIMER3) {
        LEDC_ERROR("Time Select %u\n", timer_select);
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&ledc_spinlock);
    esp_err_t ret = ESP_OK;
    precision = (0x1 << bit_num);  //2**depth
    div_param = ((uint64_t) LEDC_APB_CLK_HZ << 8) / freq_hz / precision; //8bit fragment
    /*Fail ,because the div_num overflow or too small*/
    if(div_param <= 256 || div_param > LEDC_DIV_NUM_HSTIMER0_V) { //REF TICK
        /*Selet the reference tick*/
        div_param = ((uint64_t) LEDC_REF_CLK_HZ << 8) / freq_hz / precision;
        if(div_param <= 256 || div_param > LEDC_DIV_NUM_HSTIMER0_V) {
            LEDC_ERROR("div param err,div_param=%u\n", div_param);
            ret = ESP_FAIL;
        }
        timer_clk_src = LEDC_REF_TICK;
    } else { //APB TICK
        timer_clk_src = LEDC_APB_CLK;
    }
    //1. set timer parameters
    //   timer settings decide the clk of counter and the period of PWM
    ledc_timer_config(speed_mode, timer_select, div_param, bit_num, timer_clk_src);
    //   reset timer.
    ledc_timer_rst(speed_mode, timer_select);
    //2. set channel parameters
    //   channel parameters decide how the waveform looks like in one period
    //   set channel duty, duty range is (0 ~ ((2 ** bit_num) - 1))
    ledc_set_duty(speed_mode, ledc_channel, duty);
    //update duty settings
    ledc_update(speed_mode, ledc_channel);
    //3. bind the channel with the timer
    ledc_bind_channel_timer(speed_mode, ledc_channel, timer_select);
    //4. set interrupt type
    ledc_enable_intr_type(speed_mode, ledc_channel, intr_type);
    LEDC_INFO("LEDC_PWM CHANNEL %1u|GPIO %02u|FreHz %05u|Duty %04u|Depth %04u|Time %01u|SourceClk %01u|Divparam %u\n",
        ledc_channel, gpio_num, freq_hz, duty, bit_num, timer_select, timer_clk_src, div_param
    );
    /*5. set LEDC signal in gpio matrix*/
    PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[gpio_num], PIN_FUNC_GPIO);
    gpio_set_direction(gpio_num, GPIO_MODE_OUTPUT);
    gpio_matrix_out(gpio_num, LEDC_HS_SIG_OUT0_IDX + ledc_channel, 0, 0);
    portEXIT_CRITICAL(&ledc_spinlock);
    return ret;
}

esp_err_t ledc_update(ledc_mode_t speed_mode, ledc_channel_t channel)
{
    if(!ledc_is_valid_mode(speed_mode)) {
        return ESP_ERR_INVALID_ARG;
    }
    if(!ledc_is_valid_channel(channel)) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&ledc_spinlock);
    LEDC.channel_group[speed_mode].channel[channel].conf0.sig_out_en = 1;
    LEDC.channel_group[speed_mode].channel[channel].conf1.duty_start = 1;
    portEXIT_CRITICAL(&ledc_spinlock);
    return ESP_OK;
}

esp_err_t ledc_stop(ledc_mode_t speed_mode, ledc_channel_t channel, uint32_t idle_level)
{
    if(!ledc_is_valid_mode(speed_mode)) {
        return ESP_ERR_INVALID_ARG;
    }
    if(!ledc_is_valid_channel(channel)) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&ledc_spinlock);
    LEDC.channel_group[speed_mode].channel[channel].conf0.idle_lv = idle_level & 0x1;
    LEDC.channel_group[speed_mode].channel[channel].conf0.sig_out_en = 0;
    LEDC.channel_group[speed_mode].channel[channel].conf1.duty_start = 0;
    portEXIT_CRITICAL(&ledc_spinlock);
    return ESP_OK;
}
esp_err_t ledc_set_fade(ledc_mode_t speed_mode, uint32_t channel, uint32_t duty, ledc_duty_direction_t fade_direction,
    uint32_t step_num, uint32_t duty_cyle_num, uint32_t duty_scale)
{
    if(!ledc_is_valid_mode(speed_mode)) {
        return ESP_ERR_INVALID_ARG;
    }
    if(!ledc_is_valid_channel(channel)) {
        return ESP_ERR_INVALID_ARG;
    }
    if(fade_direction > LEDC_DUTY_DIR_INCREASE) {
        LEDC_ERROR("Duty direction err\n");
        return ESP_ERR_INVALID_ARG;
    }
    if(step_num > LEDC_DUTY_NUM_HSCH0_V || duty_cyle_num > LEDC_DUTY_CYCLE_HSCH0_V || duty_scale > LEDC_DUTY_SCALE_HSCH0_V) {
        LEDC_ERROR("step_num=%u duty_cyle_num=%u duty_scale=%u\n", step_num, duty_cyle_num, duty_scale);
        return ESP_ERR_INVALID_ARG;
    }
    ledc_duty_config(speed_mode,
                     channel,        //uint32_t chan_num,
                     0,              //uint32_t hpoint_val,
                     duty << 4,      //uint32_t duty_val,the least 4 bits are decimal part
                     fade_direction, //uint32_t increase,
                     step_num,       //uint32_t duty_num,
                     duty_cyle_num,  //uint32_t duty_cycle,
                     duty_scale      //uint32_t duty_scale
                     );
    return ESP_OK;
}

esp_err_t ledc_set_duty(ledc_mode_t speed_mode, ledc_channel_t channel, uint32_t duty)
{
    if(!ledc_is_valid_mode(speed_mode)) {
        return ESP_ERR_INVALID_ARG;
    }
    if(!ledc_is_valid_channel(channel)) {
        return ESP_ERR_INVALID_ARG;
    }
    ledc_duty_config(speed_mode,
                     channel,         //uint32_t chan_num,
                     0,               //uint32_t hpoint_val,
                     duty << 4,       //uint32_t duty_val,the least 4 bits are decimal part
                     1,               //uint32_t increase,
                     1,               //uint32_t duty_num,
                     1,               //uint32_t duty_cycle,
                     0                //uint32_t duty_scale
                     );
    return ESP_OK;
}

int ledc_get_duty(ledc_mode_t speed_mode, ledc_channel_t channel)
{
    if(!ledc_is_valid_mode(speed_mode)) {
        return -1;
    }
    uint32_t duty = (LEDC.channel_group[speed_mode].channel[channel].duty_rd.duty_read >> 4);
    return duty;
}

esp_err_t ledc_set_freq(ledc_mode_t speed_mode, ledc_timer_t timer_num, uint32_t freq_hz)
{
    if(!ledc_is_valid_mode(speed_mode)) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&ledc_spinlock);
    esp_err_t ret = ESP_OK;
    uint32_t div_num = 0;
    uint32_t bit_num = LEDC.timer_group[speed_mode].timer[timer_num].conf.bit_num;
    uint32_t timer_source_clk = LEDC.timer_group[speed_mode].timer[timer_num].conf.tick_sel;
    uint32_t precision = (0x1 << bit_num);
    if(timer_source_clk == LEDC_APB_CLK) {
        div_num = ((uint64_t) LEDC_APB_CLK_HZ << 8) / freq_hz / precision;
    } else {
        div_num = ((uint64_t) LEDC_REF_CLK_HZ << 8) / freq_hz / precision;
    }
    if(div_num <= 256 || div_num > LEDC_DIV_NUM_HSTIMER0) {
        LEDC_ERROR("div param err,div_param=%u\n", div_num);
        ret = ESP_FAIL;
    }
    LEDC.timer_group[speed_mode].timer[timer_num].conf.div_num = div_num;
    portEXIT_CRITICAL(&ledc_spinlock);
    return ret;
}

uint32_t ledc_get_freq(ledc_mode_t speed_mode, ledc_timer_t timer_num)
{
    if(!ledc_is_valid_mode(speed_mode)) {
        return 0;
    }
    portENTER_CRITICAL(&ledc_spinlock);
    uint32_t freq = 0;
    uint32_t timer_source_clk = LEDC.timer_group[speed_mode].timer[timer_num].conf.tick_sel;
    uint32_t bit_num = LEDC.timer_group[speed_mode].timer[timer_num].conf.bit_num;
    uint32_t div_num = LEDC.timer_group[speed_mode].timer[timer_num].conf.div_num;
    uint32_t precision = (0x1 << bit_num);
    if(timer_source_clk == LEDC_APB_CLK) {
        freq = ((uint64_t) LEDC_APB_CLK_HZ << 8) / precision / div_num;
    } else {
        freq = ((uint64_t) LEDC_REF_CLK_HZ << 8) / precision / div_num;
    }
    portEXIT_CRITICAL(&ledc_spinlock);
    return freq;
}
