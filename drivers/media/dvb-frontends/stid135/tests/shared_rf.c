/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(expr) do { \
	if (!(expr)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #expr); \
		exit(1); \
	} \
} while (0)

struct mutex { pthread_mutex_t native; };
static _Thread_local struct mutex *held_lock;
static void lock_contended(struct mutex *lock);
static void mutex_init(struct mutex *lock)
{
	CHECK(pthread_mutex_init(&lock->native, NULL) == 0);
}
static void mutex_lock(struct mutex *lock)
{
	int result;

	CHECK(!held_lock);
	result = pthread_mutex_trylock(&lock->native);
	if (result == EBUSY) {
		lock_contended(lock);
		CHECK(pthread_mutex_lock(&lock->native) == 0);
	} else {
		CHECK(result == 0);
	}
	held_lock = lock;
}
static void mutex_unlock(struct mutex *lock)
{
	CHECK(held_lock == lock);
	held_lock = NULL;
	CHECK(pthread_mutex_unlock(&lock->native) == 0);
}

struct list_head { struct list_head *next, *prev; };
#define LIST_HEAD(name) struct list_head name = { &(name), &(name) }
#define container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))
#define list_for_each_entry(pos, head, member) \
	for (struct list_head *node = (head)->next; \
	     node != (head) && ((pos) = container_of(node, struct stv_base, member), 1); \
	     node = node->next)
static void list_add(struct list_head *node, struct list_head *head)
{
	node->next = head->next;
	node->prev = head;
	head->next->prev = node;
	head->next = node;
}
static void list_del(struct list_head *node)
{
	node->next->prev = node->prev;
	node->prev->next = node->next;
}

struct device { int unused; };
struct i2c_adapter { struct device dev; };
enum fe_sec_voltage { SEC_VOLTAGE_13, SEC_VOLTAGE_18, SEC_VOLTAGE_OFF };
enum fe_sec_tone_mode { SEC_TONE_ON, SEC_TONE_OFF };
struct dvb_frontend_ops { struct { const char *name; } info; };
struct dvb_frontend { struct dvb_frontend_ops ops; void *demodulator_priv; };
static struct dvb_frontend_ops stid135_ops = { { "test" } };
static int mode = 1;
static unsigned int rfsource, ts_nosync, bbframe, timeout = 5;
static LIST_HEAD(stvlist);
#define GFP_KERNEL 0
#define dev_dbg(...) ((void)0)
#define dev_info(...) ((void)0)
#define dev_warn(...) ((void)0)
#define dev_err(...) ((void)0)
#define kzalloc(size, flags) calloc(1, size)
static void kfree(void *ptr);
static void fake_wait(unsigned int milliseconds);

/* The kernel's s64 is long long, including on LP64 userspace hosts. */
#define HOST_PC
#define int64_t long long
#include "stid135_drv.h"
#undef int64_t
#undef calloc
#undef free
#undef WAIT_N_MS
#define WAIT_N_MS(ms) fake_wait(ms)
#include "shared-rf-source.h"

u32 C8CODEW_TOP_CTRL[1];

enum operation { ENABLE, DISEQC, MUX, STANDBY, OPERATIONS };
struct hardware {
	struct fe_stid135_internal_param params;
	STCHIP_Info_t chip;
	struct i2c_adapter i2c;
	struct stv_base *base;
	struct stv *states[8];
	int calls[OPERATIONS][4];
	int fail[OPERATIONS];
	int resets[4], routes[8], waits, terminations, frees;
	bool powered[4], probing, freed_base;
	pthread_mutex_t gate;
	pthread_cond_t changed;
	bool pause_enable, entered_enable, unblock_enable, waiter_started;
};
static struct hardware hw;

static void lock_contended(struct mutex *lock)
{
	CHECK(lock == &hw.base->status_lock);
	CHECK(pthread_mutex_lock(&hw.gate) == 0);
	hw.waiter_started = true;
	CHECK(pthread_cond_broadcast(&hw.changed) == 0);
	CHECK(pthread_mutex_unlock(&hw.gate) == 0);
}

static int rf_index(FE_OXFORD_TunerPath_t tuner)
{
	CHECK(tuner >= AFE_TUNER1 && tuner <= AFE_TUNER4);
	return tuner - 1;
}

static void check_locked(void)
{
	CHECK(hw.probing || held_lock == &hw.base->status_lock);
}

