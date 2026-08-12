/*
 * Bafang LCD SW102 Bluetooth firmware
 *
 * Released under the GPL License, Version 3
 */
#include <string.h>
#include "common.h"
#include "fstorage.h"
#include "ble_services.h"
#include "ble_hci.h"
#include "ble_advdata.h"
#include "ble_conn_params.h"
#include "ble_advertising.h"
#include "peer_manager.h"
#include "softdevice_handler.h"
#include "app_timer.h"
#include "ble_bas.h"
#include "ble_cscs.h"
#include "ble_dis.h"
#include "fds.h"
#include "state.h"
#include "ble_conn_state.h"

// define to report wheel speed via the standard Cycling Speed & Cadence service
#define BLE_CSC
// define to enable reporting battery SOC via bluetooth
#define BLE_BAS
// define to broadcast all live telemetry via a custom 128-bit GATT service
#define BLE_TELEMETRY

#define IS_SRVC_CHANGED_CHARACT_PRESENT 0                                           /**< Include the service_changed characteristic. If not enabled, the server's database cannot be changed for the lifetime of the device. */

#if (NRF_SD_BLE_API_VERSION == 3)
#define NRF_BLE_MAX_MTU_SIZE            GATT_MTU_SIZE_DEFAULT                       /**< MTU size used in the softdevice enabling and to reply to a BLE_GATTS_EVT_EXCHANGE_MTU_REQUEST event. */
#endif

#define APP_FEATURE_NOT_SUPPORTED       BLE_GATT_STATUS_ATTERR_APP_BEGIN + 2        /**< Reply when unsupported features are requested. */

#define CENTRAL_LINK_COUNT              0                                           /**< Number of central links used by the application. When changing this number remember to adjust the RAM settings*/
#define PERIPHERAL_LINK_COUNT           1                                           /**< Number of peripheral links used by the application. When changing this number remember to adjust the RAM settings*/

#define DEVICE_NAME                     "swang-stodva"                              /**< Name of device. Will be included in the advertising data. */
#define MANUFACTURER_NAME               "https://github.com/OmoYauhen/swang-stodva"

#define APP_ADV_INTERVAL                64                                          /**< The advertising interval (in units of 0.625 ms. This value corresponds to 100 ms). */
#define APP_ADV_TIMEOUT_IN_SECONDS      180                                         /**< The advertising timeout (in units of seconds). */

#define MIN_CONN_INTERVAL               MSEC_TO_UNITS(500, UNIT_1_25_MS)             /**< Minimum acceptable connection interval (20 ms), Connection interval uses 1.25 ms units. */
#define MAX_CONN_INTERVAL               MSEC_TO_UNITS(1000, UNIT_1_25_MS)             /**< Maximum acceptable connection interval (75 ms), Connection interval uses 1.25 ms units. */
#define SLAVE_LATENCY                   0                                           /**< Slave latency. */
#define CONN_SUP_TIMEOUT                MSEC_TO_UNITS(4000, UNIT_10_MS)             /**< Connection supervisory timeout (4 seconds), Supervision Timeout uses 10 ms units. */
#define FIRST_CONN_PARAMS_UPDATE_DELAY  APP_TIMER_TICKS(5000, APP_TIMER_PRESCALER)  /**< Time from initiating event (connect or start of notification) to first time sd_ble_gap_conn_param_update is called (5 seconds). */
#define NEXT_CONN_PARAMS_UPDATE_DELAY   APP_TIMER_TICKS(30000, APP_TIMER_PRESCALER) /**< Time between each call to sd_ble_gap_conn_param_update after the first call (30 seconds). */
#define MAX_CONN_PARAMS_UPDATE_COUNT    3                                           /**< Number of attempts before giving up the connection parameter negotiation. */

static uint16_t                         m_conn_handle = BLE_CONN_HANDLE_INVALID;    /**< Handle of the current connection. */

// Only 16-bit standard UUIDs are advertised: two 128-bit UUIDs (NUS + telemetry)
// would overflow the 31-byte scan response. The 128-bit vendor services stay
// discoverable after connecting.
static ble_uuid_t                       m_adv_uuids[] = {
#ifdef BLE_CSC
    {BLE_UUID_CYCLING_SPEED_AND_CADENCE, BLE_UUID_TYPE_BLE},
#endif
#ifdef BLE_BAS
    {BLE_UUID_BATTERY_SERVICE, BLE_UUID_TYPE_BLE},
#endif
    {BLE_UUID_DEVICE_INFORMATION_SERVICE, BLE_UUID_TYPE_BLE},
};  /**< Universally unique service identifier. */

/**@brief Function for the GAP initialization.
 *
 * @details This function will set up all the necessary GAP (Generic Access Profile) parameters of
 *          the device. It also sets the permissions and appearance.
 */
static void gap_params_init(void)
{
    ble_gap_conn_params_t   gap_conn_params;
    ble_gap_conn_sec_mode_t sec_mode;

    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&sec_mode);

    APP_ERROR_CHECK(sd_ble_gap_device_name_set(&sec_mode, (const uint8_t *) DEVICE_NAME, strlen(DEVICE_NAME)));

