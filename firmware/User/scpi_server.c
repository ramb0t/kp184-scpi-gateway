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
 *   [:SOURce]:{CURRent|VOLTage|RESistance|POWer}:{IRANGe|VRANGe}?   40 and 150: the
 *       KP184's single range. ngscopeclient's siglent_load driver asks for these
 *       and fails without an answer. Setting a range is not accepted (-113).
 *
 * Gateway-specific:
 *   SYSTem:FAILsafe[:STATe] {ON|OFF} and ?   input off when this client goes away (default ON)
 *   DIAGnostic:UPTime?                       seconds since the gateway started
 *   DIAGnostic:RESet:CAUSe?                  POWER, PIN, WATCHDOG or SOFTWARE
 *
 * *RST switches the input off and changes nothing else.
 * Numbers are written as fixed-point text from the load's integer counts, so
 * nothing here depends on floating-point formatting. */
#include <stdlib.h>
#include <string.h>
#include "scpi/scpi.h"
#include "eth_driver.h"
#include "scpi_server.h"
#include "netsock.h"
#include "kp184.h"
#include "failsafe.h"
#include "tick.h"
#include "wdg.h"

#define FW_VERSION          "eth485-0.2"

#define RECV_BUFFER_LEN     512
#define INPUT_BUFFER_LEN    256
#define OUTPUT_BUFFER_LEN   256
#define ERROR_QUEUE_LEN     8
/* Longest line accepted, terminator included, with room to spare in the parser's buffer. */
#define LINE_LIMIT          (INPUT_BUFFER_LEN - 8)
#define SEND_TIMEOUT        TICK_MS(500)

static scpi_t parser;
static char input_buffer[INPUT_BUFFER_LEN];
static scpi_error_t error_queue[ERROR_QUEUE_LEN];
static char output_buffer[OUTPUT_BUFFER_LEN];
static uint16_t output_len;
static uint8_t recv_buffer[RECV_BUFFER_LEN];

static net_session_t session;       /* the client, while session.active */
static uint8_t closing;             /* the client has closed: run what it sent, send nothing */
static uint16_t line_len;           /* bytes of the current line handed to the parser */
static uint8_t discarding;          /* dropping the rest of an overlong line */

/* Fail-safe: when a client that switched the input on goes away without
 * switching it off (closed, timed out, network gone), the gateway switches it
 * off. A client can opt out for its own session with SYST:FAIL OFF, e.g. to
 * start a long run and disconnect. Clients that never switched the input on
 * leave it alone, so a monitoring client does not stop someone else's run. */
static uint8_t failsafe;            /* this session's setting, ON at each connect */
static uint8_t turned_on;           /* this session switched the input on and has not switched it off */

static void end_session(void)
{
    if(!session.active) return;
    if(failsafe && turned_on) Failsafe_Trip();
    session.active = 0;
    printf("SCPI client gone\r\n");
}

/* --- output to the client ---------------------------------------------------- */

static void send_output(void)
{
    uint32_t done = 0, len;
    uint16_t t0 = Tick_Now();

    /* The connection is checked before every send: it may have closed, or
     * its slot gone to a new client, during any Net_Service(). */
    while(session.active && !closing && (done < output_len) && Net_SessionOpen(&session))
    {
        len = output_len - done;
        if(WCHNET_SocketSend(session.id, (uint8_t *)output_buffer + done, &len) == WCHNET_ERR_SUCCESS)
            done += len;
        if(done < output_len)
        {
            if(Tick_Since(t0) > SEND_TIMEOUT)
            {
                /* A reply cut short would run into the next one and read as a
                 * different number, so drop this client instead. */
                WCHNET_SocketClose(session.id, TCP_CLOSE_RST);
                end_session();
                break;
            }
            Net_Service();
        }
    }
    output_len = 0;
}

static size_t parser_write(scpi_t *context, const char *data, size_t len)
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