static fe_lla_error_t record(enum operation op, FE_OXFORD_TunerPath_t tuner)
{
	check_locked();
	hw.calls[op][rf_index(tuner)]++;
	if (hw.fail[op]) {
		hw.fail[op]--;
		return FE_LLA_I2C_ERROR;
	}
	return FE_LLA_NO_ERROR;
}

STCHIP_Error_t Oxford_EnableLO(STCHIP_Handle_t chip, FE_OXFORD_TunerPath_t tuner)
{
	CHECK(chip == &hw.chip);
	fe_lla_error_t err = record(ENABLE, tuner);
	if (hw.pause_enable) {
		CHECK(pthread_mutex_lock(&hw.gate) == 0);
		hw.entered_enable = true;
		CHECK(pthread_cond_broadcast(&hw.changed) == 0);
		while (!hw.unblock_enable)
			CHECK(pthread_cond_wait(&hw.changed, &hw.gate) == 0);
		CHECK(pthread_mutex_unlock(&hw.gate) == 0);
	}
	return (STCHIP_Error_t)err;
}

STCHIP_Error_t Oxford_TunerStartUp(STCHIP_Handle_t chip, FE_OXFORD_TunerPath_t tuner)
{
	check_locked();
	CHECK(chip == &hw.chip);
	hw.powered[rf_index(tuner)] = true;
	return CHIPERR_NO_ERROR;
}

STCHIP_Error_t Oxford_AdcStartUp(STCHIP_Handle_t chip, FE_OXFORD_TunerPath_t tuner)
{
	check_locked();
	CHECK(chip == &hw.chip);
	rf_index(tuner);
	return CHIPERR_NO_ERROR;
}

STCHIP_Error_t Oxford_SetVGLNAgainMode(STCHIP_Handle_t chip,
		FE_OXFORD_TunerPath_t tuner, U8 value)
{
	check_locked();
	CHECK(chip == &hw.chip && value == 0);
	rf_index(tuner);
	return CHIPERR_NO_ERROR;
}

STCHIP_Error_t Oxford_TunerDisable(STCHIP_Handle_t chip, FE_OXFORD_TunerPath_t tuner)
{
	CHECK(chip == &hw.chip);
	fe_lla_error_t err = record(STANDBY, tuner);
	/* An I2C failure can occur after some powerdown writes succeeded. */
	hw.powered[rf_index(tuner)] = false;
	return (STCHIP_Error_t)err;
}

STCHIP_Error_t ChipSetField(STCHIP_Handle_t chip, u32 field, s32 value)
{
	check_locked();
	CHECK(chip == &hw.chip);
	CHECK(field == FLD_FC8CODEW_C8CODEW_TOP_CTRL_TOP_STOPCLK_STOP_CKTUNER);
	CHECK(value == 0 || value == 1 || value == 2 || value == 4 || value == 8);
	for (int rf = 0; rf < 4; rf++)
		if (value & (1 << rf))
			hw.resets[rf]++;
	return CHIPERR_NO_ERROR;
}

static void fake_wait(unsigned int milliseconds)
{
	check_locked();
	CHECK(milliseconds == 10 || milliseconds == 100);
	hw.waits += milliseconds;
}

fe_lla_error_t fe_stid135_diseqc_init(fe_stid135_handle_t handle,
		FE_OXFORD_TunerPath_t tuner, enum fe_sat_diseqc_txmode txmode)
{
	CHECK(handle == &hw.params);
	CHECK(txmode == FE_SAT_DISEQC_2_3_PWM || txmode == FE_SAT_22KHZ_Continues);
	return record(DISEQC, tuner);
}

fe_lla_error_t fe_stid135_set_rfmux_path(stchip_handle_t chip,
		enum fe_stid135_demod demod, FE_OXFORD_TunerPath_t tuner)
{
	CHECK(chip == &hw.chip);
	CHECK(demod >= 1 && demod <= 8);
	fe_lla_error_t err = record(MUX, tuner);
	if (!err)
		hw.routes[demod - 1] = tuner;
	return err;
}

fe_lla_error_t fe_stid135_set_22khz_cont(fe_stid135_handle_t handle,
		FE_OXFORD_TunerPath_t tuner, BOOL enable)
{
	check_locked();
	CHECK(handle == &hw.params);
	rf_index(tuner);
	return FE_LLA_NO_ERROR;
}

fe_lla_error_t fe_stid135_init(struct fe_stid135_init_param *params,
		fe_stid135_handle_t *handle)
{
	hw.base = params->pI2CHost;
	hw.probing = true;
	hw.params.handle_demod = &hw.chip;
	*handle = &hw.params;
	return FE_LLA_NO_ERROR;
}

