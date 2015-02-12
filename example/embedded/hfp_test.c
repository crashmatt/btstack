/*
 * Copyright (C) 2014 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL MATTHIAS
 * RINGWALD OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at 
 * contact@bluekitchen-gmbh.com
 *
 */
 
// *****************************************************************************
//
// minimal setup for SDP client over USB or UART
//
// *****************************************************************************

#include "btstack-config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#include <btstack/hci_cmds.h>
#include <btstack/run_loop.h>

#include "hci.h"
#include "btstack_memory.h"
#include "hci_dump.h"
#include "l2cap.h"
#include "sdp_query_rfcomm.h"

// static bd_addr_t remote = {0x04,0x0C,0xCE,0xE4,0x85,0xD3};
static bd_addr_t remote = {0x00, 0x21, 0x3C, 0xAC, 0xF7, 0x38};
static uint8_t channel_nr = 2;

static uint8_t  service_index = 0;
static uint8_t  channels[10];
static char*    service_name[10];

static uint16_t mtu;
static uint16_t rfcomm_cid = 0;

static char hfp_data[50];
static int hfp_send_err = 0; 

static uint8_t hfp_service_level_connection_state = 0;

static void send_packet(){
    hfp_send_err = 0; 
    switch (hfp_service_level_connection_state){
        case 1:
            strcpy(hfp_data, "\r\n+BRSF: 224\r\n\r\nOK\r\n");
            hfp_send_err = rfcomm_send_internal(rfcomm_cid, (uint8_t*) hfp_data, strlen(hfp_data));
            hfp_service_level_connection_state++;
            break;
        default:
            break;
    }
    
    if (hfp_send_err){
        printf("rfcomm_send_internal -> error 0X%02x", hfp_send_err);
        return;
    }
}

void store_found_service(uint8_t * name, uint8_t port){
    printf("APP: Service name: '%s', RFCOMM port %u\n", name, port);
    channels[service_index] = port;
    service_name[service_index] = (char*) malloc(SDP_SERVICE_NAME_LEN+1);
    strncpy(service_name[service_index], (char*) name, SDP_SERVICE_NAME_LEN);
    service_name[service_index][SDP_SERVICE_NAME_LEN] = 0;
    service_index++;
}

void report_found_services(){
    printf("\n *** Client query response done. ");
    if (service_index == 0){
        printf("No service found.\n\n");
    } else {
        printf("Found following %d services:\n", service_index);
    }
    int i;
    for (i=0; i<service_index; i++){
        printf("     Service name %s, RFCOMM port %u\n", service_name[i], channels[i]);
    }    
    printf(" ***\n\n");
}

static void packet_handler (void * connection, uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    // printf("packet_handler type %u, packet[0] %x\n", packet_type, packet[0]);
    if (packet_type == RFCOMM_DATA_PACKET){
        hfp_service_level_connection_state++;
        if (rfcomm_can_send_packet_now(rfcomm_cid)) send_packet();
    }
    if (packet_type != HCI_EVENT_PACKET) return;
    uint8_t event = packet[0];
    bd_addr_t event_addr;

    switch (event) {
        case BTSTACK_EVENT_STATE:
            // bt stack activated, get started 
            if (packet[2] == HCI_STATE_WORKING){
                sdp_query_rfcomm_channel_and_name_for_uuid(remote, 0x111E);
            }
            break;

        case HCI_EVENT_PIN_CODE_REQUEST:
            // inform about pin code request
            printf("Pin code request - using '0000'\n\r");
            bt_flip_addr(event_addr, &packet[2]);
            hci_send_cmd(&hci_pin_code_request_reply, &event_addr, 4, "0000");
            break;

        case RFCOMM_EVENT_OPEN_CHANNEL_COMPLETE:
            printf("RFCOMM_EVENT_OPEN_CHANNEL_COMPLETE packet_handler type %u, packet[0] %x\n", packet_type, packet[0]);
            // data: event(8), len(8), status (8), address (48), handle(16), server channel(8), rfcomm_cid(16), max frame size(16)
            if (packet[2]) {
                printf("RFCOMM channel open failed, status %u\n", packet[2]);
            } else {
                // data: event(8), len(8), status (8), address (48), handle (16), server channel(8), rfcomm_cid(16), max frame size(16)
                rfcomm_cid = READ_BT_16(packet, 12);
                mtu = READ_BT_16(packet, 14);
                printf("RFCOMM channel open succeeded. New RFCOMM Channel ID %u, max frame size %u\n", rfcomm_cid, mtu);
                break;
            }
            break;
        case DAEMON_EVENT_HCI_PACKET_SENT:
        case RFCOMM_EVENT_CREDITS:
            if (rfcomm_can_send_packet_now(rfcomm_cid)) send_packet();
            break;
        default:
            break;
    }
}

void handle_query_rfcomm_event(sdp_query_event_t * event, void * context){
    sdp_query_rfcomm_service_event_t * ve;
    sdp_query_complete_event_t * ce;
            
    switch (event->type){
        case SDP_QUERY_RFCOMM_SERVICE:
            ve = (sdp_query_rfcomm_service_event_t*) event;
            store_found_service(ve->service_name, ve->channel_nr);
            break;
        case SDP_QUERY_COMPLETE:
            ce = (sdp_query_complete_event_t*) event;
            report_found_services();
            // hfp_initiate_service_level_connection();
            printf("connect\n");
            rfcomm_create_channel_internal(NULL, &remote, channel_nr); 
            
            break;
    }
}

int btstack_main(int argc, const char * argv[]);
int btstack_main(int argc, const char * argv[]){

    printf("Client HCI init done\r\n");
        
    // init L2CAP
    l2cap_init();
    l2cap_register_packet_handler(packet_handler);
    rfcomm_init();
    rfcomm_register_packet_handler(packet_handler);

    sdp_query_rfcomm_register_callback(handle_query_rfcomm_event, NULL);

    // turn on!
    hci_power_control(HCI_POWER_ON);

    // go!
    run_loop_execute(); 
    return 0;
}