static scpi_result_t parser_flush(scpi_t *context)
{
    (void)context;
    send_output();
    return SCPI_RES_OK;
}

/* --- what the commands share --------------------------------------------------- */

static scpi_result_t fail(scpi_t *context, int16_t error)
{
    SCPI_ErrorPush(context, error);
    return SCPI_RES_ERR;
}

/* The outcome of an exchange with the load, as a command's result. */
static scpi_result_t load_result(scpi_t *context, kp184_err_t r)
{
    switch(r)
    {
        case KP184_OK:          return SCPI_RES_OK;
        case KP184_ERR_TIMEOUT: return fail(context, SCPI_ERROR_TIME_OUT);
        case KP184_ERR_BUSY:    return fail(context, SCPI_ERROR_BUS_IN_USE);
        default:                return fail(context, SCPI_ERROR_COMMUNICATION_ERROR);
    }
}

/* The parser notices a surplus parameter only after the command has run, so
 * "CURR 2,5" (a decimal comma) would set 2 A before the error. Commands that
 * change something call this first. */
static scpi_bool_t no_more_params(scpi_t *context)
{
    lex_state_t *lex = &context->param_list.lex_state;

    if(lex->pos < lex->buffer + lex->len)
    {
        SCPI_ErrorPush(context, SCPI_ERROR_PARAMETER_NOT_ALLOWED);
        return FALSE;
    }
    return TRUE;
}

static int skip_blanks(const char *text, int i)
{
    while((text[i] == ' ') || (text[i] == '\t')) i++;
    return i;
}

/* IEEE 488.2 allows white space around the exponent mark, and the lexer takes
 * "1 E-3" as one number, but the library's conversion stops at the space and
 * reads 1. Detect that form in the next parameter so it can be refused rather
 * than acted on with a value a thousand times too large. */