fe_lla_error_t fe_stid135_get_cut_id(fe_stid135_handle_t handle, enum device_cut_id *cut)
{
	*cut = STID135_CUT2_0;
	return FE_LLA_NO_ERROR;
}

fe_lla_error_t fe_stid135_set_ts_parallel_serial(fe_stid135_handle_t handle,
		enum fe_stid135_demod demod, enum fe_ts_output_mode output)
{
	return FE_LLA_NO_ERROR;
}

fe_lla_error_t fe_stid135_enable_stfe(fe_stid135_handle_t handle,
		enum fe_stid135_stfe_output output)
{
	return FE_LLA_NO_ERROR;
}

fe_lla_error_t fe_stid135_set_stfe(fe_stid135_handle_t handle,
		enum fe_stid135_stfe_mode mode, u8 input,
		enum fe_stid135_stfe_output output, u8 tag)
{
	return FE_LLA_NO_ERROR;
}

STCHIP_Error_t stvvglna_init(SAT_VGLNA_Params_t *params, STCHIP_Handle_t *chip)
{
	CHECK(false); /* Fixtures deliberately use boards without VGLNA. */
	return CHIPERR_NO_ERROR;
}
STCHIP_Error_t stvvglna_set_standby(STCHIP_Handle_t chip, U8 standby)
{
	CHECK(false);
	return CHIPERR_NO_ERROR;
}
STCHIP_Error_t stvvglna_term(STCHIP_Handle_t chip)
{
	CHECK(false);
	return CHIPERR_NO_ERROR;
}
fe_lla_error_t FE_STiD135_Term(fe_stid135_handle_t handle)
{
	CHECK(handle == &hw.params);
	hw.terminations++;
	return FE_LLA_NO_ERROR;
}

static void kfree(void *ptr)
{
	CHECK(!held_lock);
	if (ptr == hw.base) {
		CHECK(pthread_mutex_destroy(&hw.base->status_lock.native) == 0);
		hw.freed_base = true;
	}
	for (int i = 0; i < 8; i++)
		if (hw.states[i] == ptr)
			hw.states[i] = NULL;
	hw.frees++;
	free(ptr);
}

static int board_voltage(struct i2c_adapter *i2c, enum fe_sec_voltage voltage, u8 rf)
{
	CHECK(i2c == &hw.i2c && rf < 4);
	return 0;
}

static struct dvb_frontend *attach(int demod, int rf, bool board)
{
	struct stid135_cfg cfg = { .adr = 0x68, .ts_mode = TS_8SER,
		.set_voltage = board ? board_voltage : NULL, .control_22k = true };
	struct dvb_frontend *fe = stid135_attach(&hw.i2c, &cfg, demod, rf);
	CHECK(fe);
	CHECK(!hw.states[demod]);
	hw.states[demod] = fe->demodulator_priv;
	hw.probing = false;
	CHECK(!held_lock);
	return fe;
}

static void first_use(void)
{
	struct dvb_frontend *a = attach(0, 0, false);
	attach(1, 0, false); /* Attached frontends are not active RF users. */
	CHECK(stid135_init(a) == 0);
	CHECK(hw.resets[0] == 1 && hw.waits == 110);
	CHECK(hw.calls[DISEQC][0] == 1 && hw.routes[0] == 1 && hw.powered[0]);
}

static void duplicate_init(void)
{
	struct dvb_frontend *a = attach(0, 0, false);
	CHECK(stid135_init(a) == 0);
	CHECK(stid135_init(a) == 0); /* Also models close/reopen with no sleep callback. */
	CHECK(hw.resets[0] == 1 && hw.calls[DISEQC][0] == 1);
	CHECK(stid135_sleep(a) == 0);
	CHECK(hw.calls[STANDBY][0] == 1 && !hw.powered[0]);
	CHECK(stid135_init(a) == 0);
	CHECK(hw.resets[0] == 2);
}

static void siblings(void)
{
	struct dvb_frontend *fe[8];
	rfsource = 1;
	for (int i = 0; i < 8; i++) {
		fe[i] = attach(i, i % 4, true);
		CHECK(stid135_init(fe[i]) == 0);
		CHECK(hw.routes[i] == 1);
	}
	CHECK(hw.resets[0] == 1 && hw.calls[DISEQC][0] == 1 && hw.waits == 110);
}

