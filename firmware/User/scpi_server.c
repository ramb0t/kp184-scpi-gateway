/* SCPI front end for the KP184.
 *
 * The command set is the subset shared by the Rigol DL3000, Siglent SDL1000X,
 * ITECH IT8500+ and BK Precision 8600 electronic loads, so software written
 * for those has a good chance of working unchanged:
 *
 *   [:SOURce]:FUNCtion {CURRent|VOLTage|RESistance|POWer}       and ?
 *   [:SOURce]:CURRent[:LEVel][:IMMediate][:AMPLitude] <A>        and ? [MIN|MAX]
 *   [:SOURce]:VOLTage..., RESistance..., POWer...                 likewise
 *   [:SOURce]:INPut[:STATe] {ON|OFF|1|0}                          and ?
 *   MEASure[:SCALar]:{VOLTage|CURRent|POWer|RESistance}[:DC]?     also as FETCh
 *   *IDN? *RST *TST? *CLS *ESE *ESR? *OPC *SRE *STB? *WAI, SYSTem:ERRor?
 *
 * *RST switches the input off and changes nothing else.
 * Numbers are written as fixed-point text from the load's integer counts, so
 * nothing here depends on floating-point formatting. */
#include <string.h>
#include "scpi/scpi.h"
#include "eth_driver.h"
#include "scpi_server.h"
#include "kp184.h"
#include "tick.h"

#define FW_VERSION          "eth485-0.2"

#define INPUT_BUFFER_LEN    256
#define ERROR_QUEUE_LEN     8
#define OUTPUT_BUFFER_LEN   256
#define SEND_TIMEOUT        TICK_MS(500)

/* One settable level: its register, scaling and limits in register counts. */
typedef struct {
    uint16_t    reg;
    scpi_unit_t unit;
    uint32_t    per_unit;       /* counts per volt, amp, ohm or watt */
    uint8_t     decimals;
    uint32_t    min;
    uint32_t    max;
} level_t;

enum { LEVEL_CURR = 0, LEVEL_VOLT, LEVEL_RES, LEVEL_POW };

/* Ratings of the KP184: 150 V, 40 A, 400 W. The resistance ceiling is the
 * manual's register range; the load itself has not been asked where it stops. */
static const level_t levels[] = {
    [LEVEL_CURR] = {KP184_REG_CC, SCPI_UNIT_AMPER, 1000, 3, 0, 40000},
    [LEVEL_VOLT] = {KP184_REG_CV, SCPI_UNIT_VOLT,  1000, 3, 0, 150000},
    [LEVEL_RES]  = {KP184_REG_CR, SCPI_UNIT_OHM,   10,   1, 1, 80000},
    [LEVEL_POW]  = {KP184_REG_CW, SCPI_UNIT_WATT,  100,  2, 0, 40000},
};

enum { MEAS_VOLT = 0, MEAS_CURR, MEAS_POW, MEAS_RES };

static const scpi_choice_def_t function_def[] = {
    {"VOLTage",    KP184_MODE_CV},
    {"CURRent",    KP184_MODE_CC},
    {"RESistance", KP184_MODE_CR},
    {"POWer",      KP184_MODE_CW},
    SCPI_CHOICE_LIST_END
};
static const char * const function_name[] = {"VOLTAGE", "CURRENT", "RESISTANCE", "POWER"};

static scpi_t ctx;
static char input_buffer[INPUT_BUFFER_LEN];
static scpi_error_t error_queue[ERROR_QUEUE_LEN];
static char output_buffer[OUTPUT_BUFFER_LEN];
static uint16_t output_len;
static uint8_t sock;
static uint8_t connected;

/* --- output to the TCP connection ----------------------------------------- */

static void send_output(void)
{
    uint32_t done = 0, len;
    uint16_t t0 = Tick_Now();

    while(connected && (done < output_len))
    {
        len = output_len - done;
        if(WCHNET_SocketSend(sock, (uint8_t *)output_buffer + done, &len) == WCHNET_ERR_SUCCESS)
            done += len;
        if(done < output_len)
        {
            if(Tick_Since(t0) > SEND_TIMEOUT) break;
            WCHNET_MainTask();
        }
    }
    output_len = 0;
}

static size_t scpi_write(scpi_t *context, const char *data, size_t len)
{
    size_t i;

    (void)context;
    for(i = 0; i < len; i++)
    {
        if(output_len == sizeof(output_buffer)) send_output();
        output_buffer[output_len++] = data[i];
    }
    return len;
}

