/*
 * Copyright (c) 2018, Pycom Limited.
 *
 * This software is licensed under the GNU GPL version 3 or any
 * later version, with permitted additional terms. For more information
 * see the Pycom Licence v1.0 document supplied with this file, or
 * available at https://www.pycom.io/opensource/licensing
 */

#include <string.h>
#include "py/mpconfig.h"
#include "py/obj.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"

#include "driver/uart.h"
#include "driver/gpio.h"
#include "tcpip_adapter.h"
#include "netif/ppp/pppos.h"
#include "netif/ppp/ppp.h"
#include "lwip/pppapi.h"

#include "machpin.h"
#include "lteppp.h"
#include "pins.h"

/******************************************************************************
 DEFINE CONSTANTS
 ******************************************************************************/

#define LTE_TRX_WAIT_MS(len)                                    (((len + 1) * 12 * 1000) / MICROPY_LTE_UART_BAUDRATE)

#define LTE_CEREG_CHECK_PERIOD_MS                               (500)
#define LTE_TASK_PERIOD_MS                                      (2)

/******************************************************************************
 DEFINE TYPES
 ******************************************************************************/

/******************************************************************************
 DECLARE EXPORTED DATA
 ******************************************************************************/
extern TaskHandle_t xLTETaskHndl;

/******************************************************************************
 DECLARE PRIVATE DATA
 ******************************************************************************/
static char lteppp_trx_buffer[LTE_UART_BUFFER_SIZE];
static uart_dev_t* lteppp_uart_reg;
static QueueHandle_t xCmdQueue;
static QueueHandle_t xRxQueue;
static lte_state_t lte_state;
static SemaphoreHandle_t xLTESem;
static ppp_pcb *lteppp_pcb;         // PPP control block
struct netif lteppp_netif;          // PPP net interface

/******************************************************************************
 DECLARE PRIVATE FUNCTIONS
 ******************************************************************************/
static void TASK_LTE (void *pvParameters);
static bool lte_send_at_cmd(const char *cmd, uint32_t timeout);
static void lteppp_status_cb (ppp_pcb *pcb, int err_code, void *ctx);
static uint32_t lteppp_output_callback(ppp_pcb *pcb, u8_t *data, u32_t len, void *ctx);

/******************************************************************************
 DEFINE PUBLIC FUNCTIONS
 ******************************************************************************/
void lteppp_init(void) {
    lte_state = E_LTE_INIT;

    // configure the UART pins
    pin_config(MICROPY_LTE_TX_PIN, -1, U2TXD_OUT_IDX, GPIO_MODE_OUTPUT, MACHPIN_PULL_NONE, 1);
    pin_config(MICROPY_LTE_RX_PIN, U2RXD_IN_IDX, -1, GPIO_MODE_INPUT, MACHPIN_PULL_NONE, 1);
    pin_config(MICROPY_LTE_RTS_PIN, -1, U2RTS_OUT_IDX, GPIO_MODE_OUTPUT, MACHPIN_PULL_NONE, 1);
    pin_config(MICROPY_LTE_CTS_PIN, U2CTS_IN_IDX, -1, GPIO_MODE_INPUT, MACHPIN_PULL_NONE, 1);

    // initialize the UART interface
    uart_config_t config;
    config.baud_rate = MICROPY_LTE_UART_BAUDRATE;
    config.data_bits = UART_DATA_8_BITS;
    config.parity = UART_PARITY_DISABLE;
    config.stop_bits = UART_STOP_BITS_1;
    config.flow_ctrl = UART_HW_FLOWCTRL_CTS_RTS;
    config.rx_flow_ctrl_thresh = 64;
    uart_param_config(LTE_UART_ID, &config);

    // install the UART driver
    uart_driver_install(LTE_UART_ID, LTE_UART_BUFFER_SIZE, LTE_UART_BUFFER_SIZE, 0, NULL, 0, NULL);
    lteppp_uart_reg = &UART2;

    // disable the delay between transfers
    lteppp_uart_reg->idle_conf.tx_idle_num = 0;

    // configure the rx timeout threshold
    lteppp_uart_reg->conf1.rx_tout_thrhd = 20 & UART_RX_TOUT_THRHD_V;

    xCmdQueue = xQueueCreate(LTE_CMD_QUEUE_SIZE_MAX, sizeof(lte_task_cmd_data_t));
    xRxQueue = xQueueCreate(LTE_RSP_QUEUE_SIZE_MAX, LTE_AT_RSP_SIZE_MAX);

    xLTESem = xSemaphoreCreateMutex();

    lteppp_pcb = pppapi_pppos_create(&lteppp_netif, lteppp_output_callback, lteppp_status_cb, NULL);

    xTaskCreatePinnedToCore(TASK_LTE, "LTE", LTE_TASK_STACK_SIZE / sizeof(StackType_t), NULL, LTE_TASK_PRIORITY, &xLTETaskHndl, 1);
}