//    APP_ERROR_CHECK(sd_ble_gap_appearance_set(BLE_APPEARANCE_GENERIC_CYCLING));

    memset(&gap_conn_params, 0, sizeof(gap_conn_params));

    gap_conn_params.min_conn_interval = MIN_CONN_INTERVAL;
    gap_conn_params.max_conn_interval = MAX_CONN_INTERVAL;
    gap_conn_params.slave_latency     = SLAVE_LATENCY;
    gap_conn_params.conn_sup_timeout  = CONN_SUP_TIMEOUT;

    APP_ERROR_CHECK(sd_ble_gap_ppcp_set(&gap_conn_params));
}


#ifdef BLE_CSC

#define SPEED_AND_CADENCE_MEAS_INTERVAL APP_TIMER_TICKS(1000, APP_TIMER_PRESCALER)  /**< Speed and cadence measurement interval (milliseconds). */


APP_TIMER_DEF(m_csc_meas_timer_id);                                                 /**< CSC measurement timer. */

static ble_sensor_location_t supported_locations[] = {BLE_SENSOR_LOCATION_FRONT_WHEEL,
                                                      BLE_SENSOR_LOCATION_LEFT_CRANK,
                                                      BLE_SENSOR_LOCATION_RIGHT_CRANK,
                                                      BLE_SENSOR_LOCATION_LEFT_PEDAL,
                                                      BLE_SENSOR_LOCATION_RIGHT_PEDAL,
                                                      BLE_SENSOR_LOCATION_FRONT_HUB,
                                                      BLE_SENSOR_LOCATION_REAR_DROPOUT,
                                                      BLE_SENSOR_LOCATION_CHAINSTAY,
                                                      BLE_SENSOR_LOCATION_REAR_WHEEL,
                                                      BLE_SENSOR_LOCATION_REAR_HUB}; /**< supported location for the sensor location. */


static ble_cscs_t m_cscs;                                                           /**< Structure used to identify the cycling speed and cadence service. */

static uint32_t m_cumulative_wheel_revs;                                            /**< Cumulative wheel revolutions. */
static bool     m_auto_calibration_in_progress;                                     /**< Set when an autocalibration is in progress. */


#define KPH_TO_MM_PER_SEC               278                                         /**< Constant to convert kilometers per hour into millimeters per second. */

// The CSC measurement timer fires every 1000 ms (see SPEED_AND_CADENCE_MEAS_INTERVAL).
#define CSC_MEAS_PERIOD_MS 1000

static void csc_measurement(ble_cscs_meas_t * p_measurement)
{
    static uint16_t event_time_1024        = 0;  // free-running clock, 1/1024 s units
    static uint16_t partial_mm             = 0;  // sub-revolution distance carried over
    static uint16_t last_wheel_event_time  = 0;  // timestamp of the most recent wheel revolution

    // This hardware has no cadence source, so only wheel-revolution data is reported.
    p_measurement->is_wheel_rev_data_present = true;
    p_measurement->is_crank_rev_data_present = false;

    // Advance the free-running clock by one measurement period (units of 1/1024 s).
    event_time_1024 += (1024 * CSC_MEAS_PERIOD_MS) / 1000;

    // Distance covered this period. ui16_wheel_speed_x10 is 0.1 km/h; 1 km/h = 278 mm/s.
    uint32_t mm_per_sec = (KPH_TO_MM_PER_SEC * (uint32_t) ui_vars.ui16_wheel_speed_x10) / 10;
    partial_mm += (uint16_t) (mm_per_sec * CSC_MEAS_PERIOD_MS / 1000);

    if (ui_vars.ui16_wheel_perimeter > 0)
    {
        uint16_t revs = partial_mm / ui_vars.ui16_wheel_perimeter;
        if (revs > 0)
        {
            // Only timestamp when the wheel actually turned, so a stopped bike
            // reports zero speed (both the count and the event time freeze).
            m_cumulative_wheel_revs += revs;
            partial_mm             %= ui_vars.ui16_wheel_perimeter;
            last_wheel_event_time   = event_time_1024;
        }
    }

    p_measurement->cumulative_wheel_revs = m_cumulative_wheel_revs;
    p_measurement->last_wheel_event_time = last_wheel_event_time;
}

/**@brief Function for handling the Cycling Speed and Cadence measurement timer timeouts.
 *
 * @details This function will be called each time the cycling speed and cadence
 *          measurement timer expires.
 *
 * @param[in] p_context  Pointer used for passing some arbitrary information (context) from the
 *                       app_start_timer() call to the timeout handler.
 */
