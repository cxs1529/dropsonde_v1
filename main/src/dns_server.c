/**
 * ==============================================================================
 * @file    dns_server.c
 * @brief   Captive DNS Server implementation on UDP Port 53.
 * 
 * @details
 * Runs a UDP socket listener. When a connected device queries ANY domain name 
 * (such as `aomldropsonde.com`), this task intercepts the query and returns 
 * an A-Record answer pointing directly to `192.168.4.1`, forcing the device 
 * to open our dashboard.
 * ==============================================================================
 */

#include "dns_server.h"
#include "config.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <string.h>

static const char *TAG = "DNS_SERVER";
static TaskHandle_t dns_task_handle = NULL;
static int dns_socket_fd = -1;

/**
 * @brief Standard 12-byte DNS Header Structure
 */
typedef struct __attribute__((__packed__)) {
    uint16_t id;         ///< Transaction ID
    uint8_t flags1;      ///< QR, Opcode, AA, TC, RD
    uint8_t flags2;      ///< RA, Z, RCODE
    uint16_t num_q;      ///< Number of questions
    uint16_t num_answer; ///< Number of answer RRs
    uint16_t num_auth;   ///< Number of authority RRs
    uint16_t num_num;    ///< Number of additional RRs
} dns_header_t;

/**
 * @brief Background task processing raw UDP DNS packets.
 */
static void dns_server_task(void *pvParameters) {
    uint8_t rx_buffer[128];
    struct sockaddr_in server_addr;

    // 1. Create IPv4 UDP Socket
    dns_socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (dns_socket_fd < 0) {
        ESP_LOGE(TAG, "Failed to create DNS UDP socket.");
        dns_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    // 2. Bind Socket to standard UDP Port 53
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(53); 
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(dns_socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind DNS socket to port 53.");
        close(dns_socket_fd);
        dns_socket_fd = -1;
        dns_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Captive DNS Server running on port 53...");

    // 3. Listening Loop
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        
        // Wait for incoming DNS query (blocks thread)
        int len = recvfrom(dns_socket_fd, rx_buffer, sizeof(rx_buffer), 0, 
                           (struct sockaddr *)&client_addr, &client_addr_len);
        
        // If socket is closed externally via stop_dns_server(), break the loop
        if (len < 0) break; 

        if (len >= sizeof(dns_header_t)) {
            dns_header_t *dns_hdr = (dns_header_t *)rx_buffer;

            // Prepare DNS Header Response Flags
            dns_hdr->flags1 = 0x81; // Response + Recursion Desired
            dns_hdr->flags2 = 0x80; // Recursion Available
            dns_hdr->num_answer = htons(1); // Return 1 Answer

            int reply_len = len;
            
            // Format A-Record payload (DNS Name pointer compression offset)
            rx_buffer[reply_len++] = 0xc0; 
            rx_buffer[reply_len++] = 0x0c;
            
            // Type A (Host Address)
            rx_buffer[reply_len++] = 0x00;
            rx_buffer[reply_len++] = 0x01;
            
            // Class IN (Internet)
            rx_buffer[reply_len++] = 0x00;
            rx_buffer[reply_len++] = 0x01;
            
            // Time to Live (TTL): 60 seconds
            rx_buffer[reply_len++] = 0x00;
            rx_buffer[reply_len++] = 0x00;
            rx_buffer[reply_len++] = 0x00;
            rx_buffer[reply_len++] = 0x3c;
            
            // Data Length: 4 bytes (IPv4 address size)
            rx_buffer[reply_len++] = 0x00;
            rx_buffer[reply_len++] = 0x04;
            
            // Target IP Address: 192.168.4.1 (ESP32 AP Default IP)
            rx_buffer[reply_len++] = 192;
            rx_buffer[reply_len++] = 168;
            rx_buffer[reply_len++] = 4;
            rx_buffer[reply_len++] = 1;

            // Transmit DNS response back to the connected phone/laptop
            sendto(dns_socket_fd, rx_buffer, reply_len, 0, (struct sockaddr *)&client_addr, client_addr_len);
        }
    }
    
    // Cleanup if loop exits
    if (dns_socket_fd >= 0) {
        close(dns_socket_fd);
        dns_socket_fd = -1;
    }
    dns_task_handle = NULL;
    vTaskDelete(NULL);
}

void start_dns_server(void) {
    if (dns_task_handle == NULL) {
        xTaskCreate(dns_server_task, "dns_task", 3072, NULL, 3, &dns_task_handle);
    }
}

void stop_dns_server(void) {
    if (dns_socket_fd >= 0) {
        // Closing the socket unblocks recvfrom() and terminates the task cleanly
        close(dns_socket_fd);
        dns_socket_fd = -1;
    }
}