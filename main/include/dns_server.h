/**
 * ==============================================================================
 * @file    dns_server.h
 * @brief   Header interface for the Captive DNS Server.
 * 
 * @details
 * Declares the lifecycle controls for the UDP DNS server on Port 53.
 * 
 * FUNCTIONS SUMMARY:
 * - start_dns_server(): Spawns the FreeRTOS task running the UDP socket.
 * - stop_dns_server(): Gracefully stops the task and releases the socket.
 * ==============================================================================
 */

#ifndef DNS_SERVER_H
#define DNS_SERVER_H

/**
 * @brief Spawns the UDP DNS Server task on Port 53.
 */
void start_dns_server(void);

/**
 * @brief Closes the UDP socket and deletes the DNS task safely.
 */
void stop_dns_server(void);

#endif // DNS_SERVER_H