static void csc_meas_timeout_handler(void * p_context)
{
    uint32_t        err_code;
    ble_cscs_meas_t cscs_measurement;

    UNUSED_PARAMETER(p_context);

    csc_measurement(&cscs_measurement);

    err_code = ble_cscs_measurement_send(&m_cscs, &cscs_measurement);
    if ((err_code != NRF_SUCCESS) &&
        (err_code != NRF_ERROR_INVALID_STATE) &&
        (err_code != BLE_ERROR_NO_TX_PACKETS) &&
        (err_code != BLE_ERROR_GATTS_SYS_ATTR_MISSING)
       )
    {
        APP_ERROR_HANDLER(err_code);
    }
    if (m_auto_calibration_in_progress)
    {
        err_code = ble_sc_ctrlpt_rsp_send(&(m_cscs.ctrl_pt), BLE_SCPT_SUCCESS);
        if ((err_code != NRF_SUCCESS) &&
            (err_code != NRF_ERROR_INVALID_STATE) &&
            (err_code != BLE_ERROR_NO_TX_PACKETS)
           )
        {
            APP_ERROR_HANDLER(err_code);
        }
        if (err_code != BLE_ERROR_NO_TX_PACKETS)
        {
            m_auto_calibration_in_progress = false;
        }
    }
}

/**@brief Function for handling Speed and Cadence Control point events
 *
 * @details Function for handling Speed and Cadence Control point events.
 *          This function parses the event and in case the "set cumulative value" event is received,
 *          sets the wheel cumulative value to the received value.
 */
ble_scpt_response_t sc_ctrlpt_event_handler(ble_sc_ctrlpt_t     * p_sc_ctrlpt,
                                            ble_sc_ctrlpt_evt_t * p_evt)
{
    switch (p_evt->evt_type)
    {
        case BLE_SC_CTRLPT_EVT_SET_CUMUL_VALUE:
            m_cumulative_wheel_revs = p_evt->params.cumulative_value;
            break;

        case BLE_SC_CTRLPT_EVT_START_CALIBRATION:
            m_auto_calibration_in_progress = true;
            break;

        default:
            // No implementation needed.
            break;
    }
    return (BLE_SCPT_SUCCESS);
}

static void csc_init() {
  ble_cscs_init_t       cscs_init;
  ble_sensor_location_t sensor_location;

  // Initialize Cycling Speed and Cadence Service.
  memset(&cscs_init, 0, sizeof(cscs_init));

  cscs_init.evt_handler = NULL;
  // Wheel speed only — this hardware has no cadence/crank sensor.
  cscs_init.feature     = BLE_CSCS_FEATURE_WHEEL_REV_BIT;

  // Here the sec level for the Cycling Speed and Cadence Service can be changed/increased.
  BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cscs_init.csc_meas_attr_md.cccd_write_perm);   // for the measurement characteristic, only the CCCD write permission can be set by the application, others are mandated by service specification
  BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cscs_init.csc_feature_attr_md.read_perm);      // for the feature characteristic, only the read permission can be set by the application, others are mandated by service specification
  BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cscs_init.csc_ctrlpt_attr_md.write_perm);      // for the SC control point characteristic, only the write permission and CCCD write can be set by the application, others are mandated by service specification
  BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cscs_init.csc_ctrlpt_attr_md.cccd_write_perm); // for the SC control point characteristic, only the write permission and CCCD write can be set by the application, others are mandated by service specification

  cscs_init.ctrplt_supported_functions = /* BLE_SRV_SC_CTRLPT_CUM_VAL_OP_SUPPORTED | */
                                         BLE_SRV_SC_CTRLPT_SENSOR_LOCATIONS_OP_SUPPORTED
                                         /* | BLE_SRV_SC_CTRLPT_START_CALIB_OP_SUPPORTED */;
  cscs_init.ctrlpt_evt_handler            = sc_ctrlpt_event_handler;
  cscs_init.list_supported_locations      = supported_locations;
  cscs_init.size_list_supported_locations = sizeof(supported_locations) /
                                            sizeof(ble_sensor_location_t);

  sensor_location           = BLE_SENSOR_LOCATION_FRONT_WHEEL;                 // initializes the sensor location to add the sensor location characteristic.
  cscs_init.sensor_location = &sensor_location;
  BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cscs_init.csc_sensor_loc_attr_md.read_perm); // for the sensor location characteristic, only the read permission can be set by the application, others are mendated by service specification

  APP_ERROR_CHECK(ble_cscs_init(&m_cscs, &cscs_init));

  APP_ERROR_CHECK(app_timer_create(&m_csc_meas_timer_id,
                              APP_TIMER_MODE_REPEATED,
                              csc_meas_timeout_handler));

  APP_ERROR_CHECK(app_timer_start(m_csc_meas_timer_id, SPEED_AND_CADENCE_MEAS_INTERVAL, NULL));
}

#endif

#ifdef BLE_BAS

#define BATTERY_LEVEL_MEAS_INTERVAL     APP_TIMER_TICKS(2000, APP_TIMER_PRESCALER)  /**< Battery level measurement interval (ticks). */

APP_TIMER_DEF(m_battery_timer_id);                                                  /**< Battery timer. */
static ble_bas_t  m_bas;                                                            /**< Structure used to identify the battery service. */

/**@brief Function for performing battery measurement and updating the Battery Level characteristic
 *        in Battery Service.
 */
