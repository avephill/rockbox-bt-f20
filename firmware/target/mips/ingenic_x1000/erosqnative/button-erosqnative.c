/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2021 Aidan MacDonald, Dana Conrad
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include "button.h"
#include "kernel.h"
#include "backlight.h"
#include "powermgmt.h"
#include "panic.h"
#include "axp-pmu.h"
#include "gpio-x1000.h"
#include "irq-x1000.h"
#include "i2c-x1000.h"
#include "eros_qn_codec.h"
#include <string.h>
#include <stdbool.h>
#include "devicedata.h"
#ifdef HAVE_BT_PCM_SINK
#include "bt-pcm-sink.h"
#endif

#ifndef BOOTLOADER
# include "settings.h"
# include "lcd.h"
# include "font.h"
#endif

/* ===========================================
 * | OLD STATE | NEW STATE |  DIRECTION      |
 * |   0  0    |   0  0    |  0: NO CHANGE   |
 * |   0  0    |   0  1    | -1: CCW         |
 * |   0  0    |   1  0    |  1: CW          |
 * |   0  0    |   1  1    |  0: INVALID     |
 * |   0  1    |   0  0    |  1: CW          |
 * |   0  1    |   0  1    |  0: NO CHANGE   |
 * |   0  1    |   1  0    |  0: INVALID     |
 * |   0  1    |   1  1    | -1: CCW         |
 * |   1  0    |   0  0    | -1: CCW         |
 * |   1  0    |   0  1    |  0: INVALID     |
 * |   1  0    |   1  0    |  0: NO CHANGE   |
 * |   1  0    |   1  1    |  1: CW          |
 * |   1  1    |   0  0    |  0: INVALID     |
 * |   1  1    |   0  1    |  1: CW          |
 * |   1  1    |   1  0    | -1: CCW         |
 * |   1  1    |   1  1    |  0: NO CHANGE   |
 * ===========================================
 *
 * Quadrature explanation since it's not plainly obvious how this works:
 *
 * If either of the quadrature lines change, we can look up the combination
 * of previous state and new state in the table above (enc_state[] below)
 * and it tells us whether to add 1, subtract 1, or no change from the sum (enc_position).
 * This also gives us a nice debounce, since each state can only have 1 pin change
 * at a time. I didn't come up with this, but I've used it before and it works well.
 *
 * Old state is 2 higher bits, new state is 2 lower bits of enc_current_state. */

/* list of valid quadrature states and their directions */
signed char enc_state[] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
volatile unsigned char enc_current_state = 0;
volatile signed int enc_position = 0;

#ifndef BOOTLOADER
/* ---- wheel acceleration (HAVE_WHEEL_ACCELERATION) ----
 *
 * A decaying activity counter tracks how energetically the wheel is being
 * spun: each detent adds WHEEL_ACCEL_INC, each 10 ms button poll subtracts
 * WHEEL_ACCEL_DECAY. The list-step multiplier posted with each
 * BUTTON_SCROLL_* event is derived from the counter value *before* this
 * detent's charge (so an isolated click is always exactly 1 item),
 * shifted and capped per the user's "Wheel Acceleration" setting. The
 * multiplier rides in button-data bits 24..30, bit 31 clear —
 * button_apply_acceleration() returns it as-is (the e200v2 scheme; the
 * iPod velocity curve is only for data with bit 31 set).
 *
 * The constants are tuned for an iPod-classic-like feel:
 *  - charge/drain ratio puts the engage knife-edge around 25 detents/s —
 *    below that the counter drains as fast as it charges and every click
 *    is 1:1 fine control;
 *  - WHEEL_ACCEL_ENGAGE keeps the multiplier at exactly 1 until the
 *    counter clears the threshold, so a single quick flick stays 1:1 and
 *    only *sustained* fast spinning (~0.2-0.5 s of it) engages and then
 *    saturates — retuned 2026-07-22 after the first cut engaged on any
 *    casual flick;
 *  - the heavy drain means ~200 ms after you stop spinning the counter
 *    is empty — a careful single click right after a big flick lands on
 *    the very next item, like the real thing. */
