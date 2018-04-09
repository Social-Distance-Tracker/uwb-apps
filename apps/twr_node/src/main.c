/**
 * Copyright (C) 2017-2018, Decawave Limited, All Rights Reserved
 * 
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "sysinit/sysinit.h"
#include "os/os.h"
#include "bsp/bsp.h"
#include "hal/hal_gpio.h"
#include "hal/hal_bsp.h"
#ifdef ARCH_sim
#include "mcu/mcu_sim.h"
#endif

#include <dw1000/dw1000_dev.h>
#include <dw1000/dw1000_hal.h>
#include <dw1000/dw1000_phy.h>
#include <dw1000/dw1000_mac.h>
#include <dw1000/dw1000_rng.h>
#include <dw1000/dw1000_lwip.h>
#include <dw1000/dw1000_ftypes.h>

static dwt_config_t mac_config = {
    .chan = 5,                          // Channel number. 
    .prf = DWT_PRF_64M,                 // Pulse repetition frequency. 
    .txPreambLength = DWT_PLEN_256,     // Preamble length. Used in TX only. 
    .rxPAC = DWT_PAC8,                 // Preamble acquisition chunk size. Used in RX only. 
    .txCode = 8,                        // TX preamble code. Used in TX only. 
    .rxCode = 9,                        // RX preamble code. Used in RX only. 
    .nsSFD = 0,                         // 0 to use standard SFD, 1 to use non-standard SFD. 
    .dataRate = DWT_BR_6M8,             // Data rate. 
    .phrMode = DWT_PHRMODE_STD,         // PHY header mode. 
    .sfdTO = (256 + 1 + 8 - 8)         // SFD timeout (preamble length + 1 + SFD length - PAC size). Used in RX only. 
};

static dw1000_phy_txrf_config_t txrf_config = { 
        .PGdly = TC_PGDELAY_CH5,
        //.power = 0x25456585
        .BOOSTNORM = dw1000_power_value(DW1000_txrf_config_9db, 5),
        .BOOSTP500 = dw1000_power_value(DW1000_txrf_config_9db, 5),
        .BOOSTP250 = dw1000_power_value(DW1000_txrf_config_9db, 5),
        .BOOSTP125 = dw1000_power_value(DW1000_txrf_config_9db, 5)
};

static dw1000_rng_config_t rng_config = {
    .tx_holdoff_delay = 0x0800,         // Send Time delay in usec.
    .rx_timeout_period = 0x4000         // Receive response timeout in usec.
};

static twr_frame_t twr[] = {
    [0] = {
        .fctrl = 0x8841,                // frame control (0x8841 to indicate a data frame using 16-bit addressing).
        .PANID = 0xDECA,                 // PAN ID (0xDECA)
        .code = DWT_TWR_INVALID
    },
    [1] = {
        .fctrl = 0x8841,                // frame control (0x8841 to indicate a data frame using 16-bit addressing).
        .PANID = 0xDECA,                 // PAN ID (0xDECA)
        .code = DWT_TWR_INVALID,
    }
};

void print_frame(const char * name, twr_frame_t twr){
    printf("%s{\n\tfctrl:0x%04X,\n", name, twr.fctrl);
    printf("\tseq_num:0x%02X,\n", twr.seq_num);
    printf("\tPANID:0x%04X,\n", twr.PANID);
    printf("\tdst_address:0x%04X,\n", twr.dst_address);
    printf("\tsrc_address:0x%04X,\n", twr.src_address);
    printf("\tcode:0x%04X,\n", twr.code);
    printf("\treception_timestamp:0x%08lX,\n", twr.reception_timestamp); 
    printf("\ttransmission_timestamp:0x%08lX,\n", twr.transmission_timestamp); 
    printf("\trequest_timestamp:0x%08lX,\n", twr.request_timestamp); 
    printf("\tresponse_timestamp:0x%08lX\n}\n", twr.response_timestamp);
}


/* The timer callout */
static struct os_callout blinky_callout;

/*
 * Event callback function for timer events. It toggles the led pin.
*/