static void battery_level_update(void)
{
    uint32_t err_code;
    uint8_t battery_level = ui8_g_battery_soc; // from 0 to 100

    err_code = ble_bas_battery_level_update(&m_bas, battery_level);
    if ((err_code != NRF_SUCCESS) &&
        (err_code != NRF_ERROR_INVALID_STATE) &&
        (err_code != BLE_ERROR_NO_TX_PACKETS) &&
        (err_code != BLE_ERROR_GATTS_SYS_ATTR_MISSING)
       )
    {
      APP_ERROR_HANDLER(err_code);
    }
}


/**@brief Function for handling the Battery measurement timer timeout.
 *
 * @details This function will be called each time the battery level measurement timer expires.
 *
 * @param[in] p_context  Pointer used for passing some arbitrary information (context) from the
 *                       app_start_timer() call to the timeout handler.
 */
static void battery_level_meas_timeout_handler(void * p_context)
{
    UNUSED_PARAMETER(p_context);
    battery_level_update();
}

static void bas_init() {
  // Initialize Battery Service.
  ble_bas_init_t        bas_init;
  memset(&bas_init, 0, sizeof(bas_init));

  // Here the sec level for the Battery Service can be changed/increased.
  BLE_GAP_CONN_SEC_MODE_SET_OPEN(&bas_init.battery_level_char_attr_md.cccd_write_perm);
  BLE_GAP_CONN_SEC_MODE_SET_OPEN(&bas_init.battery_level_char_attr_md.read_perm);
  BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(&bas_init.battery_level_char_attr_md.write_perm);

  BLE_GAP_CONN_SEC_MODE_SET_OPEN(&bas_init.battery_level_report_read_perm);

  bas_init.evt_handler          = NULL;
  bas_init.support_notification = true;
  bas_init.p_report_ref         = NULL;
  bas_init.initial_batt_level   = 100;

  APP_ERROR_CHECK(ble_bas_init(&m_bas, &bas_init));

  APP_ERROR_CHECK(app_timer_create(&m_battery_timer_id,
                              APP_TIMER_MODE_REPEATED,
                              battery_level_meas_timeout_handler));

  APP_ERROR_CHECK(app_timer_start(m_battery_timer_id, BATTERY_LEVEL_MEAS_INTERVAL, NULL));
}
#endif


#ifdef BLE_TELEMETRY

// Custom telemetry service: a single NOTIFY characteristic carrying all live ride
// data in one packed struct, for a custom reader (nRF Connect / DIY screen).
// 128-bit vendor base UUID, distinct from the Nordic UART Service base. The
// service/characteristic are the 0x0001 / 0x0002 slots within that base:
//   57430001-9a43-11e5-a58f-0002a5d5c51b  (service)
//   57430002-9a43-11e5-a58f-0002a5d5c51b  (telemetry characteristic)
#define BLE_UUID_TELEMETRY_SERVICE 0x0001
#define BLE_UUID_TELEMETRY_CHAR    0x0002
#define TELEMETRY_PACKET_VERSION   1

// Base UUID in little-endian byte order (the 0x0000 at [12..13] is the 16-bit slot).
static const ble_uuid128_t telemetry_base_uuid =
    {{0x1b, 0xc5, 0xd5, 0xa5, 0x02, 0x00, 0x8f, 0xa5,
      0xe5, 0x11, 0x43, 0x9a, 0x00, 0x00, 0x43, 0x57}};

typedef struct __attribute__((packed)) {
    uint8_t  version;       // packet format version (TELEMETRY_PACKET_VERSION)
    int16_t  speed_x10;     // wheel speed, 0.1 km/h
    uint8_t  soc;           // battery state of charge, %
    int16_t  power_w;       // battery power, W
    uint16_t volts_x10;     // battery voltage, 0.1 V
    uint16_t amps_x5;       // battery current, 0.2 A units (x5)
    int8_t   motor_temp_c;  // motor temperature, deg C
    uint8_t  assist_level;  // PAS / assist level
    uint8_t  brake;         // 1 = braking
    uint8_t  lights;        // 1 = lights on
} telemetry_packet_t;

static uint8_t                  m_telemetry_uuid_type;
static uint16_t                 m_telemetry_service_handle;
static ble_gatts_char_handles_t m_telemetry_char_handles;