#define WHEEL_ACCEL_INC     16  /* charge per detent */
#define WHEEL_ACCEL_DECAY    4  /* drain per 10 ms poll */
#define WHEEL_ACCEL_MAX     80
#define WHEEL_ACCEL_ENGAGE  32  /* counter level where accel kicks in */
static int  wheel_accel;
static int  wheel_accel_shift = 2; /* multiplier slope above the knee */
static int  wheel_delta_cap   = 8; /* ...clamped to this many items */
static long wheel_last_detent;     /* tick of the previous detent */
static int  wheel_last_btn;        /* direction of the previous detent */
/* A pause this long between detents means the wheel stopped: the counter
 * hard-resets so the very next click after any pause is exactly 1 item —
 * no lingering "deceleration" from the decay tail. */
#define WHEEL_ACCEL_IDLE_RESET (HZ/10)

/* Strength levels for the "Wheel Acceleration" setting:
 * 0=off, 1=weak (max x4), 2=moderate (max x8), 3=strong (max x16). */
void button_wheel_set_accel(int level)
{
    switch(level) {
    case 0:  wheel_accel_shift = 2; wheel_delta_cap = 1;  break;
    case 1:  wheel_accel_shift = 3; wheel_delta_cap = 4;  break;
    case 2:  wheel_accel_shift = 2; wheel_delta_cap = 8;  break;
    default: wheel_accel_shift = 1; wheel_delta_cap = 16; break;
    }
}

/* Compute this event's button data and charge the counter. `steps` is how
 * many detents accumulated since the last poll (>= 1) — a fast spin can
 * land several inside one 10 ms poll window; folding them all in keeps
 * that energy instead of losing it to the one-event-per-poll bottleneck. */
static unsigned wheel_accel_data(int btn, int steps)
{
    /* stopped for a beat, or reversed direction -> all acceleration is
     * forfeit immediately */
    if (TIME_AFTER(current_tick, wheel_last_detent + WHEEL_ACCEL_IDLE_RESET)
        || btn != wheel_last_btn)
        wheel_accel = 0;
    wheel_last_detent = current_tick;
    wheel_last_btn    = btn;
    int delta = 1;
    if (wheel_accel > WHEEL_ACCEL_ENGAGE)
        delta += (wheel_accel - WHEEL_ACCEL_ENGAGE) >> wheel_accel_shift;
    if (delta > wheel_delta_cap) delta = wheel_delta_cap;
    wheel_accel += WHEEL_ACCEL_INC * steps;
    if (wheel_accel > WHEEL_ACCEL_MAX) wheel_accel = WHEEL_ACCEL_MAX;
    return (unsigned)delta << 24;
}
#else
static unsigned wheel_accel_data(int btn, int steps)
{ (void)btn; (void)steps; return 1u << 24; }
#endif

/* Value of headphone detect register */
static uint8_t hp_detect_reg = 0x00;
static uint8_t hp_detect_reg_old = 0x00;
#ifndef BOOTLOADER
static uint8_t hp_detect_debounce1 = 0x00;
#endif
static uint8_t hp_detect_debounce2 = 0x00;
static uint8_t debounce_count = 0;

/* Interval to poll the register */
#define HPD_POLL_TIME (HZ/4)

#ifndef BOOTLOADER
static int hp_detect_tmo_cb(struct timeout* tmo)
{
    if (hp_detect_debounce1 == hp_detect_debounce2){
        if (debounce_count >= 2){
            debounce_count = 2;
        } else {
            debounce_count = debounce_count + 1;
        }
    } else {
        debounce_count = 0;
        hp_detect_debounce2 = hp_detect_debounce1;
    }

    i2c_descriptor* d = (i2c_descriptor*)tmo->data;
    i2c_async_queue(AXP_PMU_BUS, TIMEOUT_NOBLOCK, I2C_Q_ADD, 0, d);
    return HPD_POLL_TIME;
}

