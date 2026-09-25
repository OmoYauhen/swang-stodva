/*
 * Bafang LCD SW102 Bluetooth firmware
 *
 * Copyright (C) lowPerformer, 2019.
 *
 * Released under the GPL License, Version 3
 */

#include <string.h>
#include "common.h"
#include "nrf_drv_uart.h"
#include "uart.h"
#include "utils.h"
#include "assert.h"
#include "app_util_platform.h"
#include "app_uart.h"

extern uint32_t _app_uart_init(const app_uart_comm_params_t * p_comm_params,
    app_uart_buffers_t *     p_buffers,
    app_uart_event_handler_t event_handler,
    app_irq_priority_t       irq_priority);
extern uint8_t app_uart_get(void);

#define UART_IRQ_PRIORITY                       APP_IRQ_PRIORITY_LOW

/**
 *@breif UART configuration structure
 */
static const app_uart_comm_params_t comm_params =
{
    .rx_pin_no  = UART_RX__PIN,
    .tx_pin_no  = UART_TX__PIN,
    .rts_pin_no = RTS_PIN_NUMBER,
    .cts_pin_no = CTS_PIN_NUMBER,
    //Below values are defined in ser_config.h common for application and connectivity
    .flow_control = APP_UART_FLOW_CONTROL_DISABLED,
    .use_parity   = false,
    // Bafang display UART: 1200 baud (TSDZ2 was 19200).
    .baud_rate    = UART_BAUDRATE_BAUDRATE_Baud1200
};

uint8_t ui8_rx_buffer[UART_NUMBER_DATA_BYTES_TO_RECEIVE];
uint8_t ui8_tx_buffer[UART_NUMBER_DATA_BYTES_TO_SEND];
volatile uint8_t ui8_received_package_flag = 0;

// Bafang replies have no start byte, no length byte, no CRC16 —
// only the requester knows the reply length (per opcode). The caller
// primes this via uart_prime_rx() before sending each request.
static volatile uint8_t ui8_expected_rx_len = 0;
static volatile uint8_t ui8_rx_cnt = 0;

uint8_t* uart_get_tx_buffer(void)
{
  return ui8_tx_buffer;
}

uint8_t* usart1_get_rx_buffer(void)
{
  return ui8_rx_buffer;
}

uint8_t usart1_received_package(void)
{
  return ui8_received_package_flag;
}

void usart1_reset_received_package(void)
{
  ui8_received_package_flag = 0;
  ui8_expected_rx_len = 0;
  ui8_rx_cnt = 0;
}

void uart_prime_rx(uint8_t expected_len)
{
  ui8_expected_rx_len = expected_len;
  ui8_rx_cnt = 0;
  ui8_received_package_flag = 0;
}

void uart_evt_callback(app_uart_evt_t * uart_evt)
{
  uint8_t ui8_byte_received;

  switch (uart_evt->evt_type)
  {
    case APP_UART_DATA:
      ui8_byte_received = app_uart_get();

      // If we have no active request or the buffer is already full,
      // silently drop stray bytes. Bafang has no framing to resync on.
      if (ui8_expected_rx_len == 0 || ui8_received_package_flag) {
        break;
      }

      if (ui8_rx_cnt < ui8_expected_rx_len) {
        ui8_rx_buffer[ui8_rx_cnt++] = ui8_byte_received;
        if (ui8_rx_cnt == ui8_expected_rx_len) {
          ui8_received_package_flag = 1;
        }
      }
      break;

    case APP_UART_TX_EMPTY:
      break;

    case APP_UART_COMMUNICATION_ERROR:
      ui8_rx_cnt = 0;
      break;

    default:
      break;
  }
}

/**
 * @brief Init UART peripheral
 */
void uart_init(void)
{
  uint32_t err_code;
  app_uart_buffers_t buffers;
  static uint8_t tx_buf[128]; // must be equal or higher than UART_NUMBER_DATA_BYTES_TO_SEND and power of 2

  buffers.tx_buf = tx_buf;
  buffers.tx_buf_size = sizeof (tx_buf);
  err_code = _app_uart_init(&comm_params, &buffers, uart_evt_callback, UART_IRQ_PRIORITY);

  APP_ERROR_CHECK(err_code);
}

/**
 * @brief Returns pointer to RX buffer ready for parsing or NULL
 */
const uint8_t* uart_get_rx_buffer_rdy(void)
{
  if(!usart1_received_package()) {
    return NULL;
  }

  uint8_t *r = usart1_get_rx_buffer();
  usart1_reset_received_package();
  return r;
}

/**
 * @brief Send TX buffer over UART.
 */
void uart_send_tx_buffer(uint8_t *tx_buffer, uint8_t ui8_len)
{
  uint32_t err_code;

  for (uint8_t i = 0; i < ui8_len; i++)
  {
    err_code = app_uart_put(tx_buffer[i]);
// assume that buffer will never get full, like for instance when we are debugging
//    if (err_code != 0)
//      APP_ERROR_CHECK(err_code);
  }
}