void lteppp_start (void) {
    xSemaphoreTake(xLTESem, portMAX_DELAY);
    if (E_LTE_INIT == lte_state) {
        lte_state = E_LTE_IDLE;
    }
    xSemaphoreGive(xLTESem);
}

bool lteppp_send_at_command (lte_task_cmd_data_t *cmd, lte_task_rsp_data_t *rsp) {
    xQueueSend(xCmdQueue, (void *)cmd, (TickType_t)portMAX_DELAY);
    xQueueReceive(xRxQueue, rsp, (TickType_t)portMAX_DELAY);
    if (rsp->ok) {
        return true;
    }
    return false;
}

lte_state_t lteppp_get_state(void) {
    lte_state_t state;
    xSemaphoreTake(xLTESem, portMAX_DELAY);
    state = lte_state;
    xSemaphoreGive(xLTESem);
    return state;
}

void lteppp_stop(void) {
    pppapi_close(lteppp_pcb, 0);
}

/******************************************************************************
 DEFINE PRIVATE FUNCTIONS
 ******************************************************************************/

static void TASK_LTE (void *pvParameters) {
    uint32_t reg_check_count = 0;

    lte_task_cmd_data_t *lte_task_cmd = (lte_task_cmd_data_t *)lteppp_trx_buffer;
    lte_task_rsp_data_t *lte_task_rsp = (lte_task_rsp_data_t *)lteppp_trx_buffer;

    vTaskDelay(1050 / portTICK_RATE_MS);
    if (lte_send_at_cmd("+++", 1050)) {
        lte_send_at_cmd("ATH", LTE_RX_TIMEOUT_MIN_MS);
        while (true) {
            if (lte_send_at_cmd("AT", LTE_RX_TIMEOUT_MIN_MS)) {
                break;
            }
        }
    } else {
        lte_send_at_cmd("AT", LTE_RX_TIMEOUT_MIN_MS);
        if (!lte_send_at_cmd("AT", LTE_RX_TIMEOUT_MIN_MS)) {
            vTaskDelay(1050 / portTICK_RATE_MS);
            if (!lte_send_at_cmd("+++", 1050)) {
                if (!lte_send_at_cmd("+++", 1050)) {
                    // nlr_raise(mp_obj_new_exception_msg(&mp_type_OSError, mpexception_os_operation_failed));
                }
            }
            vTaskDelay(550 / portTICK_RATE_MS);
        }
        lte_send_at_cmd("AT+SQNCTM?", LTE_RX_TIMEOUT_DEF_MS);
        if (!strstr(lteppp_trx_buffer, "verizon")) {
            lte_send_at_cmd("AT+SQNCTM=\"verizon\"", LTE_RX_TIMEOUT_DEF_MS);
            lte_send_at_cmd("AT", LTE_RX_TIMEOUT_DEF_MS);
            lte_send_at_cmd("AT", LTE_RX_TIMEOUT_DEF_MS);
        }
    }

    // enter low power mode
    lte_send_at_cmd("AT!=\"setlpm airplane=1 enable=1\"", LTE_RX_TIMEOUT_MIN_MS);
    uart_set_hw_flow_ctrl(LTE_UART_ID, UART_HW_FLOWCTRL_DISABLE, 0);
    uart_set_rts(LTE_UART_ID, 0);

    for (;;) {
        vTaskDelay(LTE_TASK_PERIOD_MS);

        if (xQueueReceive(xCmdQueue, lteppp_trx_buffer, 0)) {
            switch (lte_task_cmd->cmd) {
            case E_LTE_CMD_AT:
            case E_LTE_CMD_PPP_EXIT:
                if (lte_send_at_cmd(lte_task_cmd->data, lte_task_cmd->timeout)) {
                    lte_task_rsp->ok = true;
                    if (E_LTE_CMD_PPP_EXIT == lte_task_cmd->cmd) {
                        xSemaphoreTake(xLTESem, portMAX_DELAY);
                        lte_state = E_LTE_ATTACHED;
                        xSemaphoreGive(xLTESem);
                    }
                } else {
                    lte_task_rsp->ok = false;
                }
                printf("%s\n", lteppp_trx_buffer);
                xQueueSend(xRxQueue, (void *)lte_task_rsp, (TickType_t)portMAX_DELAY);
                break;
            case E_LTE_CMD_PPP_ENTER:
                lte_send_at_cmd(lte_task_cmd->data, lte_task_cmd->timeout);
                printf("%s\n", lteppp_trx_buffer);
                if (strstr(lteppp_trx_buffer, "CONNECT") != NULL) {
                    xSemaphoreTake(xLTESem, portMAX_DELAY);
                    lte_state = E_LTE_PPP;
                    xSemaphoreGive(xLTESem);
                    pppapi_set_default(lteppp_pcb);
                    pppapi_set_auth(lteppp_pcb, PPPAUTHTYPE_PAP, "", "");
                    pppapi_connect(lteppp_pcb, 0);
                }
                xQueueSend(xRxQueue, (void *)lte_task_rsp, (TickType_t)portMAX_DELAY);
                break;
            default:
                break;
            }
        } else {
            reg_check_count += LTE_TASK_PERIOD_MS;
            xSemaphoreTake(xLTESem, portMAX_DELAY);
            if (reg_check_count >= LTE_CEREG_CHECK_PERIOD_MS && lte_state >= E_LTE_IDLE && lte_state < E_LTE_PPP) {
                reg_check_count = 0;
                if (lte_send_at_cmd("AT+CEREG?", LTE_RX_TIMEOUT_DEF_MS)) {
                    char *pos;
                    if ((pos = strstr(lteppp_trx_buffer, "+CEREG: 2,1,")) && (strlen(pos) >= 21)) {
                        lte_state = E_LTE_ATTACHED;
                    } else {
                        lte_state = E_LTE_IDLE;
                    }
                } else {
                    lte_state = E_LTE_IDLE;
                }
                xSemaphoreGive(xLTESem);
            } else {
                xSemaphoreGive(xLTESem);
                uint32_t // try to read up to the size of the buffer
                rx_len = uart_read_bytes(LTE_UART_ID, (uint8_t *)lteppp_trx_buffer, sizeof(lteppp_trx_buffer), LTE_TRX_WAIT_MS(sizeof(lteppp_trx_buffer)) / portTICK_RATE_MS);
                if (rx_len > 0) {
                    pppos_input_tcpip(lteppp_pcb, (uint8_t *)lteppp_trx_buffer, rx_len);
                }
            }
        }
    }
}