static void telemetry_init(void)
{
    ble_uuid_t service_uuid;
    APP_ERROR_CHECK(sd_ble_uuid_vs_add(&telemetry_base_uuid, &m_telemetry_uuid_type));

    service_uuid.type = m_telemetry_uuid_type;
    service_uuid.uuid = BLE_UUID_TELEMETRY_SERVICE;
    APP_ERROR_CHECK(sd_ble_gatts_service_add(BLE_GATTS_SRVC_TYPE_PRIMARY,
                                             &service_uuid,
                                             &m_telemetry_service_handle));

    ble_gatts_char_md_t char_md;
    ble_gatts_attr_md_t cccd_md;
    ble_gatts_attr_md_t attr_md;
    ble_gatts_attr_t    attr_char_value;
    ble_uuid_t          char_uuid;
    telemetry_packet_t  initial = { .version = TELEMETRY_PACKET_VERSION };

    // CCCD (so clients can subscribe to notifications).
    memset(&cccd_md, 0, sizeof(cccd_md));
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cccd_md.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cccd_md.write_perm);
    cccd_md.vloc = BLE_GATTS_VLOC_STACK;

    memset(&char_md, 0, sizeof(char_md));
    char_md.char_props.read   = 1;
    char_md.char_props.notify = 1;
    char_md.p_cccd_md         = &cccd_md;

    char_uuid.type = m_telemetry_uuid_type;
    char_uuid.uuid = BLE_UUID_TELEMETRY_CHAR;

    memset(&attr_md, 0, sizeof(attr_md));
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&attr_md.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(&attr_md.write_perm);
    attr_md.vloc = BLE_GATTS_VLOC_STACK;

    memset(&attr_char_value, 0, sizeof(attr_char_value));
    attr_char_value.p_uuid    = &char_uuid;
    attr_char_value.p_attr_md = &attr_md;
    attr_char_value.init_len  = sizeof(telemetry_packet_t);
    attr_char_value.max_len   = sizeof(telemetry_packet_t);
    attr_char_value.p_value   = (uint8_t *) &initial;

    APP_ERROR_CHECK(sd_ble_gatts_characteristic_add(m_telemetry_service_handle,
                                                    &char_md,
                                                    &attr_char_value,
                                                    &m_telemetry_char_handles));
}

// Fill and notify the telemetry packet from the current live values.
static void telemetry_update(void)
{
    if (ui_vars.ui8_ble_broadcast_enabled == 0)
        return;

    if (m_conn_handle == BLE_CONN_HANDLE_INVALID)
        return;

    telemetry_packet_t pkt = {
        .version      = TELEMETRY_PACKET_VERSION,
        .speed_x10    = (int16_t)  ui_vars.ui16_wheel_speed_x10,
        .soc          =            ui8_g_battery_soc,
        .power_w      = (int16_t)  ui_vars.ui16_battery_power,
        .volts_x10    =            ui_vars.ui16_battery_voltage_filtered_x10,
        .amps_x5      =            ui_vars.ui16_battery_current_filtered_x5,
        .motor_temp_c = (int8_t)   ui_vars.ui8_motor_temperature,
        .assist_level =            ui_vars.ui8_assist_level,
        .brake        =            ui_vars.ui8_braking ? 1 : 0,
        .lights       =            ui_vars.ui8_lights  ? 1 : 0,
    };

    uint16_t len = sizeof(pkt);
    ble_gatts_hvx_params_t hvx = {
        .handle = m_telemetry_char_handles.value_handle,
        .type   = BLE_GATT_HVX_NOTIFICATION,
        .offset = 0,
        .p_len  = &len,
        .p_data = (uint8_t *) &pkt,
    };

    uint32_t err_code = sd_ble_gatts_hvx(m_conn_handle, &hvx);
    if ((err_code != NRF_SUCCESS) &&
        (err_code != NRF_ERROR_INVALID_STATE) &&
        (err_code != BLE_ERROR_NO_TX_PACKETS) &&
        (err_code != BLE_ERROR_GATTS_SYS_ATTR_MISSING))
    {
        APP_ERROR_HANDLER(err_code);
    }
}

#endif // BLE_TELEMETRY



/**@brief Function for initializing services that will be used by the application.
 */
static void services_init(void)
{
#ifdef BLE_CSC
    csc_init();
#endif

#ifdef BLE_BAS
    bas_init();
#endif

#ifdef BLE_TELEMETRY
    telemetry_init();
#endif

//    // Initialize Device Information Service.
//    ble_dis_init_t dis_init;
//    memset(&dis_init, 0, sizeof(dis_init));
//
//    ble_srv_ascii_to_utf8(&dis_init.manufact_name_str, MANUFACTURER_NAME);
//
//    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&dis_init.dis_attr_md.read_perm);
//    BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(&dis_init.dis_attr_md.write_perm);
//
//    APP_ERROR_CHECK(ble_dis_init(&dis_init));
}




/**@brief Function for handling an event from the Connection Parameters Module.
 *
 * @details This function will be called for all events in the Connection Parameters Module
 *          which are passed to the application.
 *
 * @note All this function does is to disconnect. This could have been done by simply setting
 *       the disconnect_on_fail config parameter, but instead we use the event handler
 *       mechanism to demonstrate its use.
 *
 * @param[in] p_evt  Event received from the Connection Parameters Module.
 */
static void on_conn_params_evt(ble_conn_params_evt_t * p_evt)
{
  switch(p_evt->evt_type) {
  case BLE_CONN_PARAMS_EVT_FAILED:
    APP_ERROR_CHECK(sd_ble_gap_disconnect(m_conn_handle, BLE_HCI_CONN_INTERVAL_UNACCEPTABLE));
    break;
  case BLE_CONN_PARAMS_EVT_SUCCEEDED:
    break;
  default:
    break;
  }
}


/**@brief Function for handling errors from the Connection Parameters module.
 *
 * @param[in] nrf_error  Error code containing information about what went wrong.
 */