static void independent_rf(void)
{
	struct dvb_frontend *fe[4];
	for (int i = 0; i < 4; i++) {
		fe[i] = attach(i, i, false);
		CHECK(stid135_init(fe[i]) == 0);
		CHECK(hw.resets[i] == 1 && hw.routes[i] == i + 1);
	}
	CHECK(stid135_sleep(fe[2]) == 0);
	CHECK(!hw.powered[2] && hw.powered[0] && hw.powered[1] && hw.powered[3]);
	CHECK(stid135_init(fe[2]) == 0);
	CHECK(hw.resets[2] == 2 && hw.resets[0] == 1);
}

static void shared_sleep(void)
{
	struct dvb_frontend *a = attach(0, 0, false), *b = attach(1, 0, false);
	CHECK(stid135_init(a) == 0 && stid135_init(b) == 0);
	CHECK(stid135_sleep(a) == 0);
	CHECK(hw.calls[STANDBY][0] == 0 && hw.powered[0]);
	CHECK(stid135_sleep(a) == 0);
	CHECK(hw.calls[STANDBY][0] == 0 && hw.powered[0]);
	CHECK(stid135_sleep(b) == 0);
	CHECK(hw.calls[STANDBY][0] == 1 && !hw.powered[0]);
	CHECK(stid135_sleep(b) == 0);
	CHECK(hw.calls[STANDBY][0] == 1);
}

static void inactive_sleep(void)
{
	struct dvb_frontend *a = attach(0, 0, false), *b = attach(1, 0, false);
	CHECK(stid135_sleep(a) == 0);
	CHECK(hw.calls[STANDBY][0] == 0);
	CHECK(stid135_init(b) == 0 && stid135_sleep(a) == 0);
	CHECK(hw.calls[STANDBY][0] == 0 && hw.powered[0]);
}

static void board_sleep(void)
{
	struct dvb_frontend *a = attach(0, 0, true), *b = attach(1, 0, true);
	CHECK(stid135_init(a) == 0 && stid135_init(b) == 0);
	CHECK(stid135_sleep(a) == 0 && stid135_sleep(b) == 0);
	CHECK(hw.calls[STANDBY][0] == 0 && hw.powered[0]);
	CHECK(stid135_init(a) == 0 && stid135_init(b) == 0);
	CHECK(hw.resets[0] == 1 && hw.calls[DISEQC][0] == 1);
}

static void failed_init(enum operation op, bool cleanup_failure)
{
	struct dvb_frontend *a = attach(0, 0, false);
	hw.fail[op] = 1;
	hw.fail[STANDBY] = cleanup_failure;
	CHECK(stid135_init(a) != 0);
	CHECK(!held_lock);
	CHECK(hw.calls[STANDBY][0] == 1 && !hw.powered[0]);
	CHECK(hw.calls[DISEQC][0] == (op != ENABLE));
	CHECK(hw.calls[MUX][0] == (op == MUX));
	CHECK(stid135_sleep(a) == 0 && hw.calls[STANDBY][0] == 1);
	CHECK(stid135_init(a) == 0 && hw.powered[0]);
	CHECK(hw.resets[0] == 2);
	CHECK(stid135_sleep(a) == 0 && hw.calls[STANDBY][0] == 2);
}
static void failed_enable(void) { failed_init(ENABLE, false); }
static void failed_diseqc(void) { failed_init(DISEQC, false); }
static void failed_mux(void) { failed_init(MUX, false); }
static void failed_cleanup(void) { failed_init(DISEQC, true); }

static void sibling_failure(void)
{
	struct dvb_frontend *a = attach(0, 0, false), *b = attach(1, 0, false);
	CHECK(stid135_init(a) == 0);
	hw.fail[MUX] = 1;
	CHECK(stid135_init(b) != 0);
	CHECK(hw.resets[0] == 1 && hw.calls[STANDBY][0] == 0 && hw.powered[0]);
	CHECK(stid135_sleep(b) == 0 && hw.calls[STANDBY][0] == 0);
	CHECK(stid135_init(b) == 0 && hw.resets[0] == 1);
	CHECK(stid135_sleep(a) == 0 && hw.calls[STANDBY][0] == 0);
	CHECK(stid135_sleep(b) == 0 && hw.calls[STANDBY][0] == 1);
}

