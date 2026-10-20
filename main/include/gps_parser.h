/**
 * ==============================================================================
 * @file    gps_parser.h
 * @brief   Header interface for the GPS UART driver and NMEA sentence parser.
 * 
 * FUNCTIONS SUMMARY:
 * - gps_rx_task(): FreeRTOS background task that initializes the UART hardware,
 *   reads raw NMEA bytes, resyncs the RTC, and dispatches to the `minmea` parser.
 * ==============================================================================
 */

#ifndef GPS_PARSER_H
#define GPS_PARSER_H

/**
 * @brief FreeRTOS Task entry point for reading and parsing incoming GPS data.
 * @param arg Unused task parameter pointer.
 */
void gps_rx_task(void *arg);

#endif // GPS_PARSER_H