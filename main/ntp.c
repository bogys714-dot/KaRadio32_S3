//////////////////////////////////////////////////
// Simple NTP client for ESP8266, RTOS SDK.
// Copyright 2016 jp cocatrix (KaraWin)
// jp@karawin.fr
// See license.txt for license terms.
//////////////////////////////////////////////////
// esp32
#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#define TAG "NTP"
#define TIMEZONE_OFFSET 2  // Часовой пояс UTC+2 (Украина)

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include "lwip/sockets.h"
#include "lwip/api.h"
#include "lwip/netdb.h"
#include "ntp.h"
#include "interface.h"

// get ntp time and return an allocated tm struct (UTC+2)
bool ntp_get_time(struct tm **dt) {
    struct timeval timeout; 
    timeout.tv_usec = 0;
    timeout.tv_sec = 5; 
    int sockfd = 0;
    ntp_t* ntp;
    char *msg;
    int rv;
    char service[] = {"123"};
    char node[] = {"pool.ntp.org"};
    struct addrinfo hints, *servinfo = NULL, *p = NULL;
    time_t timestamp;
    
    msg = kcalloc(sizeof(ntp_t),1);
    if (msg == NULL){
        ESP_LOGE(TAG,"##SYS.DATE#: ntp fails on kcalloc");
        return false;
    } 
    
    ntp = (ntp_t*)msg;
    ntp->options = 0x1B;
    ntp->stratum = 0;
    ntp->poll = 6;
    ntp->precision = 1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    if ((rv = getaddrinfo(node, service, &hints, &servinfo)) != 0) {
        free(msg);
        return false;
    }     
    
    for (p = servinfo; p != NULL; p = p->ai_next) {
        if ((sockfd = socket(p->ai_family, p->ai_socktype,p->ai_protocol)) == -1) {
            ESP_LOGE(TAG,"##SYS.DATE#: ntp fails on %s %d","sockfd",sockfd);
            continue;
        }
        break;
    }
    
    if (setsockopt (sockfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout)) < 0){
        ESP_LOGE(TAG,"##SYS.DATE#: ntp fails on %s %d","setsockopt",0);
        free(msg);
        freeaddrinfo(servinfo);
        close(sockfd);
        return false;
    }   
    
    if ((rv = sendto(sockfd, msg, sizeof(ntp_t), 0,p->ai_addr, p->ai_addrlen)) == -1) {
        ESP_LOGE(TAG,"##SYS.DATE#: ntp fails on %s %d","sendto",rv);
        free(msg);
        freeaddrinfo(servinfo);
        close(sockfd);
        return false;                 
    }
    freeaddrinfo(servinfo); 
    
    if ((rv = recvfrom(sockfd, msg, sizeof(ntp_t) , 0,NULL, NULL)) <=0) {
        ESP_LOGE(TAG,"##SYS.DATE#: ntp fails on %s %d","recvfrom",rv);
        free(msg);
        close(sockfd);
        return false;   
    }   
            
    ntp = (ntp_t*)msg;    
    timestamp = ntp->trans_time[0] << 24 | ntp->trans_time[1] << 16 |ntp->trans_time[2] << 8 | ntp->trans_time[3];
    
    // convert to unix time
    timestamp -= 2208988800UL;
    
    // apply timezone offset (UTC+2)
    timestamp += TIMEZONE_OFFSET * 3600;
    
    // create tm struct
    *dt = gmtime(&timestamp);
    free(msg);
    close(sockfd);
    return true;
}

// print date time in ISO-8601 local time format
void ntp_print_time() {
    struct tm* dt;
    char msg[30];
    
    if (ntp_get_time(&dt))
    {
        strftime(msg, 48, "%Y-%m-%dT%H:%M:%S", dt);
        kprintf("##SYS.DATE#: %s+%02d:00\n", msg, TIMEZONE_OFFSET);
    }
}