static void hp_detect_init(int version)
{
    if (version <= 3) {
        static struct timeout tmo;
        static const uint8_t gpio_reg = AXP192_REG_GPIOSTATE1;
        static i2c_descriptor desc = {
            .slave_addr = AXP_PMU_ADDR,
            .bus_cond = I2C_START | I2C_STOP,
            .tran_mode = I2C_READ,
            .buffer[0] = (void*)&gpio_reg,
            .count[0] = 1,
            .buffer[1] = &hp_detect_debounce1,
            .count[1] = 1,
            .callback = NULL,
            .arg = 0,
            .next = NULL,
        };

        /* Headphone and LO detects are wired to AXP192 GPIOs 0 and 1,
        * set them to inputs. */
        i2c_reg_write1(AXP_PMU_BUS, AXP_PMU_ADDR, AXP192_REG_GPIO0FUNCTION, 0x01); /* HP detect */
        i2c_reg_write1(AXP_PMU_BUS, AXP_PMU_ADDR, AXP192_REG_GPIO1FUNCTION, 0x01); /* LO detect */

        /* Get an initial reading before startup */
        int r = i2c_reg_read1(AXP_PMU_BUS, AXP_PMU_ADDR, gpio_reg);
        if(r >= 0)
        {
            hp_detect_reg = r;
            hp_detect_debounce1 = r;
            hp_detect_debounce2 = r;
            hp_detect_reg_old = hp_detect_reg;
        }

        /* Poll the register every second */
        timeout_register(&tmo, &hp_detect_tmo_cb, HPD_POLL_TIME, (intptr_t)&desc);
    } else {
        uint32_t b = REG_GPIO_PIN(GPIO_B);

        // initialize headphone detect variables
        // HP_detect PB14 --> bit 4
        // LO_detect PB22 --> bit 5
        hp_detect_reg = ( (b>>10)&(0x10) | (b>>17)&(0x20) );
        hp_detect_debounce1 = hp_detect_reg;
        hp_detect_debounce2 = hp_detect_reg;
        hp_detect_reg_old = hp_detect_reg;
    }
}
#endif

bool headphones_inserted(void)
{
    if (debounce_count > 1){
        hp_detect_reg = hp_detect_debounce2;
    }
    /* if the status has changed, set the output volume accordingly */
    if ((hp_detect_reg & 0x30) != (hp_detect_reg_old & 0x30))
    {
        hp_detect_reg_old = hp_detect_reg;
#if !defined(BOOTLOADER)
        eros_qn_set_outputs();
#endif
    }
#if !defined(BOOTLOADER) && defined(HAVE_BT_PCM_SINK)
    /* When BT is the active output, report headphones as present so the
     * playback engine doesn't auto-pause via SYS_PHONE_UNPLUGGED. The
     * existing button.c poll will detect this transition and post
     * SYS_PHONE_PLUGGED, which unpauses any prior unplug-pause. */
    if (bt_pcm_sink_is_active())
        return true;
#endif
#if !defined(BOOTLOADER)
    if (global_settings.hp_lo_select == 1) // force headphones
    {
        return true;
    }
    else if (global_settings.hp_lo_select == 2) // force lineout
    {
        return false;
    }
    else // automatic
#endif
    {
        return hp_detect_reg & 0x10 ? false : true;
    }
}

bool lineout_inserted(void)
{
    if (debounce_count > 1){
        hp_detect_reg = hp_detect_debounce2;
    }
    /* if the status has changed, set the output volume accordingly */
    if ((hp_detect_reg & 0x30) != (hp_detect_reg_old & 0x30))
    {
        hp_detect_reg_old = hp_detect_reg;
#if !defined(BOOTLOADER)
        eros_qn_set_outputs();
#endif
    }
#if !defined(BOOTLOADER)
    if (global_settings.hp_lo_select == 1) // force headphones
    {
        return false;
    }
    else if (global_settings.hp_lo_select == 2) // force lineout
    {
        return true;
    }
    else // automatic
#endif
    {
        return hp_detect_reg & 0x20 ? false : true;
    }
}

/* Rockbox interface */
void button_init_device(void)
{
    /* set both quadrature lines to interrupts */
    gpio_set_function(GPIO_BTN_SCROLL_A, GPIOF_IRQ_EDGE(1));
    gpio_set_function(GPIO_BTN_SCROLL_B, GPIOF_IRQ_EDGE(1));

    /* set interrupts to fire on the next edge based on current state */
    gpio_flip_edge_irq(GPIO_BTN_SCROLL_A);
    gpio_flip_edge_irq(GPIO_BTN_SCROLL_B);

    /* get current state of both encoder gpios */
    enc_current_state = (REG_GPIO_PIN(GPIO_B)>>21) & 0x0c;

    /* enable quadrature interrupts */
    gpio_enable_irq(GPIO_BTN_SCROLL_A);
    gpio_enable_irq(GPIO_BTN_SCROLL_B);

    /* Set up headphone and line out detect polling */
#ifndef BOOTLOADER
    hp_detect_init(device_data.hw_rev);
#endif
}

/* wheel Quadrature line A interrupt */
void GPIOB24(void)
{
    /* fill state with previous (2 higher bits) and current (2 lower bits) */
    enc_current_state = (enc_current_state & 0x0c) | ((REG_GPIO_PIN(GPIO_B)>>23) & 0x03);

    /* look up in table */
    enc_position = enc_position + enc_state[(enc_current_state)];

    /* move current state to previous state if valid data */
    if (enc_state[(enc_current_state)] != 0)
        enc_current_state = (enc_current_state << 2);

    /* we want the other edge next time */
    gpio_flip_edge_irq(GPIO_BTN_SCROLL_A);
}