static bool lte_send_at_cmd(const char *cmd, uint32_t timeout) {
    bool ret = false;
    uint32_t rx_len = 0;

    // uart_set_hw_flow_ctrl(LTE_UART_ID, UART_HW_FLOWCTRL_CTS_RTS, 0);

    uint32_t cmd_len = strlen(cmd);
    // flush the rx buffer first
    uart_flush(LTE_UART_ID);
    // then send the command
    uart_write_bytes(LTE_UART_ID, cmd, cmd_len);
    if (strcmp(cmd, "+++")) {
        uart_write_bytes(LTE_UART_ID, "\r\n", 2);
    }
    uart_wait_tx_done(LTE_UART_ID, LTE_TRX_WAIT_MS(cmd_len) / portTICK_RATE_MS);
    vTaskDelay(1 / portTICK_RATE_MS);

    // wait until characters start arriving
    do {
        vTaskDelay(1 / portTICK_RATE_MS);
        uart_get_buffered_data_len(LTE_UART_ID, &rx_len);
        if (timeout > 0) {
            timeout--;
        }
    } while (timeout > 0 && 0 == rx_len);

    memset(lteppp_trx_buffer, 0, sizeof(lteppp_trx_buffer));
    if (rx_len > 0) {
        // try to read up to the size of the buffer minus null terminator (minus 2 because we store the OK status in the last byte)
        rx_len = uart_read_bytes(LTE_UART_ID, (uint8_t *)lteppp_trx_buffer, sizeof(lteppp_trx_buffer) - 2, LTE_TRX_WAIT_MS(sizeof(lteppp_trx_buffer)) / portTICK_RATE_MS);
        if (rx_len > 0) {
            // NULL terminate the string
            lteppp_trx_buffer[rx_len] = '\0';
            if (strstr(lteppp_trx_buffer, LTE_OK_RSP) != NULL) {
                ret = true;
            }
            ret = false;
        }
    }
    // uart_set_hw_flow_ctrl(LTE_UART_ID, UART_HW_FLOWCTRL_DISABLE, 0);
    // uart_set_rts(LTE_UART_ID, 0);

    return ret;
}