static scpi_bool_t split_exponent(scpi_t *context)
{
    lex_state_t *lex = &context->param_list.lex_state;
    const char *p = lex->pos;
    int n = (int)(lex->buffer + lex->len - lex->pos);
    char text[40];
    char *end;
    int i;

    while((n > 0) && ((*p == ' ') || (*p == '\t'))) { p++; n--; }
    if(n <= 0) return FALSE;
    if(n >= (int)sizeof(text)) n = sizeof(text) - 1;
    memcpy(text, p, n);
    text[n] = 0;
    strtod(text, &end);
    if(end == text) return FALSE;                   /* not a number: MIN, MAX, ON ... */
    i = skip_blanks(text, end - text);
    if((text[i] != 'E') && (text[i] != 'e')) return FALSE;     /* a suffix such as mA, or nothing */
    i = skip_blanks(text, i + 1);
    if((text[i] == '+') || (text[i] == '-')) i++;
    i = skip_blanks(text, i);
    return (text[i] >= '0') && (text[i] <= '9');
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

/* --- IEEE 488.2 ---------------------------------------------------------------- */

static scpi_result_t parser_reset(scpi_t *context)
{
    kp184_err_t r = KP184_SetInput(0);

    if(r == KP184_OK) turned_on = 0;
    return load_result(context, r);
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

/* --- mode ---------------------------------------------------------------------- */

static const scpi_choice_def_t function_def[] = {
    {"VOLTage",    KP184_MODE_CV},
    {"CURRent",    KP184_MODE_CC},
    {"RESistance", KP184_MODE_CR},
    {"POWer",      KP184_MODE_CW},
    SCPI_CHOICE_LIST_END
};
static const char * const function_name[] = {"VOLTAGE", "CURRENT", "RESISTANCE", "POWER"};

/* Measured on firmware 2128: the load acknowledges a switch to CV, reports
 * CV for a few tens of milliseconds and then goes back to its old mode, so
 * the mode is read back only after this. */
#define MODE_SETTLE         TICK_MS(150)

static scpi_result_t cmd_function(scpi_t *context)
{
    int32_t mode;
    uint32_t now;
    kp184_status_t st;
    kp184_err_t r;

    if(!SCPI_ParamChoice(context, function_def, &mode, TRUE)) return SCPI_RES_ERR;
    if(!no_more_params(context)) return SCPI_RES_ERR;

    /* The manual says the mode cannot change while the input is on, but
     * firmware 2128 switches at once, onto the other mode's setpoint. The
     * gateway refuses instead, as the manual describes. */
    r = KP184_Status(&st);
    if(r != KP184_OK) return load_result(context, r);
    if(st.output_on) return fail(context, SCPI_ERROR_SETTINGS_CONFLICT);

    r = KP184_Write(KP184_REG_MODE, (uint32_t)mode);
    if(r == KP184_OK)
    {
        Net_ServiceFor(MODE_SETTLE);
        r = KP184_Read(KP184_REG_MODE, &now);
    }
    if(r != KP184_OK) return load_result(context, r);
    /* The load did not take it; it never takes CV over the bus. */
    if(now != (uint32_t)mode) return fail(context, SCPI_ERROR_SETTINGS_CONFLICT);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_function_q(scpi_t *context)
{
    uint32_t mode;
    kp184_err_t r = KP184_Read(KP184_REG_MODE, &mode);

    if(r != KP184_OK) return load_result(context, r);
    if(mode > KP184_MODE_CW) return fail(context, SCPI_ERROR_COMMUNICATION_ERROR);
    SCPI_ResultMnemonic(context, function_name[mode]);
    return SCPI_RES_OK;
}

/* --- setpoints ----------------------------------------------------------------- */

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

/* MIN or MAX as counts; no other special value means anything here. */
static scpi_bool_t limit_counts(const level_t *lv, const scpi_number_t *n, uint32_t *counts)
{
    if(n->special && (n->content.tag == SCPI_NUM_MIN)) *counts = lv->min;
    else if(n->special && (n->content.tag == SCPI_NUM_MAX)) *counts = lv->max;
    else return FALSE;
    return TRUE;
}

static scpi_result_t cmd_level(scpi_t *context)
{
    const level_t *lv = &levels[SCPI_CmdTag(context)];
    scpi_number_t n;
    uint32_t counts;
    double scaled;

    if(split_exponent(context)) return fail(context, SCPI_ERROR_NUMERIC_DATA_ERROR);
    if(!SCPI_ParamNumber(context, scpi_special_numbers_def, &n, TRUE)) return SCPI_RES_ERR;
    if(!no_more_params(context)) return SCPI_RES_ERR;
    if(n.special)
    {
        if(!limit_counts(lv, &n, &counts)) return fail(context, SCPI_ERROR_ILLEGAL_PARAMETER_VALUE);
    }
    else
    {
        if((n.unit != SCPI_UNIT_NONE) && (n.unit != lv->unit)) return fail(context, SCPI_ERROR_INVALID_SUFFIX);
        scaled = n.content.value * lv->per_unit + 0.5;      /* round to the nearest count */
        if(!((scaled >= (double)lv->min) && (scaled < (double)lv->max + 1.0)))
            return fail(context, SCPI_ERROR_DATA_OUT_OF_RANGE);
        counts = (uint32_t)scaled;
    }
    return load_result(context, KP184_Write(lv->reg, counts));
}

static scpi_result_t cmd_level_q(scpi_t *context)
{
    const level_t *lv = &levels[SCPI_CmdTag(context)];
    scpi_number_t n;
    uint32_t counts;
    kp184_err_t r;

    if(SCPI_ParamNumber(context, scpi_special_numbers_def, &n, FALSE))
    {
        if(!limit_counts(lv, &n, &counts)) return fail(context, SCPI_ERROR_ILLEGAL_PARAMETER_VALUE);
    }
    else
    {
        if(SCPI_ParamErrorOccurred(context)) return SCPI_RES_ERR;
        r = KP184_Read(lv->reg, &counts);
        if(r != KP184_OK) return load_result(context, r);
    }
    result_fixed(context, counts, lv->decimals);
    return SCPI_RES_OK;
}

/* The KP184 has one range, so the tag is the answer: its full scale. */
enum { RANGE_AMPS = 40, RANGE_VOLTS = 150 };

static scpi_result_t cmd_range_q(scpi_t *context)
{
    SCPI_ResultInt32(context, SCPI_CmdTag(context));
    return SCPI_RES_OK;
}

/* --- input --------------------------------------------------------------------- */

static scpi_result_t cmd_input(scpi_t *context)
{
    scpi_parameter_t p;
    kp184_err_t r;
    int32_t choice;
    double v;
    uint8_t on;

    if(split_exponent(context)) return fail(context, SCPI_ERROR_NUMERIC_DATA_ERROR);
    if(!SCPI_Parameter(context, &p, TRUE)) return SCPI_RES_ERR;
    if(p.type == SCPI_TOKEN_PROGRAM_MNEMONIC)
    {
        if(!SCPI_ParamToChoice(context, &p, scpi_bool_def, &choice)) return SCPI_RES_ERR;
        on = choice ? 1 : 0;
    }
    else if(p.type == SCPI_TOKEN_DECIMAL_NUMERIC_PROGRAM_DATA)
    {
        /* SCPI rounds a numeric boolean to an integer. The library truncates,
         * which would switch the input on for "INP 1E-3". */
        if(!SCPI_ParamToDouble(context, &p, &v)) return SCPI_RES_ERR;
        on = ((v >= 0.5) || (v <= -0.5)) ? 1 : 0;
    }
    else
    {
        return fail(context, SCPI_ERROR_DATA_TYPE_ERROR);
    }
    if(!no_more_params(context)) return SCPI_RES_ERR;
    /* The rest of a line can still arrive here after send_output() has
     * dropped its client: nobody is left to switch on for. */
    if(on && !session.active) return fail(context, SCPI_ERROR_EXECUTION_ERROR);

    r = KP184_SetInput(on);
    /* Once the request has gone out the load may have switched on, even if
     * its acknowledgement was lost; only an acknowledged OFF clears that. */
    if(on && (r != KP184_ERR_BUSY)) turned_on = 1;
    if(!on && (r == KP184_OK)) turned_on = 0;
    return load_result(context, r);
}

static scpi_result_t cmd_input_q(scpi_t *context)
{
    kp184_status_t st;
    kp184_err_t r = KP184_Status(&st);      /* the output register's reply is unreliable, the status bit is not */

    if(r != KP184_OK) return load_result(context, r);
    SCPI_ResultInt32(context, st.output_on);
    return SCPI_RES_OK;
}

/* --- readings ------------------------------------------------------------------ */

enum { MEAS_VOLT = 0, MEAS_CURR, MEAS_POW, MEAS_RES };

static scpi_result_t cmd_measure_q(scpi_t *context)
{
    kp184_status_t st;
    kp184_err_t r = KP184_Status(&st);      /* voltage and current from one frame, so power and resistance are consistent */

    if(r != KP184_OK) return load_result(context, r);
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

/* --- gateway-specific ---------------------------------------------------------- */

static scpi_result_t cmd_failsafe(scpi_t *context)
{
    scpi_bool_t on;

    if(!SCPI_ParamBool(context, &on, TRUE)) return SCPI_RES_ERR;
    if(!no_more_params(context)) return SCPI_RES_ERR;
    failsafe = on ? 1 : 0;
    return SCPI_RES_OK;
}

static scpi_result_t cmd_failsafe_q(scpi_t *context)
{
    SCPI_ResultInt32(context, failsafe);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_uptime_q(scpi_t *context)
{
    SCPI_ResultUInt32Base(context, Tick_UptimeSeconds(), 10);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_reset_cause_q(scpi_t *context)
{
    SCPI_ResultMnemonic(context, Wdg_ResetCause());
    return SCPI_RES_OK;
}

#ifdef WDG_TEST
/* Test builds only: stop feeding the watchdog. */
static scpi_result_t cmd_hang(scpi_t *context)
{
    (void)context;
    for(;;);
}
#endif

/* --- the command set ----------------------------------------------------------- */

#define LEVEL(name, tag) \
    {"[:SOURce]:" name "[:LEVel][:IMMediate][:AMPLitude]",  cmd_level,   tag}, \
    {"[:SOURce]:" name "[:LEVel][:IMMediate][:AMPLitude]?", cmd_level_q, tag}, \
    {"[:SOURce]:" name ":IRANGe?", cmd_range_q, RANGE_AMPS}, \
    {"[:SOURce]:" name ":VRANGe?", cmd_range_q, RANGE_VOLTS}

#define READING(name, tag) \
    {"MEASure[:SCALar]:" name "[:DC]?", cmd_measure_q, tag}, \
    {"FETCh[:SCALar]:"   name "[:DC]?", cmd_measure_q, tag}

static const scpi_command_t commands[] = {
    {"*CLS",  SCPI_CoreCls,  0},
    {"*ESE",  SCPI_CoreEse,  0},
    {"*ESE?", SCPI_CoreEseQ, 0},
    {"*ESR?", SCPI_CoreEsrQ, 0},
    {"*IDN?", SCPI_CoreIdnQ, 0},
    {"*OPC",  SCPI_CoreOpc,  0},
    {"*OPC?", SCPI_CoreOpcQ, 0},
    {"*RST",  SCPI_CoreRst,  0},
    {"*SRE",  SCPI_CoreSre,  0},
    {"*SRE?", SCPI_CoreSreQ, 0},
    {"*STB?", SCPI_CoreStbQ, 0},
    {"*TST?", cmd_tst_q,     0},
    {"*WAI",  SCPI_CoreWai,  0},

    {"SYSTem:ERRor[:NEXT]?",         SCPI_SystemErrorNextQ,          0},
    {"SYSTem:ERRor:COUNt?",          SCPI_SystemErrorCountQ,         0},
    {"SYSTem:VERSion?",              SCPI_SystemVersionQ,            0},
    {"STATus:QUEStionable[:EVENt]?", SCPI_StatusQuestionableEventQ,  0},
    {"STATus:QUEStionable:ENABle",   SCPI_StatusQuestionableEnable,  0},
    {"STATus:QUEStionable:ENABle?",  SCPI_StatusQuestionableEnableQ, 0},
    {"STATus:PRESet",                SCPI_StatusPreset,              0},

    {"[:SOURce]:FUNCtion",       cmd_function,   0},
    {"[:SOURce]:FUNCtion?",      cmd_function_q, 0},
    {"[:SOURce]:INPut[:STATe]",  cmd_input,      0},
    {"[:SOURce]:INPut[:STATe]?", cmd_input_q,    0},

    LEVEL("CURRent",    LEVEL_CURR),
    LEVEL("VOLTage",    LEVEL_VOLT),
    LEVEL("RESistance", LEVEL_RES),
    LEVEL("POWer",      LEVEL_POW),

    READING("VOLTage",    MEAS_VOLT),
    READING("CURRent",    MEAS_CURR),
    READING("POWer",      MEAS_POW),
    READING("RESistance", MEAS_RES),

    {"SYSTem:FAILsafe[:STATe]",  cmd_failsafe,      0},
    {"SYSTem:FAILsafe[:STATe]?", cmd_failsafe_q,    0},
    {"DIAGnostic:UPTime?",       cmd_uptime_q,      0},
    {"DIAGnostic:RESet:CAUSe?",  cmd_reset_cause_q, 0},
#ifdef WDG_TEST
    {"DIAGnostic:HANG",          cmd_hang,          0},
#endif

    SCPI_CMD_LIST_END
};

static scpi_interface_t interface = {
    .write = parser_write,
    .flush = parser_flush,
    .reset = parser_reset,
};

/* --- the connection ------------------------------------------------------------ */

void Scpi_Init(void)
{
    SCPI_Init(&parser, commands, &interface, scpi_units_def,
              "Kunkin", "KP184", "0", FW_VERSION,
              input_buffer, sizeof(input_buffer),
              error_queue, ERROR_QUEUE_LEN);
}

void Scpi_Connect(uint8_t socketid)
{
    Net_SessionStart(&session, socketid, recv_buffer, sizeof(recv_buffer));
    output_len = 0;
    parser.buffer.position = 0;     /* drop a half-typed line from the previous client */
    line_len = 0;
    discarding = 0;
    closing = 0;
    failsafe = 1;
    turned_on = 0;
    SCPI_ErrorClear(&parser);
    printf("SCPI client connected\r\n");
}

void Scpi_Close(void)
{
    if(!session.active) return;
    Net_SessionDrop(&session);
    Scpi_Disconnect();
}

void Scpi_Check(void)
{
    if(session.active && !Net_SessionOwnsSlot(&session)) end_session();
}

uint8_t Scpi_IsConnected(void)
{
    return session.active;
}

uint8_t Scpi_Has(uint8_t socketid)
{
    return session.active && (session.id == socketid);
}

/* Give the parser the next stretch of a line. False once the client has been
 * dropped, which send_output() does to one that stops taking its replies:
 * nothing more of what it sent is run. */
static scpi_bool_t parse(const uint8_t *data, uint32_t len)
{
    SCPI_Input(&parser, (const char *)data, (int)len);
    return session.active;
}

/* Hand received bytes to the parser. A line too long for the parser's buffer
 * is dropped whole, up to its newline: left to itself the parser would drop
 * only what it holds and then run the rest of the line as a new command. */
static void feed(const uint8_t *data, uint32_t len)
{
    uint32_t i, start = 0;

    for(i = 0; i < len; i++)
    {
        if(discarding)
        {
            if(data[i] == '\n')
            {
                discarding = 0;
                line_len = 0;
            }
            start = i + 1;
            continue;
        }
        line_len++;
        if(data[i] == '\n')
        {
            if(!parse(data + start, i + 1 - start)) return;
            start = i + 1;
            line_len = 0;
        }
        else if(line_len >= LINE_LIMIT)
        {
            parser.buffer.position = 0;
            SCPI_ErrorPush(&parser, SCPI_ERROR_INPUT_BUFFER_OVERRUN);
            discarding = 1;
            start = i + 1;
        }
    }
    if(!discarding && (start < len)) parse(data + start, len - start);
}

/* Read and run the next chunk of what the client sent; returns its size. */
static uint32_t run_chunk(void)
{
    static uint8_t chunk[128];
    uint32_t len;

    Failsafe_Poll();                /* a pending switch-off goes before any later command */
    Scpi_Check();
    if(!session.active) return 0;
    len = Net_SessionRead(&session, chunk, sizeof(chunk));
    if(len) feed(chunk, len);
    return len;
}

/* One chunk per call, so the main loop polls socket events between chunks
 * and a long command stream cannot starve the network. A connection found
 * closed ends once everything it sent has been read; the main loop's event
 * handler may get there first. */
void Scpi_Poll(void)
{
    run_chunk();
    if(session.active && !Net_SessionOpen(&session) && !Net_SessionPending(&session)) end_session();
}

/* Run the complete lines still waiting in the socket, such as a final INP OFF;
 * their replies have nowhere to go. A line cut off by the end of the
 * connection is not run. */
void Scpi_Disconnect(void)
{
    closing = 1;
    while(run_chunk());
    closing = 0;
    end_session();
}
