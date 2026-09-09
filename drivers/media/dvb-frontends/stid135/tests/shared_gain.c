/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
 fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #expr); exit(1); \
} } while (0)

struct mutex { bool held; };
static void mutex_lock(struct mutex *m) { CHECK(!m->held); m->held = true; }
static void mutex_unlock(struct mutex *m) { CHECK(m->held); m->held = false; }
#define HOST_PC
#define int64_t long long
#define uint64_t unsigned long long
#include "stid135_drv.h"
#include "stid135_addr_map.h"
#include "c8codew_addr_map.h"
#include "oxford_anafe_init.h"
#undef int64_t
#undef uint64_t
#undef calloc
#undef free
#undef WAIT_N_MS
#define WAIT_N_MS(ms) ((void)(ms))

static struct fe_stid135_internal_param params;
static STCHIP_Info_t chip;
static struct mutex master;
static u8 regs[65536], shadow[65536];
static int gain_writes, fail_read = -1, fail_write = -1;
static bool started[8], never_lock, matype_timeout, interleave, did_interleave;
static fe_lla_error_t setup_error, tracking_error;
static int inner_demod = 2, nesting;
static int interleaved_writes;
static fe_lla_error_t inner_result;
static fe_lla_error_t tune(int demod);

#include "shared-gain-source.h"

static void put_field(u32 field, u32 value)
{
    unsigned int mask = field & 255, shift = 0;
    while (!((mask >> shift) & 1)) shift++;
    regs[field >> 16] = (regs[field >> 16] & ~mask) | ((value << shift) & mask);
}

static u32 route_field(int demod)
{
    u32 fields[] = {
        FLD_FC8CODEW_C8CODEW_RFMUX_RFMUX0_RFMUX_DEMOD_SEL_1,
        FLD_FC8CODEW_C8CODEW_RFMUX_RFMUX1_RFMUX_DEMOD_SEL_2,
        FLD_FC8CODEW_C8CODEW_RFMUX_RFMUX2_RFMUX_DEMOD_SEL_3,
        FLD_FC8CODEW_C8CODEW_RFMUX_RFMUX3_RFMUX_DEMOD_SEL_4,
        FLD_FC8CODEW_C8CODEW_RFMUX_RFMUX4_RFMUX_DEMOD_SEL_5,
        FLD_FC8CODEW_C8CODEW_RFMUX_RFMUX5_RFMUX_DEMOD_SEL_6,
        FLD_FC8CODEW_C8CODEW_RFMUX_RFMUX6_RFMUX_DEMOD_SEL_7,
        FLD_FC8CODEW_C8CODEW_RFMUX_RFMUX7_RFMUX_DEMOD_SEL_8,
    };
    return fields[demod - 1];
}

static void reception(int demod, int state, int lock)
{
    put_field(FLD_FC8CODEW_DVBSX_DEMOD_DMDSTATE_HEADER_MODE(demod), state);
    put_field(FLD_FC8CODEW_DVBSX_DEMOD_DSTATUS_LOCK_DEFINITIF(demod), lock);
}

STCHIP_Error_t ChipGetOneRegister(STCHIP_Handle_t h, u16 reg, u32 *out)
{
    CHECK(master.held);
    h->Error = CHIPERR_NO_ERROR;
    if (reg == fail_read) { *out = 0; return h->Error = CHIPERR_I2C_NO_ACK; }
    *out = regs[reg];
    shadow[reg] = regs[reg];
    return CHIPERR_NO_ERROR;
}

STCHIP_Error_t ChipSetOneRegister(STCHIP_Handle_t h, u16 reg, u32 value)
{
    CHECK(master.held);
    h->Error = CHIPERR_NO_ERROR;
    if (reg == RAFE_RF_CFG1) gain_writes++;
    if (reg == fail_write) return h->Error = CHIPERR_I2C_NO_ACK;
    regs[reg] = shadow[reg] = value;
    for (int d = 1; d <= 8; d++)
        if (reg == (u16)REG_RC8CODEW_DVBSX_DEMOD_DMDISTATE(d))
            reception(d, FE_SAT_SEARCH, 0);
    return CHIPERR_NO_ERROR;
}

STCHIP_Error_t ChipGetRegisters(STCHIP_Handle_t h, u16 reg, s32 count)
{
    for (int i = 0; i < count; i++) {
        u32 value;
        STCHIP_Error_t error = ChipGetOneRegister(h, reg + i, &value);
        if (error) return error;
    }
    return CHIPERR_NO_ERROR;
}