// PPP output callback
static uint32_t lteppp_output_callback(ppp_pcb *pcb, u8_t *data, u32_t len, void *ctx) {
    LWIP_UNUSED_ARG(ctx);
    uint32_t tx_bytes = uart_write_bytes(LTE_UART_ID, (const char*)data, len);
    uart_wait_tx_done(LTE_UART_ID, LTE_TRX_WAIT_MS(len) / portTICK_RATE_MS);
    return tx_bytes;
}

// PPP status callback
static void lteppp_status_cb (ppp_pcb *pcb, int err_code, void *ctx) {
    struct netif *pppif = ppp_netif(pcb);
    LWIP_UNUSED_ARG(ctx);

    switch (err_code) {
    case PPPERR_NONE:
        printf("status_cb: Connected\n");
        #if PPP_IPV4_SUPPORT
        printf("ipaddr    = %s\n", ipaddr_ntoa(&pppif->ip_addr));
        printf("gateway   = %s\n", ipaddr_ntoa(&pppif->gw));
        printf("netmask   = %s\n", ipaddr_ntoa(&pppif->netmask));
        #endif
        #if PPP_IPV6_SUPPORT
        printf("ip6addr   = %s\n", ip6addr_ntoa(netif_ip6_addr(pppif, 0)));
        #endif
        break;
    case PPPERR_PARAM:
        printf("status_cb: Invalid parameter\n");
        break;
    case PPPERR_OPEN:
        printf("status_cb: Unable to open PPP session\n");
        break;
    case PPPERR_DEVICE:
        printf("status_cb: Invalid I/O device for PPP\n");
        break;
    case PPPERR_ALLOC:
        printf("status_cb: Unable to allocate resources\n");
        break;
    case PPPERR_USER:
        printf("status_cb: User interrupt (disconnected)\n");
        break;
    case PPPERR_CONNECT:
        printf("status_cb: Connection lost\n");
        break;
    case PPPERR_AUTHFAIL:
        printf("status_cb: Failed authentication challenge\n");
        break;
    case PPPERR_PROTOCOL:
        printf("status_cb: Failed to meet protocol\n");
        break;
    case PPPERR_PEERDEAD:
        printf("status_cb: Connection timeout\n");
        break;
    case PPPERR_IDLETIMEOUT:
        printf("status_cb: Idle Timeout\n");
        break;
    case PPPERR_CONNECTTIME:
        printf("status_cb: Max connect time reached\n");
        break;
    case PPPERR_LOOPBACK:
        printf("status_cb: Loopback detected\n");
        break;
    default:
        printf("status_cb: Unknown error code %d\n", err_code);
        break;
    }
}