static scpi_result_t scpi_flush(scpi_t *context)
{
    (void)context;
    send_output();
    return SCPI_RES_OK;
}

static int scpi_error(scpi_t *context, int_fast16_t err)
{
    (void)context;
    (void)err;
    return 0;
}

static scpi_result_t scpi_control(scpi_t *context, scpi_ctrl_name_t ctrl, scpi_reg_val_t val)
{
    (void)context;
    (void)ctrl;
    (void)val;
    return SCPI_RES_OK;
}

/* --- helpers --------------------------------------------------------------- */

/* Queue the SCPI error for a failed exchange with the load. */
static scpi_result_t bus_result(scpi_t *context, kp184_err_t r)
{
    switch(r)
    {
        case KP184_OK:          return SCPI_RES_OK;
        case KP184_ERR_TIMEOUT: SCPI_ErrorPush(context, SCPI_ERROR_TIME_OUT); break;
        case KP184_ERR_BUSY:    SCPI_ErrorPush(context, SCPI_ERROR_BUS_IN_USE); break;
        default:                SCPI_ErrorPush(context, SCPI_ERROR_COMMUNICATION_ERROR); break;
    }
    return SCPI_RES_ERR;
}

/* Write value / 10^decimals as plain decimal text, e.g. 24031 with 3 -> 24.031. */
static void result_fixed(scpi_t *context, uint64_t value, uint8_t decimals)
{
    char text[24];
    uint8_t pos = sizeof(text), digits = 0;

    do
    {
        text[--pos] = '0' + (char)(value % 10);
        value /= 10;
        digits++;
        if((digits == decimals) && decimals) text[--pos] = '.';
    } while(value || (digits <= decimals));
    SCPI_ResultCharacters(context, text + pos, sizeof(text) - pos);
}

/* --- IEEE 488.2 ------------------------------------------------------------ */

static scpi_result_t scpi_reset(scpi_t *context)
{
    return bus_result(context, KP184_Write(KP184_REG_OUTPUT, 0));
}

/* *TST? is 0 when the load answers and identifies as a KP184, otherwise 1.
 * It queues no error: the parser sends no reply for a command that raised one. */
static scpi_result_t cmd_tst_q(scpi_t *context)
{
    uint32_t model = 0;
    kp184_err_t r = KP184_Read(KP184_REG_MODEL, &model);

    SCPI_ResultInt32(context, ((r == KP184_OK) && (model == KP184_MODEL_KP184)) ? 0 : 1);
    return SCPI_RES_OK;
}

/* --- load commands ---------------------------------------------------------- */

static scpi_result_t cmd_function(scpi_t *context)
{
    int32_t mode;
    uint32_t now;
    kp184_err_t r;

    if(!SCPI_ParamChoice(context, function_def, &mode, TRUE)) return SCPI_RES_ERR;
    r = KP184_Write(KP184_REG_MODE, (uint32_t)mode);
    if(r == KP184_OK) r = KP184_Read(KP184_REG_MODE, &now);
    if(r != KP184_OK) return bus_result(context, r);
    if(now != (uint32_t)mode)
    {
        /* The load keeps its mode while the input is on. */
        SCPI_ErrorPush(context, SCPI_ERROR_SETTINGS_CONFLICT);
        return SCPI_RES_ERR;
    }
    return SCPI_RES_OK;
}