STCHIP_Error_t ChipSetRegisters(STCHIP_Handle_t h, u16 reg, int count)
{
    for (int i = 0; i < count; i++) {
        STCHIP_Error_t error = ChipSetOneRegister(h, reg + i, shadow[reg + i]);
        if (error) return error;
    }
    return CHIPERR_NO_ERROR;
}

s32 ChipGetFieldImage(STCHIP_Handle_t h, u32 field)
{
    return (shadow[field >> 16] & ChipGetFieldMask(field)) >> ChipGetFieldPosition(field & 255);
}

STCHIP_Error_t ChipSetFieldImage(STCHIP_Handle_t h, u32 field, s32 value)
{
    unsigned int mask = field & 255, shift = ChipGetFieldPosition(mask);
    shadow[field >> 16] = (shadow[field >> 16] & ~mask) | ((value << shift) & mask);
    return h->Error;
}

void ChipWaitOrAbort(STCHIP_Handle_t h, u32 delay)
{
    if (master.held) return;
    if (interleave && !did_interleave) {
        did_interleave = true;
        regs[(u16)REG_RC8CODEW_DVBSX_AGCRF_AGCRFIN1(1)] = 1;
        nesting++;
        inner_result = tune(inner_demod);
        nesting--;
        CHECK(gain_writes == interleaved_writes);
#ifdef GAIN_POLICY_PRESENT
        CHECK(params.acquiring_demods == 1);
#endif
    }
    if (!never_lock && !nesting)
        for (int d = 1; d <= 8; d++)
            if (started[d-1]) reception(d, FE_SAT_DVBS2_FOUND, 1);
}

static void reset(void)
{
    memset(&params, 0, sizeof(params)); memset(&chip, 0, sizeof(chip));
    memset(regs, 0, sizeof(regs)); memset(shadow, 0, sizeof(shadow));
    memset(started, 0, sizeof(started));
    master.held = false; params.master_lock = &master; params.handle_demod = &chip;
    params.master_clock = 135000000;
    gain_writes = 0; fail_read = fail_write = -1;
    setup_error = tracking_error = FE_LLA_NO_ERROR;
    never_lock = matype_timeout = interleave = did_interleave = false;
    inner_demod = 2; inner_result = FE_LLA_NO_ERROR; nesting = 0; interleaved_writes = 0;
    for (int d = 1; d <= 8; d++) {
        put_field(route_field(d), 0);
        put_field(FLD_FC8CODEW_DVBSX_PKTDELIN_PDELSTATUS1_FIRST_LOCK(d), 1);
    }
    for (int rf = 1; rf <= 4; rf++)
        regs[(u16)REG_RC8CODEW_DVBSX_AGCRF_AGCRFIN1(rf)] = 1;
}

static fe_lla_error_t tune(int demod)
{
    struct fe_sat_search_params search = {0};
    struct fe_sat_search_result result = {0};
    search.symbol_rate = 27500000; search.search_range = 10000000;
    search.search_algo = FE_SAT_COLD_START; search.frequency = 1260000000;
    search.standard = FE_SAT_SEARCH_DVBS2;
    if (matype_timeout)
        put_field(FLD_FC8CODEW_DVBSX_PKTDELIN_PDELSTATUS1_FIRST_LOCK(demod), 0);
    mutex_lock(&master);
    fe_lla_error_t error = fe_stid135_search(&params, demod, &search, &result, false);
    CHECK(master.held);
    mutex_unlock(&master);
    return error;
}

static void shared_reception(void)
{
    for (int rf = 1; rf <= 4; rf++) for (int target = 1; target <= 8; target++)
    for (int d = 1; d <= 8; d++) for (int mode = 0; mode < 2; mode++)
    for (int state = 0; state < 3; state++) {
        if (d == target) continue;
        reset(); put_field(route_field(target), rf - 1); put_field(route_field(d), rf - 1);
        regs[RAFE_RF_CFG1] = mode << (rf-1);
        regs[(u16)REG_RC8CODEW_DVBSX_AGCRF_AGCRFIN1(rf)] = mode ? 0x81 : 0x01;
        reception(d, state == 0 ? FE_SAT_DVBS2_FOUND :
                     state == 1 ? FE_SAT_DVBS_FOUND : FE_SAT_SEARCH, state != 1);
        CHECK(tune(target) == FE_LLA_NO_ERROR);
        CHECK(gain_writes == 0);
        CHECK(regs[RAFE_RF_CFG1] == (mode << (rf-1)));
    }
}