/* wheel Quadrature line B interrupt */
void GPIOB23(void)
{
    /* fill state with previous (2 higher bits) and current (2 lower bits) */
    enc_current_state = (enc_current_state & 0x0c) | ((REG_GPIO_PIN(GPIO_B)>>23) & 0x03);

    /* look up in table */
    enc_position = enc_position + enc_state[(enc_current_state)];

    /* move current state to previous state if valid data */
    if (enc_state[(enc_current_state)] != 0)
        enc_current_state = (enc_current_state << 2);

    /* we want the other edge next time */
    gpio_flip_edge_irq(GPIO_BTN_SCROLL_B);
}

int button_read_device(void)
{
    int r = 0;

    /* Read GPIOs for normal buttons */
    uint32_t a = REG_GPIO_PIN(GPIO_A);
    uint32_t b = REG_GPIO_PIN(GPIO_B);
    uint32_t c = REG_GPIO_PIN(GPIO_C);
    uint32_t d = REG_GPIO_PIN(GPIO_D);

    /* All buttons are active low */
    if((a & (1 << 16)) == 0) r  |= BUTTON_PLAY;
    if((a & (1 << 17)) == 0) r  |= BUTTON_VOL_UP;
    if((a & (1 << 19)) == 0) r  |= BUTTON_VOL_DOWN;
#ifdef BOOTLOADER
# if EROSQN_VER >= 4
    if((b & (1 << 31)) == 0) r |= BUTTON_POWER;
    if((a & (1 << 18)) == 0) r |= BUTTON_BACK;
# else
    if((b & (1 << 7)) == 0) r  |= BUTTON_POWER;
    if((d & (1 << 5)) == 0) r  |= BUTTON_BACK;
# endif
#else
    if (device_data.hw_rev >= 4){
        if((b & (1 << 31)) == 0) r |= BUTTON_POWER;
        if((a & (1 << 18)) == 0) r |= BUTTON_BACK;
    } else {
        if((b & (1 << 7)) == 0) r  |= BUTTON_POWER;
        if((d & (1 << 5)) == 0) r  |= BUTTON_BACK;
    }
#endif
    if((b & (1 << 28)) == 0) r  |= BUTTON_MENU;
    
    if((d & (1 <<  4)) == 0) r  |= BUTTON_PREV;
    if((c & (1 << 24)) == 0) r  |= BUTTON_NEXT;

#ifndef BOOTLOADER
    if (device_data.hw_rev >= 4){
        // get new HP/LO detect states
        // HP_detect PB14 --> hp_detect bit 4
        // LO_detect PB22 --> hp_detect bit 5
        hp_detect_debounce1 = ( (b>>10)&(0x10) | (b>>17)&(0x20) );

        // enter them into the debounce process
        if (hp_detect_debounce1 == hp_detect_debounce2){
        if (debounce_count >= 2){
            debounce_count = 2;
        } else {
            debounce_count = debounce_count + 1;
        }
        } else {
            debounce_count = 0;
            hp_detect_debounce2 = hp_detect_debounce1;
        }
    }
#endif

#ifndef BOOTLOADER
    /* wheel-acceleration drain, once per 10 ms poll */
    wheel_accel -= WHEEL_ACCEL_DECAY;
    if (wheel_accel < 0)
        wheel_accel = 0;
#endif
    /* check encoder - from testing, each indent is 2 state changes or so */
    if (enc_position > 1)
    {
        /* need to use queue_post() in order to do BUTTON_SCROLL_*,
         * Rockbox treats these buttons differently. */
        button_queue_post(BUTTON_SCROLL_FWD,
                          wheel_accel_data(BUTTON_SCROLL_FWD, enc_position / 2));
        enc_position = 0;
        reset_poweroff_timer();
        backlight_on();
    }
    else if (enc_position < -1)
    {
        /* need to use queue_post() in order to do BUTTON_SCROLL_*,
         * Rockbox treats these buttons differently. */
        button_queue_post(BUTTON_SCROLL_BACK,
                          wheel_accel_data(BUTTON_SCROLL_BACK, -enc_position / 2));
        enc_position = 0;
        reset_poweroff_timer();
        backlight_on();
    }

    return r;
}