static void conn_params_error_handler(uint32_t nrf_error)
{
    APP_ERROR_HANDLER(nrf_error);
}


/**@brief Function for initializing the Connection Parameters module.
 */
static void conn_params_init(void)
{
    ble_conn_params_init_t cp_init;

    memset(&cp_init, 0, sizeof(cp_init));

    cp_init.p_conn_params                  = NULL;
    cp_init.first_conn_params_update_delay = FIRST_CONN_PARAMS_UPDATE_DELAY;
    cp_init.next_conn_params_update_delay  = NEXT_CONN_PARAMS_UPDATE_DELAY;
    cp_init.max_conn_params_update_count   = MAX_CONN_PARAMS_UPDATE_COUNT;
    cp_init.start_on_notify_cccd_handle    = BLE_GATT_HANDLE_INVALID;
    cp_init.disconnect_on_fail             = false;
    cp_init.evt_handler                    = on_conn_params_evt;
    cp_init.error_handler                  = conn_params_error_handler;

    APP_ERROR_CHECK(ble_conn_params_init(&cp_init));
}


/**@brief Function for handling advertising events.
 *
 * @details This function will be called for advertising events which are passed to the application.
 *
 * @param[in] ble_adv_evt  Advertising event.
 */
static void on_adv_evt(ble_adv_evt_t ble_adv_evt)
{
    switch (ble_adv_evt)
    {
        case BLE_ADV_EVT_FAST:
            break;
        case BLE_ADV_EVT_IDLE:
            break;
        default:
            break;
    }
}


/**@brief Function for the application's SoftDevice event handler.
 *
 * @param[in] p_ble_evt SoftDevice event.
 */