static scpi_result_t cmd_function_q(scpi_t *context)
{
    uint32_t mode;
    kp184_err_t r = KP184_Read(KP184_REG_MODE, &mode);

    if(r != KP184_OK) return bus_result(context, r);
    if(mode > KP184_MODE_CW)
    {
        SCPI_ErrorPush(context, SCPI_ERROR_COMMUNICATION_ERROR);
        return SCPI_RES_ERR;
    }
    SCPI_ResultMnemonic(context, function_name[mode]);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_level(scpi_t *context)
{
    const level_t *lv = &levels[SCPI_CmdTag(context)];
    scpi_number_t n;
    uint32_t counts;
    double scaled;

    if(!SCPI_ParamNumber(context, scpi_special_numbers_def, &n, TRUE)) return SCPI_RES_ERR;
    if(n.special)
    {
        if(n.content.tag == SCPI_NUM_MIN) counts = lv->min;
        else if(n.content.tag == SCPI_NUM_MAX) counts = lv->max;
        else
        {
            SCPI_ErrorPush(context, SCPI_ERROR_ILLEGAL_PARAMETER_VALUE);
            return SCPI_RES_ERR;
        }
    }
    else
    {
        if((n.unit != SCPI_UNIT_NONE) && (n.unit != lv->unit))
        {
            SCPI_ErrorPush(context, SCPI_ERROR_INVALID_SUFFIX);
            return SCPI_RES_ERR;
        }
        scaled = n.content.value * lv->per_unit + 0.5;      /* round to the nearest count */
        if(!((scaled >= (double)lv->min) && (scaled < (double)lv->max + 1.0)))
        {
            SCPI_ErrorPush(context, SCPI_ERROR_DATA_OUT_OF_RANGE);
            return SCPI_RES_ERR;
        }
        counts = (uint32_t)scaled;
    }
    return bus_result(context, KP184_Write(lv->reg, counts));
}

static scpi_result_t cmd_level_q(scpi_t *context)
{
    const level_t *lv = &levels[SCPI_CmdTag(context)];
    scpi_number_t n;
    uint32_t counts;
    kp184_err_t r;

    if(SCPI_ParamNumber(context, scpi_special_numbers_def, &n, FALSE))
    {
        if(n.special && (n.content.tag == SCPI_NUM_MIN)) counts = lv->min;
        else if(n.special && (n.content.tag == SCPI_NUM_MAX)) counts = lv->max;
        else
        {
            SCPI_ErrorPush(context, SCPI_ERROR_ILLEGAL_PARAMETER_VALUE);
            return SCPI_RES_ERR;
        }
    }
    else
    {
        if(SCPI_ParamErrorOccurred(context)) return SCPI_RES_ERR;
        r = KP184_Read(lv->reg, &counts);
        if(r != KP184_OK) return bus_result(context, r);
    }
    result_fixed(context, counts, lv->decimals);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_input(scpi_t *context)
{
    scpi_bool_t on;

    if(!SCPI_ParamBool(context, &on, TRUE)) return SCPI_RES_ERR;
    return bus_result(context, KP184_Write(KP184_REG_OUTPUT, on ? 1 : 0));
}

static scpi_result_t cmd_input_q(scpi_t *context)
{
    kp184_status_t st;
    kp184_err_t r = KP184_Status(&st);      /* the output register's reply is unreliable, the status bit is not */

    if(r != KP184_OK) return bus_result(context, r);
    SCPI_ResultInt32(context, st.output_on);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_measure_q(scpi_t *context)
{
    kp184_status_t st;
    kp184_err_t r = KP184_Status(&st);      /* voltage and current from one frame, so power and resistance are consistent */

    if(r != KP184_OK) return bus_result(context, r);
    switch(SCPI_CmdTag(context))
    {
        case MEAS_VOLT:
            result_fixed(context, st.voltage_mv, 3);
            break;
        case MEAS_CURR:
            result_fixed(context, st.current_ma, 3);
            break;
        case MEAS_POW:      /* mV x mA is microwatts; report milliwatt resolution */
            result_fixed(context, ((uint64_t)st.voltage_mv * st.current_ma + 500) / 1000, 3);
            break;
        default:            /* resistance in milliohms; infinite with no current */
            if(st.current_ma == 0) SCPI_ResultMnemonic(context, "9.9E37");
            else result_fixed(context, ((uint64_t)st.voltage_mv * 1000 + st.current_ma / 2) / st.current_ma, 3);
            break;
    }
    return SCPI_RES_OK;
}

static const scpi_command_t commands[] = {
    {"*CLS", SCPI_CoreCls, 0},
    {"*ESE", SCPI_CoreEse, 0},
    {"*ESE?", SCPI_CoreEseQ, 0},
    {"*ESR?", SCPI_CoreEsrQ, 0},
    {"*IDN?", SCPI_CoreIdnQ, 0},
    {"*OPC", SCPI_CoreOpc, 0},
    {"*OPC?", SCPI_CoreOpcQ, 0},
    {"*RST", SCPI_CoreRst, 0},
    {"*SRE", SCPI_CoreSre, 0},
    {"*SRE?", SCPI_CoreSreQ, 0},
    {"*STB?", SCPI_CoreStbQ, 0},
    {"*TST?", cmd_tst_q, 0},
    {"*WAI", SCPI_CoreWai, 0},

    {"SYSTem:ERRor[:NEXT]?", SCPI_SystemErrorNextQ, 0},
    {"SYSTem:ERRor:COUNt?", SCPI_SystemErrorCountQ, 0},
    {"SYSTem:VERSion?", SCPI_SystemVersionQ, 0},
    {"STATus:QUEStionable[:EVENt]?", SCPI_StatusQuestionableEventQ, 0},
    {"STATus:QUEStionable:ENABle", SCPI_StatusQuestionableEnable, 0},
    {"STATus:QUEStionable:ENABle?", SCPI_StatusQuestionableEnableQ, 0},
    {"STATus:PRESet", SCPI_StatusPreset, 0},

    {"[:SOURce]:FUNCtion", cmd_function, 0},
    {"[:SOURce]:FUNCtion?", cmd_function_q, 0},
    {"[:SOURce]:CURRent[:LEVel][:IMMediate][:AMPLitude]", cmd_level, LEVEL_CURR},
    {"[:SOURce]:CURRent[:LEVel][:IMMediate][:AMPLitude]?", cmd_level_q, LEVEL_CURR},
    {"[:SOURce]:VOLTage[:LEVel][:IMMediate][:AMPLitude]", cmd_level, LEVEL_VOLT},
    {"[:SOURce]:VOLTage[:LEVel][:IMMediate][:AMPLitude]?", cmd_level_q, LEVEL_VOLT},
    {"[:SOURce]:RESistance[:LEVel][:IMMediate][:AMPLitude]", cmd_level, LEVEL_RES},
    {"[:SOURce]:RESistance[:LEVel][:IMMediate][:AMPLitude]?", cmd_level_q, LEVEL_RES},
    {"[:SOURce]:POWer[:LEVel][:IMMediate][:AMPLitude]", cmd_level, LEVEL_POW},
    {"[:SOURce]:POWer[:LEVel][:IMMediate][:AMPLitude]?", cmd_level_q, LEVEL_POW},
    {"[:SOURce]:INPut[:STATe]", cmd_input, 0},
    {"[:SOURce]:INPut[:STATe]?", cmd_input_q, 0},

    {"MEASure[:SCALar]:VOLTage[:DC]?", cmd_measure_q, MEAS_VOLT},
    {"MEASure[:SCALar]:CURRent[:DC]?", cmd_measure_q, MEAS_CURR},
    {"MEASure[:SCALar]:POWer[:DC]?", cmd_measure_q, MEAS_POW},
    {"MEASure[:SCALar]:RESistance[:DC]?", cmd_measure_q, MEAS_RES},
    {"FETCh[:SCALar]:VOLTage[:DC]?", cmd_measure_q, MEAS_VOLT},
    {"FETCh[:SCALar]:CURRent[:DC]?", cmd_measure_q, MEAS_CURR},
    {"FETCh[:SCALar]:POWer[:DC]?", cmd_measure_q, MEAS_POW},
    {"FETCh[:SCALar]:RESistance[:DC]?", cmd_measure_q, MEAS_RES},

    SCPI_CMD_LIST_END
};

static scpi_interface_t interface = {
    .error = scpi_error,
    .write = scpi_write,
    .control = scpi_control,
    .flush = scpi_flush,
    .reset = scpi_reset,
};

/* --- connection handling ---------------------------------------------------- */

void Scpi_Init(void)
{
    SCPI_Init(&ctx, commands, &interface, scpi_units_def,
              "Kunkin", "KP184", "0", FW_VERSION,
              input_buffer, sizeof(input_buffer),
              error_queue, ERROR_QUEUE_LEN);
}

void Scpi_Connect(uint8_t socketid)
{
    sock = socketid;
    output_len = 0;
    ctx.buffer.position = 0;        /* drop a half-typed line from the previous client */
    SCPI_ErrorClear(&ctx);
    connected = 1;
}

void Scpi_Disconnect(void)
{
    connected = 0;
}

uint8_t Scpi_IsConnected(void)
{
    return connected;
}

uint8_t Scpi_Socket(void)
{
    return sock;
}

void Scpi_Poll(void)
{
    static uint8_t chunk[128];
    uint32_t len;

    while(connected)
    {
        len = WCHNET_SocketRecvLen(sock, NULL);
        if(len == 0) break;
        if(len > sizeof(chunk)) len = sizeof(chunk);
        if((WCHNET_SocketRecv(sock, chunk, &len) != WCHNET_ERR_SUCCESS) || (len == 0)) break;
        SCPI_Input(&ctx, (const char *)chunk, (int)len);
    }
}
