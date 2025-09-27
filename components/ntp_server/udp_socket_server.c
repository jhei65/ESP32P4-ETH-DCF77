#include <string.h>
#include <sys/param.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <sys/time.h>
#include <time.h>

#include "lwip/netdb.h"
#include "lwip/sockets.h"

static const char *TAG = "udp_server";
// NTP port and packet buffer
#define NTP_PORT 123
#define NTP_PACKET_SIZE 48
static bool overflow;
long offset = 0;
const long oneSecond_inMicroseconds_L = 1000000;  // one second in microseconds

struct tm getTimeStruct() {
    struct tm timeinfo;
    time_t now;
    time(&now);
    localtime_r(&now, &timeinfo);
    time_t tt = mktime(&timeinfo);

    if (overflow) {
        tt += 63071999;
    }
    if (offset > 0) {
        tt += (unsigned long)offset;
    } else {
        tt -= (unsigned long)(offset * -1);
    }
    struct tm *tn = localtime(&tt);
    if (overflow) {
        tn->tm_year += 64;
    }
    return *tn;
}

unsigned long getEpoch() {
    struct tm timeinfo = getTimeStruct();
    return mktime(&timeinfo);
}
unsigned long getMicros() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_usec;
}

uint64_t getCurrentTimeInNTP64BitFormat() {
    const uint64_t numberOfSecondsBetween1900and1970 = 2208988800;

    uint64_t clockSecondsSinceEpoch = numberOfSecondsBetween1900and1970 + getEpoch();
    long clockMicroSeconds = (long)getMicros();

    while (clockMicroSeconds > oneSecond_inMicroseconds_L) {
        clockSecondsSinceEpoch++;
        clockMicroSeconds -= oneSecond_inMicroseconds_L;
    };

    while (clockMicroSeconds < 0L) {
        clockSecondsSinceEpoch--;
        clockMicroSeconds += oneSecond_inMicroseconds_L;
    };

    // for the next two lines to be clear, please see: https://tickelton.gitlab.io/articles/ntp-timestamps/

    double clockMicroSeconds_D = (double)clockMicroSeconds * (double)(4294.967296);
    return ((uint64_t)clockSecondsSinceEpoch << 32) | (uint64_t)(clockMicroSeconds_D);
}

void udp_server_task(void *pvParameters) {
    char ntp_packet[NTP_PACKET_SIZE];
    int addr_family = AF_INET;
    int ip_protocol = IPPROTO_IP;

    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(NTP_PORT);

    // print time

    int sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        return;
    }
    ESP_LOGI(TAG, "Socket created");

    int err = bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err < 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        close(sock);
        return;
    }
    ESP_LOGI(TAG, "Socket bound, port %d", NTP_PORT);

    while (1) {
        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);
        int len = recvfrom(sock, ntp_packet, sizeof(ntp_packet) - 1, 0, (struct sockaddr *)&source_addr, &socklen);
        ESP_LOGI(TAG, "received udp request");
        uint64_t receiveTime_uint64_t = getCurrentTimeInNTP64BitFormat();

        if (len < 0 || len > NTP_PACKET_SIZE) {
            ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
            break;
        } else {
            ntp_packet[len] = 0;  // Null-terminieren
            ntp_packet[0] = 0b00011100;
            // Stratum, or type of clock
            ntp_packet[1] = 0b00000001;
            // Polling Interval
            ntp_packet[2] = 4;
            ntp_packet[3] = 0xF7;
            ntp_packet[4] = 0;
            ntp_packet[5] = 0;
            ntp_packet[6] = 0;
            ntp_packet[7] = 0;
            ntp_packet[8] = 0;
            ntp_packet[9] = 0;
            ntp_packet[10] = 0;
            ntp_packet[11] = 0x50;

            // time source (namestring)
            ntp_packet[12] = 69;  // D
            ntp_packet[13] = 67;  // C
            ntp_packet[14] = 70;  // F
            ntp_packet[15] = 0;

            uint64_t referenceTime_uint64_t = getCurrentTimeInNTP64BitFormat();

            ntp_packet[16] = (int)((referenceTime_uint64_t >> 56) & 0xFF);
            ntp_packet[17] = (int)((referenceTime_uint64_t >> 48) & 0xFF);
            ntp_packet[18] = (int)((referenceTime_uint64_t >> 40) & 0xFF);
            ntp_packet[19] = (int)((referenceTime_uint64_t >> 32) & 0xFF);
            ntp_packet[20] = (int)((referenceTime_uint64_t >> 24) & 0xFF);
            ntp_packet[21] = (int)((referenceTime_uint64_t >> 16) & 0xFF);
            ntp_packet[22] = (int)((referenceTime_uint64_t >> 8) & 0xFF);
            ntp_packet[23] = (int)(referenceTime_uint64_t & 0xFF);

            // copy transmit time from the NTP original request to bytes 24 to 31 of the response packet
            ntp_packet[24] = ntp_packet[40];
            ntp_packet[25] = ntp_packet[41];
            ntp_packet[26] = ntp_packet[42];
            ntp_packet[27] = ntp_packet[43];
            ntp_packet[28] = ntp_packet[44];
            ntp_packet[29] = ntp_packet[45];
            ntp_packet[30] = ntp_packet[46];
            ntp_packet[31] = ntp_packet[47];

            // write out the receive time (it was set above) to bytes 32 to 39 of the response packet
            ntp_packet[32] = (int)((receiveTime_uint64_t >> 56) & 0xFF);
            ntp_packet[33] = (int)((receiveTime_uint64_t >> 48) & 0xFF);
            ntp_packet[34] = (int)((receiveTime_uint64_t >> 40) & 0xFF);
            ntp_packet[35] = (int)((receiveTime_uint64_t >> 32) & 0xFF);
            ntp_packet[36] = (int)((receiveTime_uint64_t >> 24) & 0xFF);
            ntp_packet[37] = (int)((receiveTime_uint64_t >> 16) & 0xFF);
            ntp_packet[38] = (int)((receiveTime_uint64_t >> 8) & 0xFF);
            ntp_packet[39] = (int)(receiveTime_uint64_t & 0xFF);

            // get the current time and write it out as the transmit time to bytes 40 to 47 of the response packet
            uint64_t transmitTime_uint64_t = getCurrentTimeInNTP64BitFormat();

            ntp_packet[40] = (int)((transmitTime_uint64_t >> 56) & 0xFF);
            ntp_packet[41] = (int)((transmitTime_uint64_t >> 48) & 0xFF);
            ntp_packet[42] = (int)((transmitTime_uint64_t >> 40) & 0xFF);
            ntp_packet[43] = (int)((transmitTime_uint64_t >> 32) & 0xFF);
            ntp_packet[44] = (int)((transmitTime_uint64_t >> 24) & 0xFF);
            ntp_packet[45] = (int)((transmitTime_uint64_t >> 16) & 0xFF);
            ntp_packet[46] = (int)((transmitTime_uint64_t >> 8) & 0xFF);
            ntp_packet[47] = (int)(transmitTime_uint64_t & 0xFF);

            sendto(sock, ntp_packet, NTP_PACKET_SIZE, 0, (struct sockaddr *)&source_addr, sizeof(source_addr));
        }
    }

    if (sock != -1) {
        close(sock);
    }
    vTaskDelete(NULL);
}