static void timer_ev_cb(struct os_event *ev) {
    assert(ev != NULL);

    hal_gpio_toggle(LED_BLINK_PIN);
    os_callout_reset(&blinky_callout, OS_TICKS_PER_SEC/10);
    
    dw1000_dev_instance_t * inst = hal_dw1000_inst(0);

    dw1000_rng_request(inst, 0x4321, DWT_DS_TWR);
   
    if (inst->status.start_rx_error)
        printf("{\"utime\": %lu,\"timer_ev_cb\": \"start_rx_error\"}\n",os_cputime_ticks_to_usecs(os_cputime_get32()));
    if (inst->status.start_tx_error)
        printf("{\"utime\": %lu,\"timer_ev_cb\":\"start_tx_error\"}\n",os_cputime_ticks_to_usecs(os_cputime_get32()));
    if (inst->status.rx_error)
        printf("{\"utime\": %lu,\"timer_ev_cb\":\"rx_error\"}\n",os_cputime_ticks_to_usecs(os_cputime_get32()));
    if (inst->status.request_timeout)
        printf("{\"utime\": %lu,\"timer_ev_cb\":\"request_timeout\"}\n",os_cputime_ticks_to_usecs(os_cputime_get32()));
    if (inst->status.rx_timeout_error)
        printf("{\"utime\": %lu,\"timer_ev_cb\":\"rx_timeout_error\"}\n",os_cputime_ticks_to_usecs(os_cputime_get32()));
   
    if (twr[0].code == DWT_SS_TWR_FINAL ){   
            uint32_t time_of_flight = (uint32_t) dw1000_rng_twr_to_tof(twr, DWT_SS_TWR);
            float range = dw1000_rng_tof_to_meters(dw1000_rng_twr_to_tof(twr, DWT_SS_TWR));
            print_frame("trw=", twr[0]);
            printf("{\"utime\": %lu,\"tof\": %lu,\"range\": %lu,\"res_req\": %lX, \"rec_tra\": %lX}\n", 
                    os_cputime_ticks_to_usecs(os_cputime_get32()), 
                    time_of_flight, 
                    (uint32_t)(range * 1000), 
                    (twr[0].response_timestamp - twr[0].request_timestamp), 
                    (twr[0].transmission_timestamp - twr[0].reception_timestamp)
            );
    }     
    if (twr[1].code == DWT_DS_TWR_FINAL || twr[1].code == DWT_DS_TWR_EXT_FINAL){
            uint32_t time_of_flight = (uint32_t) dw1000_rng_twr_to_tof(twr, DWT_DS_TWR);
            float range = dw1000_rng_tof_to_meters(dw1000_rng_twr_to_tof(twr, DWT_DS_TWR));
            print_frame("1st=", twr[0]); 
            print_frame("2nd=", twr[1]); 
            printf("{\"utime\": %lu,\"tof\": %lu,\"range\": %lu,\"res_req\": %lX, \"rec_tra\": %lX}\n", 
                    os_cputime_ticks_to_usecs(os_cputime_get32()), 
                    time_of_flight, 
                    (uint32_t)(range * 1000), 
                    (twr[1].response_timestamp - twr[1].request_timestamp), 
                    (twr[1].transmission_timestamp - twr[1].reception_timestamp)
            );
            twr[1].code = DWT_DS_TWR_END;
    }
    
}

/*
* Initialize the callout for a timer event.
*/
static void init_timer(void) {
    os_callout_init(&blinky_callout, os_eventq_dflt_get(), timer_ev_cb, NULL);
    os_callout_reset(&blinky_callout, OS_TICKS_PER_SEC);
}

int main(int argc, char **argv){
    int rc;
    
    sysinit();
    hal_gpio_init_out(LED_BLINK_PIN, 1);
    hal_gpio_init_out(LED_1, 1);
    hal_gpio_init_out(LED_3, 1);
    
    dw1000_dev_instance_t * inst = hal_dw1000_inst(0);
    dw1000_softreset(inst);
    dw1000_phy_init(inst, &txrf_config);   

    inst->PANID = 0xDECA;
    inst->my_short_address = MYNEWT_VAL(DEVICE_ID);

    dw1000_set_panid(inst,inst->PANID);
    dw1000_mac_init(inst, &mac_config);
    dw1000_rng_init(inst, &rng_config, sizeof(twr)/sizeof(twr_frame_t));
    dw1000_rng_set_frames(inst, twr, sizeof(twr)/sizeof(twr_frame_t));

    printf("device_id = 0x%lX\n",inst->device_id);
    printf("PANID = 0x%X\n",inst->PANID);
    printf("DeviceID = 0x%X\n",inst->my_short_address);
    printf("partID = 0x%lX\n",inst->partID);
    printf("lotID = 0x%lX\n",inst->lotID);
    printf("xtal_trim = 0x%X\n",inst->xtal_trim);
  
    init_timer();

    while (1) {
        os_eventq_run(os_eventq_dflt_get());
    }
    assert(0);
    return rc;
}