static void exclusive_thresholds(void)
{
    const u32 agc[] = {0, 0x3bff, 0x3c00, 0x5000, 0x8000, 0x8001};
    for (int rf = 1; rf <= 4; rf++) for (int mode = 0; mode < 2; mode++)
    for (unsigned int a = 0; a < sizeof(agc)/sizeof(*agc); a++) {
        reset(); put_field(route_field(1), rf - 1);
        u8 original = (0xa5 & ~(1 << (rf-1))) | (mode << (rf-1));
        regs[RAFE_RF_CFG1] = original;
        regs[(u16)REG_RC8CODEW_DVBSX_AGCRF_AGCRFIN1(rf)] = agc[a] >> 8;
        regs[(u16)REG_RC8CODEW_DVBSX_AGCRF_AGCRFIN0(rf)] = agc[a];
        CHECK(tune(1) == FE_LLA_NO_ERROR);
        int change = mode ? agc[a] > 0x8000 : agc[a] < 0x3c00;
        CHECK(gain_writes == change);
        CHECK(regs[RAFE_RF_CFG1] == (original ^ (change << (rf-1))));
    }
}

static void other_rf(void)
{
    reset(); put_field(route_field(2), 1); reception(2, FE_SAT_DVBS2_FOUND, 1);
    CHECK(tune(1) == FE_LLA_NO_ERROR); CHECK(gain_writes == 1);
}

static void stale_cache(void)
{
    reset(); params.demod_results[1].locked = true;
    CHECK(tune(1) == FE_LLA_NO_ERROR); CHECK(gain_writes == 1);
}

static void recovering_reception(void)
{
    reset(); reception(2, FE_SAT_DVBS_FOUND, 0);
    CHECK(tune(1) == FE_LLA_NO_ERROR); CHECK(gain_writes == 0);
}

static void overlapping_acquisition(void)
{
    reset(); interleave = true;
    regs[(u16)REG_RC8CODEW_DVBSX_AGCRF_AGCRFIN1(1)] = 0x50;
    CHECK(tune(1) == FE_LLA_NO_ERROR); CHECK(did_interleave); CHECK(gain_writes == 0);
    for (int d = 1; d <= 8; d++) reception(d, FE_SAT_SEARCH, 0);
    memset(started, 0, sizeof(started));
    CHECK(tune(3) == FE_LLA_NO_ERROR); CHECK(gain_writes == 1);
}

static void read_errors(void)
{
    u16 failures[] = {RAFE_RF_CFG1, route_field(1) >> 16, route_field(8) >> 16,
        REG_RC8CODEW_DVBSX_AGCRF_AGCRFIN1(1),
        FLD_FC8CODEW_DVBSX_DEMOD_DSTATUS_LOCK_DEFINITIF(2) >> 16,
        FLD_FC8CODEW_DVBSX_DEMOD_DMDSTATE_HEADER_MODE(2) >> 16};
    for (unsigned int i = 0; i < sizeof(failures)/sizeof(*failures); i++) {
        reset(); fail_read = failures[i];
        CHECK(tune(1) != FE_LLA_NO_ERROR); CHECK(gain_writes == 0);
    }
}

static void invalid_rf(void)
{
#ifdef GAIN_POLICY_PRESENT
    reset(); mutex_lock(&master);
    CHECK(fe_stid135_manage_shared_gain(&params, FE_SAT_DEMOD_1, 0, 0x100) == FE_LLA_BAD_PARAMETER);
    CHECK(fe_stid135_manage_shared_gain(&params, FE_SAT_DEMOD_1, 5, 0x100) == FE_LLA_BAD_PARAMETER);
    CHECK(gain_writes == 0); mutex_unlock(&master);
#else
    CHECK(!"checked RF policy absent");
#endif
}

static void acquisition_other_rf(void)
{
    reset(); interleave = true; interleaved_writes = 1;
    put_field(route_field(2), 1);
    regs[(u16)REG_RC8CODEW_DVBSX_AGCRF_AGCRFIN1(1)] = 0x50;
    CHECK(tune(1) == FE_LLA_NO_ERROR); CHECK(did_interleave);
    CHECK(gain_writes == 1); CHECK(regs[RAFE_RF_CFG1] == 2);
}

static void duplicate_acquisition(void)
{
    reset(); interleave = true; inner_demod = 1;
    regs[(u16)REG_RC8CODEW_DVBSX_AGCRF_AGCRFIN1(1)] = 0x50;
    CHECK(tune(1) == FE_LLA_NO_ERROR); CHECK(did_interleave);
    CHECK(inner_result == FE_LLA_BAD_PARAMETER); CHECK(gain_writes == 0);
}