static void duplicate_failure(void)
{
	struct dvb_frontend *a = attach(0, 0, false);
	CHECK(stid135_init(a) == 0);
	hw.fail[MUX] = 1;
	CHECK(stid135_init(a) != 0);
	CHECK(hw.resets[0] == 1 && hw.calls[STANDBY][0] == 0 && hw.powered[0]);
	CHECK(stid135_init(a) == 0);
	CHECK(stid135_sleep(a) == 0 && hw.calls[STANDBY][0] == 1);
}

static void failed_standby(void)
{
	struct dvb_frontend *a = attach(0, 0, false), *b = attach(1, 0, false);
	CHECK(stid135_init(a) == 0);
	hw.fail[STANDBY] = 1;
	CHECK(stid135_sleep(a) != 0 && !held_lock);
	CHECK(stid135_init(b) == 0 && hw.resets[0] == 2 && hw.powered[0]);
	CHECK(stid135_sleep(a) == 0 && hw.calls[STANDBY][0] == 1);
	CHECK(stid135_sleep(b) == 0 && hw.calls[STANDBY][0] == 2);
}

static void release_active(void)
{
	struct dvb_frontend *a = attach(0, 0, false), *b = attach(1, 0, false);
	CHECK(stid135_init(a) == 0 && stid135_init(b) == 0);
	stid135_release(a);
	CHECK(hw.calls[STANDBY][0] == 0 && hw.powered[0] && hw.terminations == 0);
	CHECK(stid135_sleep(b) == 0 && hw.calls[STANDBY][0] == 1);
	stid135_release(b);
	CHECK(hw.calls[STANDBY][0] == 1 && hw.terminations == 1);
	CHECK(hw.freed_base && hw.frees == 3 && stvlist.next == &stvlist);
}

static void release_last_active(void)
{
	struct dvb_frontend *a = attach(0, 0, false), *b = attach(1, 0, false);
	CHECK(stid135_init(a) == 0);
	stid135_release(a); /* Detach without any prior sleep callback. */
	CHECK(hw.calls[STANDBY][0] == 1 && !hw.powered[0]);
	CHECK(stid135_init(b) == 0 && hw.resets[0] == 2);
	stid135_release(b);
	CHECK(hw.calls[STANDBY][0] == 2 && hw.terminations == 1 && hw.freed_base);
}

static void release_inactive(void)
{
	struct dvb_frontend *a = attach(0, 0, false), *b = attach(1, 0, false);
	CHECK(stid135_init(b) == 0);
	stid135_release(a);
	CHECK(hw.powered[0] && hw.calls[STANDBY][0] == 0);
	CHECK(stid135_sleep(b) == 0 && hw.calls[STANDBY][0] == 1);
	stid135_release(b);
}

static void release_board(void)
{
	struct dvb_frontend *a = attach(0, 0, true), *b = attach(1, 0, true);
	CHECK(stid135_init(a) == 0);
	stid135_release(a);
	CHECK(hw.powered[0] && hw.calls[STANDBY][0] == 0);
	CHECK(stid135_init(b) == 0 && hw.resets[0] == 1);
	stid135_release(b);
	CHECK(hw.calls[STANDBY][0] == 0 && hw.freed_base);
}

static void release_failed_standby(void)
{
	struct dvb_frontend *a = attach(0, 0, false), *b = attach(1, 0, false);
	CHECK(stid135_init(a) == 0);
	hw.fail[STANDBY] = 1;
	stid135_release(a);
	CHECK(hw.calls[STANDBY][0] == 1);
	CHECK(stid135_init(b) == 0 && hw.resets[0] == 2 && hw.powered[0]);
	CHECK(stid135_sleep(b) == 0 && hw.calls[STANDBY][0] == 2);
	stid135_release(b);
}

static void multiswitch(void)
{
	mode = 0;
	struct dvb_frontend *a = attach(0, 0, true);
	for (int i = 0; i < 4; i++)
		CHECK(hw.resets[i] == 1 && hw.calls[DISEQC][i] == 1);
	CHECK(stid135_init(a) == 0 && stid135_init(a) == 0);
	CHECK(stid135_set_voltage(a, SEC_VOLTAGE_18) == 0);
	CHECK(stid135_set_tone(a, SEC_TONE_ON) == 0);
	CHECK(((struct stv *)a->demodulator_priv)->rf_in == 3);
	CHECK(stid135_set_voltage(a, SEC_VOLTAGE_13) == 0);
	CHECK(stid135_set_tone(a, SEC_TONE_OFF) == 0);
	CHECK(((struct stv *)a->demodulator_priv)->rf_in == 0);
	CHECK(stid135_sleep(a) == 0);
	stid135_release(a);
	for (int i = 0; i < 4; i++)
		CHECK(hw.resets[i] == 1 && hw.calls[STANDBY][i] == 0);
}