static void on_ble_evt(ble_evt_t * p_ble_evt)
{
    switch (p_ble_evt->header.evt_id)
    {
        case BLE_GAP_EVT_CONNECTED:
            m_conn_handle = p_ble_evt->evt.gap_evt.conn_handle;
            break; // BLE_GAP_EVT_CONNECTED

        case BLE_GAP_EVT_DISCONNECTED:
            m_conn_handle = BLE_CONN_HANDLE_INVALID;
            break; // BLE_GAP_EVT_DISCONNECTED

        case BLE_GAP_EVT_SEC_PARAMS_REQUEST:
            break; // BLE_GAP_EVT_SEC_PARAMS_REQUEST

        case BLE_GATTS_EVT_SYS_ATTR_MISSING:
            // No system attributes have been stored.
            sd_ble_gatts_sys_attr_set(m_conn_handle, NULL, 0, 0);
            break; // BLE_GATTS_EVT_SYS_ATTR_MISSING

        case BLE_GATTC_EVT_TIMEOUT:
            // Disconnect on GATT Client timeout event.
            sd_ble_gap_disconnect(p_ble_evt->evt.gattc_evt.conn_handle, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
            break; // BLE_GATTC_EVT_TIMEOUT

        case BLE_GATTS_EVT_TIMEOUT:
            // Disconnect on GATT Server timeout event.
            sd_ble_gap_disconnect(p_ble_evt->evt.gatts_evt.conn_handle, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
            break; // BLE_GATTS_EVT_TIMEOUT

        case BLE_EVT_USER_MEM_REQUEST:
            sd_ble_user_mem_reply(p_ble_evt->evt.gattc_evt.conn_handle, NULL);
            break; // BLE_EVT_USER_MEM_REQUEST

        case BLE_GATTS_EVT_RW_AUTHORIZE_REQUEST:
        {
            ble_gatts_evt_rw_authorize_request_t  req;
            ble_gatts_rw_authorize_reply_params_t auth_reply;

            req = p_ble_evt->evt.gatts_evt.params.authorize_request;

            if (req.type != BLE_GATTS_AUTHORIZE_TYPE_INVALID)
            {
                if ((req.request.write.op == BLE_GATTS_OP_PREP_WRITE_REQ)     ||
                    (req.request.write.op == BLE_GATTS_OP_EXEC_WRITE_REQ_NOW) ||
                    (req.request.write.op == BLE_GATTS_OP_EXEC_WRITE_REQ_CANCEL))
                {
                    if (req.type == BLE_GATTS_AUTHORIZE_TYPE_WRITE)
                    {
                        auth_reply.type = BLE_GATTS_AUTHORIZE_TYPE_WRITE;
                    }
                    else
                    {
                        auth_reply.type = BLE_GATTS_AUTHORIZE_TYPE_READ;
                    }
                    auth_reply.params.write.gatt_status = APP_FEATURE_NOT_SUPPORTED;
                    sd_ble_gatts_rw_authorize_reply(p_ble_evt->evt.gatts_evt.conn_handle, &auth_reply);
                }
            }
        } break; // BLE_GATTS_EVT_RW_AUTHORIZE_REQUEST

#if (NRF_SD_BLE_API_VERSION == 3)
        case BLE_GATTS_EVT_EXCHANGE_MTU_REQUEST:
            sd_ble_gatts_exchange_mtu_reply(p_ble_evt->evt.gatts_evt.conn_handle, NRF_BLE_MAX_MTU_SIZE);
            break; // BLE_GATTS_EVT_EXCHANGE_MTU_REQUEST
#endif

        default:
            // No implementation needed.
            break;
    }
}


/**@brief Function for dispatching a SoftDevice event to all modules with a SoftDevice
 *        event handler.
 *
 * @details This function is called from the SoftDevice event interrupt handler after a
 *          SoftDevice event has been received.
 *
 * @param[in] p_ble_evt  SoftDevice event.
 */
static void ble_evt_dispatch(ble_evt_t * p_ble_evt)
{
  ble_conn_state_on_ble_evt(p_ble_evt);
  pm_on_ble_evt(p_ble_evt);
  ble_conn_params_on_ble_evt(p_ble_evt);
  on_ble_evt(p_ble_evt);
  ble_advertising_on_ble_evt(p_ble_evt);
}


/**@brief Function for dispatching a system event to interested modules.
 *
 * @details This function is called from the System event interrupt handler after a system
 *          event has been received.
 *
 * @param[in] sys_evt  System stack event.
 */
static void sys_evt_dispatch(uint32_t sys_evt)
{
    // Dispatch the system event to the fstorage module, where it will be
    // dispatched to the Flash Data Storage (FDS) module.
    fs_sys_event_handler(sys_evt);

    // Dispatch to the Advertising module last, since it will check if there are any
    // pending flash operations in fstorage. Let fstorage process system events first,
    // so that it can report correctly to the Advertising module.
    ble_advertising_on_sys_evt(sys_evt);
}


/**@brief Function for the SoftDevice initialization.
 *
 * @details This function initializes the SoftDevice and the BLE event interrupt.
 */
static void ble_stack_init(void)
{
    //APP_TIMER_INIT(APP_TIMER_PRESCALER, APP_TIMER_OP_QUEUE_SIZE, false);

    nrf_clock_lf_cfg_t clock_lf_cfg = NRF_CLOCK_LFCLKSRC;

    // Initialize SoftDevice.
    SOFTDEVICE_HANDLER_INIT(&clock_lf_cfg, NULL);

    ble_enable_params_t ble_enable_params;
    softdevice_enable_get_default_config(CENTRAL_LINK_COUNT,
        PERIPHERAL_LINK_COUNT,
        &ble_enable_params);

    // One 128-bit vendor UUID base is in use: the custom telemetry service.
    ble_enable_params.common_enable_params.vs_uuid_count = 1;

    //Check the ram settings against the used number of links
    CHECK_RAM_START_ADDR(CENTRAL_LINK_COUNT, PERIPHERAL_LINK_COUNT);

    // Enable BLE stack.
#if (NRF_SD_BLE_API_VERSION == 3)
    ble_enable_params.gatt_enable_params.att_mtu = NRF_BLE_MAX_MTU_SIZE;
#endif
//    ble_enable_params.gatts_enable_params.service_changed = IS_SRVC_CHANGED_CHARACT_PRESENT;
    APP_ERROR_CHECK(softdevice_enable(&ble_enable_params));

    // Subscribe for BLE events.
    APP_ERROR_CHECK(softdevice_ble_evt_handler_set(ble_evt_dispatch));

    // Register with the SoftDevice handler module for system events.
    // Important for FDS and fstorage or event handler doesn't fire!
    APP_ERROR_CHECK(softdevice_sys_evt_handler_set(sys_evt_dispatch));
}

/**@brief Function for initializing the Advertising functionality.
 */
static void advertising_init(void)
{
    ble_advdata_t          advdata;
    ble_advdata_t          scanrsp;
    ble_adv_modes_config_t options;

    // Build advertising data struct to pass into @ref ble_advertising_init.
    memset(&advdata, 0, sizeof(advdata));
    advdata.name_type          = BLE_ADVDATA_FULL_NAME;
    advdata.include_appearance = false;
    advdata.flags              = BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE;

    memset(&scanrsp, 0, sizeof(scanrsp));
    scanrsp.uuids_complete.uuid_cnt = sizeof(m_adv_uuids) / sizeof(m_adv_uuids[0]);
    scanrsp.uuids_complete.p_uuids  = m_adv_uuids;

    memset(&options, 0, sizeof(options));
    options.ble_adv_fast_enabled  = true;
    options.ble_adv_fast_interval = APP_ADV_INTERVAL;
    options.ble_adv_fast_timeout  = APP_ADV_TIMEOUT_IN_SECONDS;

    APP_ERROR_CHECK(ble_advertising_init(&advdata, &scanrsp, &options, on_adv_evt, NULL));
}

static void peer_manager_event_handler(pm_evt_t const *p_evt)
{
    ret_code_t err_code;
    switch(p_evt->evt_id)
    {
        case PM_EVT_BONDED_PEER_CONNECTED:
            // Update the rank of the peer.
//            err_code = pm_peer_rank_highest(p_evt->peer_id);
            break;
        case PM_EVT_CONN_SEC_START:
            break;
        case PM_EVT_CONN_SEC_SUCCEEDED:
            // Update the rank of the peer.
            ble_conn_state_role(p_evt->conn_handle);
            break;
        case PM_EVT_CONN_SEC_FAILED:
            // In some cases, when securing fails, it can be restarted directly. Sometimes it can be
            // restarted, but only after changing some Security Parameters. Sometimes, it cannot be
            // restarted until the link is disconnected and reconnected. Sometimes it is impossible
            // to secure the link, or the peer device does not support it. How to handle this error
            // is highly application-dependent.
            m_conn_handle = BLE_CONN_HANDLE_INVALID;
            err_code = sd_ble_gap_disconnect(m_conn_handle, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
            APP_ERROR_CHECK(err_code);
            break;
        case PM_EVT_CONN_SEC_CONFIG_REQ:
        {
            // A connected peer (central) is trying to pair, but the Peer Manager already has a bond
            // for that peer. Setting allow_repairing to false rejects the pairing request.
            // If this event is ignored (pm_conn_sec_config_reply is not called in the event
            // handler), the Peer Manager assumes allow_repairing to be false.
            pm_conn_sec_config_t conn_sec_config = {.allow_repairing = false};
            pm_conn_sec_config_reply(p_evt->conn_handle, &conn_sec_config);
        }
        break;
        case PM_EVT_STORAGE_FULL:
            // Run garbage collection on the flash.
            err_code = fds_gc();
            if (err_code == FDS_ERR_BUSY || err_code == FDS_ERR_NO_SPACE_IN_QUEUES)
            {
                // Retry.
            }
            else
            {
                APP_ERROR_CHECK(err_code);
            }
            break;
        case PM_EVT_ERROR_UNEXPECTED:
            // Assert.
            APP_ERROR_CHECK(p_evt->params.error_unexpected.error);
            break;
        case PM_EVT_PEER_DATA_UPDATE_SUCCEEDED:
            break;
        case PM_EVT_PEER_DATA_UPDATE_FAILED:
            // Assert.
            APP_ERROR_CHECK_BOOL(false);
            break;
        case PM_EVT_PEER_DELETE_SUCCEEDED:
            break;
        case PM_EVT_PEER_DELETE_FAILED:
            // Assert.
            APP_ERROR_CHECK(p_evt->params.peer_delete_failed.error);
            break;
        case PM_EVT_PEERS_DELETE_SUCCEEDED:
            // At this point it is safe to start advertising or scanning.
            ble_advertising_start(BLE_ADV_MODE_FAST);
            break;
        case PM_EVT_PEERS_DELETE_FAILED:
            // Assert.
            APP_ERROR_CHECK(p_evt->params.peers_delete_failed_evt.error);
            break;
        case PM_EVT_LOCAL_DB_CACHE_APPLIED:
            break;
        case PM_EVT_LOCAL_DB_CACHE_APPLY_FAILED:
            // The local database has likely changed, send service changed indications.
            pm_local_database_has_changed();
            break;
        case PM_EVT_SERVICE_CHANGED_IND_SENT:
            break;
        case PM_EVT_SERVICE_CHANGED_IND_CONFIRMED:
            break;
    }
}


static void peer_init() {
  bool erase_bonds = false; // FIXME, have UX have a place to delete remembered BT devices
  ret_code_t err_code;
  err_code = pm_init();
  APP_ERROR_CHECK(err_code);
  if (erase_bonds)
  {
    pm_peers_delete();
  }

  ble_gap_sec_params_t sec_param;
  memset(&sec_param, 0, sizeof(ble_gap_sec_params_t));
  // Security parameters to be used for all security procedures.
  // per https://infocenter.nordicsemi.com/topic/com.nordic.infocenter.sdk5.v12.3.0/lib_peer_manager.html?cp=5_5_7_3_1_8
  // currently set to be super open and pair with anyone
  sec_param.bond = true;
  sec_param.mitm = false;
  sec_param.lesc = 0;
  sec_param.keypress = 0;
  sec_param.io_caps = BLE_GAP_IO_CAPS_NONE;
  sec_param.oob = false;
  sec_param.min_key_size = 7;
  sec_param.max_key_size = 16;
  sec_param.kdist_own.enc = 1;
  sec_param.kdist_own.id = 1;
  sec_param.kdist_peer.enc = 1;
  sec_param.kdist_peer.id = 1;
  err_code = pm_sec_params_set(&sec_param);
  APP_ERROR_CHECK(err_code);
  err_code = pm_register(peer_manager_event_handler);
  APP_ERROR_CHECK(err_code);
}

void ble_init(void)
{
  ble_stack_init();
  peer_init();
  gap_params_init();
  services_init();
  advertising_init();
  conn_params_init();
  APP_ERROR_CHECK(ble_advertising_start(BLE_ADV_MODE_FAST));
}

// Called every 100 ms from the main loop.
void send_bluetooth(rt_vars_t *rt_vars) {
 UNUSED_PARAMETER(rt_vars);

#ifdef BLE_TELEMETRY
 telemetry_update();
#endif
}