static void abort_search(void)
{
    reset(); chip.Abort = true;
    CHECK(tune(1) != FE_LLA_NO_ERROR); CHECK(gain_writes == 0);
#ifdef GAIN_POLICY_PRESENT
    CHECK(params.acquiring_demods == 0);
#endif
}

static void diagnostics(void)
{
#ifdef GAIN_POLICY_PRESENT
    reset(); reception(2, FE_SAT_DVBS2_FOUND, 1); reception(8, FE_SAT_DVBS_FOUND, 0);
    CHECK(tune(1) == FE_LLA_NO_ERROR);
    CHECK(params.gain_state[0].action == FE_GAIN_DEFERRED);
    CHECK(params.gain_state[0].protected_mask == 0x82);
    CHECK(params.gain_state[0].old_mode == 0);
    CHECK(params.gain_state[0].requested_mode == 1);
    CHECK(params.gain_state[0].agc == 0x100);
    CHECK(params.gain_state[0].rf == 1);
    reset(); CHECK(tune(1) == FE_LLA_NO_ERROR);
    CHECK(params.gain_state[0].action == FE_GAIN_CHANGED);
    reset(); fail_write = RAFE_RF_CFG1; CHECK(tune(1) != FE_LLA_NO_ERROR);
    CHECK(params.gain_state[0].action == FE_GAIN_ERROR);
#else
    CHECK(!"gain diagnostics absent");
#endif
}

static void late_setup_errors(void)
{
    for (int demod = 2; demod <= 4; demod += 2) {
        reset(); fail_write = REG_RC8CODEW_DVBSX_DEMOD_HDEBITCFG2(demod);
        CHECK(tune(demod) != FE_LLA_NO_ERROR); CHECK(gain_writes == 0);
#ifdef GAIN_POLICY_PRESENT
        CHECK(params.acquiring_demods == 0);
#endif
    }
}

static void rejected_search_diagnostics(void)
{
#ifdef GAIN_POLICY_PRESENT
    reset(); CHECK(tune(1) == FE_LLA_NO_ERROR);
    CHECK(params.gain_state[0].action == FE_GAIN_CHANGED);
    struct fe_sat_search_params search = {0};
    struct fe_sat_search_result result = {0};
    mutex_lock(&master);
    CHECK(fe_stid135_search(&params, FE_SAT_DEMOD_1, &search, &result, false) == FE_LLA_BAD_PARAMETER);
    CHECK(params.gain_state[0].action == FE_GAIN_UNCHECKED);
    CHECK(params.gain_state[0].old_mode == -1); CHECK(params.gain_state[0].rf == 0);
    mutex_unlock(&master); CHECK(gain_writes == 1);
#else
    CHECK(!"gain diagnostics absent");
#endif
}

static void failed_acquisition_cleanup(void)
{
    for (int fail = 0; fail < 5; fail++) {
        reset();
        if (fail == 0) setup_error = FE_LLA_I2C_ERROR;
        if (fail == 1) never_lock = true;
        if (fail == 2) matype_timeout = true;
        if (fail == 3) fail_write = RAFE_RF_CFG1;
        if (fail == 4) tracking_error = FE_LLA_I2C_ERROR;
        fe_lla_error_t error = tune(1);
        if (fail != 1) CHECK(error != FE_LLA_NO_ERROR);
        CHECK(!master.held);
#ifdef GAIN_POLICY_PRESENT
        CHECK(params.acquiring_demods == 0);
#endif
    }
}

struct test { const char *name; void (*run)(void); };
static const struct test tests[] = {
    {"shared_reception", shared_reception}, {"exclusive_thresholds", exclusive_thresholds},
    {"other_rf", other_rf}, {"stale_cache", stale_cache},
    {"recovering_reception", recovering_reception},
    {"overlapping_acquisition", overlapping_acquisition},
    {"read_errors", read_errors}, {"failed_acquisition_cleanup", failed_acquisition_cleanup},
    {"invalid_rf", invalid_rf}, {"acquisition_other_rf", acquisition_other_rf},
    {"duplicate_acquisition", duplicate_acquisition}, {"abort_search", abort_search},
    {"diagnostics", diagnostics},
    {"late_setup_errors", late_setup_errors},
    {"rejected_search_diagnostics", rejected_search_diagnostics},
};
int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    for (unsigned int i = 0; i < sizeof(tests)/sizeof(*tests); i++) {
        if (!strcmp(argv[1], "--list")) puts(tests[i].name);
        else if (!strcmp(argv[1], tests[i].name)) {
            tests[i].run(); printf("PASS %s\n", tests[i].name); return 0;
        }
    }
    return strcmp(argv[1], "--list") ? 2 : 0;
}