static void forced_rf(void)
{
	mode = 2;
	rfsource = 1;
	struct dvb_frontend *a = attach(0, 0, true), *b = attach(1, 1, true);
	CHECK(stid135_init(a) == 0 && stid135_init(b) == 0);
	CHECK(hw.resets[3] == 1 && hw.routes[0] == 4 && hw.routes[1] == 4);
	CHECK(hw.resets[0] == 0);
}

struct worker { struct dvb_frontend *fe; bool sleep; int result; };
static void *run_callback(void *arg)
{
	struct worker *worker = arg;
	worker->result = worker->sleep ? stid135_sleep(worker->fe) : stid135_init(worker->fe);
	CHECK(!held_lock);
	return NULL;
}

static void concurrent(bool same_frontend, bool sleep_waiter)
{
	struct dvb_frontend *a = attach(0, 0, false), *b = attach(1, 0, false);
	struct worker first = { .fe = a }, second = {
		.fe = same_frontend ? a : b, .sleep = sleep_waiter };
	pthread_t t1, t2;
	hw.pause_enable = true;
	CHECK(pthread_create(&t1, NULL, run_callback, &first) == 0);
	CHECK(pthread_mutex_lock(&hw.gate) == 0);
	while (!hw.entered_enable)
		CHECK(pthread_cond_wait(&hw.changed, &hw.gate) == 0);
	/* The first callback is held inside the real enable's I/O stub. */
	CHECK(pthread_mutex_trylock(&hw.base->status_lock.native) == EBUSY);
	CHECK(pthread_create(&t2, NULL, run_callback, &second) == 0);
	while (!hw.waiter_started)
		CHECK(pthread_cond_wait(&hw.changed, &hw.gate) == 0);
	hw.unblock_enable = true;
	CHECK(pthread_cond_broadcast(&hw.changed) == 0);
	CHECK(pthread_mutex_unlock(&hw.gate) == 0);
	CHECK(pthread_join(t1, NULL) == 0 && pthread_join(t2, NULL) == 0);
	CHECK(first.result == 0 && second.result == 0);
	CHECK(hw.resets[0] == 1 && hw.calls[DISEQC][0] == 1);
	CHECK(stid135_sleep(a) == 0);
	if (!same_frontend && !sleep_waiter) {
		CHECK(hw.calls[STANDBY][0] == 0 && hw.powered[0]);
		CHECK(stid135_sleep(b) == 0);
	}
	CHECK(hw.calls[STANDBY][0] == 1 && !hw.powered[0]);
}
static void concurrent_siblings(void) { concurrent(false, false); }
static void concurrent_duplicate(void) { concurrent(true, false); }
static void concurrent_init_sleep(void) { concurrent(true, true); }

static const struct test { const char *name; void (*run)(void); } tests[] = {
#define TEST(name) { #name, name }
	TEST(first_use), TEST(duplicate_init), TEST(siblings), TEST(independent_rf),
	TEST(shared_sleep), TEST(inactive_sleep), TEST(board_sleep),
	TEST(failed_enable), TEST(failed_diseqc), TEST(failed_mux), TEST(failed_cleanup),
	TEST(sibling_failure), TEST(duplicate_failure), TEST(failed_standby),
	TEST(release_active), TEST(release_last_active), TEST(release_inactive),
	TEST(release_board), TEST(release_failed_standby), TEST(multiswitch), TEST(forced_rf),
	TEST(concurrent_siblings), TEST(concurrent_duplicate), TEST(concurrent_init_sleep),
};

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	CHECK(pthread_mutex_init(&hw.gate, NULL) == 0);
	CHECK(pthread_cond_init(&hw.changed, NULL) == 0);
	for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
		if (!strcmp(argv[1], "--list"))
			puts(tests[i].name);
		else if (!strcmp(argv[1], tests[i].name)) {
			tests[i].run();
			CHECK(!held_lock);
			for (int demod = 0; demod < 8; demod++)
				if (hw.states[demod])
					stid135_release(&hw.states[demod]->fe);
			CHECK(stvlist.next == &stvlist);
			CHECK(pthread_cond_destroy(&hw.changed) == 0);
			CHECK(pthread_mutex_destroy(&hw.gate) == 0);
			printf("PASS %s\n", tests[i].name);
			return 0;
		}
	}
	return strcmp(argv[1], "--list") != 0;
}
