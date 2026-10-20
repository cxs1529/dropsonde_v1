/**
 * ==============================================================================
 * @file    web_server.h
 * @brief   Header interface for Wi-Fi Access Point and HTTP Web Server.
 * 
 * FUNCTIONS SUMMARY:
 * - init_wifi_base(): Initializes TCP/IP stack and Wi-Fi AP settings.
 * - wifi_manager_task(): FreeRTOS task handling 10-second debounced RF power toggling.
 * ==============================================================================
 */
#ifndef WEB_SERVER_H
#define WEB_SERVER_H

void init_wifi_base(void);
void wifi_manager_task(void *arg);

#endif // WEB_SERVER_H