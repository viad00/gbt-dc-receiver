/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : gbt_27930_bms.c
  * @brief          : GB/T 27930 BMS state machine and CAN helpers
  * LICENSE: MIT
  * Authors: Vladislav Utkin
  ******************************************************************************
  */
/* USER CODE END Header */

#include "gbt_27930_bms.h"

#include "can.h"
#include "helpers.h"

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

static void handle_rx_message(uint32_t id, uint8_t *buf, uint8_t len);
static int tp_send_rts_cts(uint8_t prio, uint32_t pgn, uint8_t dest, const uint8_t *payload, uint16_t len);

static void put_u16le(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v & 0xFFu); p[1] = (uint8_t)(v >> 8); }
static void put_u24le(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v & 0xFFu); p[1] = (uint8_t)((v >> 8) & 0xFFu); p[2] = (uint8_t)((v >> 16) & 0xFFu); }

#define SA_BMS      0xF4u
#define SA_CHARGER  0x56u

#define PERIOD_BCL_MS   50u
#define PERIOD_BCS_MS   250u
#define PERIOD_BSM_MS   250u
#define PERIOD_BCP_MS   500u
#define PERIOD_BRM_MS   250u
#define PERIOD_BRO_MS   250u
#define PERIOD_BHM_MS   250u
#define PERIOD_BST_MS   250u

#define TP_FRAME_GAP_MS 1u
#define TP_CTS_TIMEOUT_MS   25u
#define TP_EOM_TIMEOUT_MS   25u
#define TP_OVERALL_TIMEOUT_MS 200u

#define PGN_CHM   0x002600u
#define PGN_BHM   0x002700u
#define PGN_CRM   0x000100u
#define PGN_BRM   0x000200u
#define PGN_BCP   0x000600u
#define PGN_CML   0x000800u
#define PGN_BRO   0x000900u
#define PGN_CRO   0x000A00u
#define PGN_BCL   0x001000u
#define PGN_BCS   0x001100u
#define PGN_CCS   0x001200u
#define PGN_BSM   0x001300u
#define PGN_BST   0x001900u
#define PGN_CST   0x001A00u

#define PGN_TP_CM 0x00EC00u
#define PGN_TP_DT 0x00EB00u

#define TP_CM_RTS   0x10u
#define TP_CM_CTS   0x11u
#define TP_CM_EOM_ACK 0x13u
#define TP_CM_ABORT 0xFFu

static inline uint16_t encode_voltage_dV(uint16_t v_dV) { return v_dV; }
static inline uint16_t encode_current_off400(int16_t i_dA)
{
	int32_t tmp = (int32_t)i_dA + 4000;
	if (tmp < 0) tmp = 0;
	if (tmp > 0xFFFF) tmp = 0xFFFF;
	return (uint16_t)tmp;
}
static inline uint16_t encode_cell_0p01V(uint16_t mV)
{
	uint16_t v01 = (uint16_t)((mV + 5u) / 10u);
	if (v01 > 0x0FFFu) v01 = 0x0FFFu;
	return v01;
}

typedef struct {
	char manufacturer[4];
	char pack_code[4];
	uint16_t prod_year;
	uint8_t  prod_month;
	uint8_t  prod_day;
	char plant[3];
	uint8_t ownership;
	char VIN[17];
	char sw_ver[8];
	uint8_t batt_type;
	uint16_t rated_Ah_0p1;
	uint16_t rated_V_dV;
} BattInfo;

// Placeholder info, does not need to be accurate for testing
static BattInfo g_batt = {
	.manufacturer = {'L','A','D','A'},
	.pack_code = {'P','K','0','1'},
	.prod_year = 2024,
	.prod_month = 8,
	.prod_day = 28,
	.plant = {'V','A','Z'},
	.ownership = 1,
	.VIN = {'X','T','A','G','F','L','2','1','0','R','Y','6','7','8','9','0','1'},
	.sw_ver = {'V','1','.','0','.','1','\0','\0'},
	.batt_type = 0x06,
	.rated_Ah_0p1 = 500,
	.rated_V_dV = 3931
};

// Runtime parameters
static GbtRuntime g_runtime = {
	.demandVoltage_dV = 3931, // Start with demand voltage equal to rated voltage
	.demandCurrent_dA = -400, // Start with demand current at -40A
	.mode = 0x01, // Start in CV mode
	.packVoltage_dV = 3500, // Initial pack voltage in dV, set to 350V, will be updated based on messages received
	.packCurrent_dA = 0,    // Initial pack current in dA, set to 0A, will be updated based on messages received
	.tailSwitchVoltage_dV = 3920u, // Volage threshold for tail request in dV, set to 392V (Approx 4.1V per cell in 96s cell pack)
	.tailSwitchCurrent_dA = 70,    // Current threshold for tail request in dA, set to 7A
	.tailVoltage_dV = 4032u,      // Tail request voltage in dV, set to 403.2V (4.2V per cell in 96s cell pack)
	.tailCurrent_dA = -70,        // Tail request current in dA, set to -7A
	.maxCell_mV = 4000, // Dummy max cell voltage reported
	.maxCellGroup = 0, // Dummy max cell group
	.soc_percent = 50, // Dummy SOC percent (Can be changed in runtime via lcd interface)
	.remaining_min = 120, // Dummy remaining time in minutes
	.maxCellIndex = 1, // Dummy max cell index
	.tempMax_C = 30, //	Dummy max temperature in C
	.tempMaxIndex = 1, // Dummy max temperature cell index
	.tempMin_C = 25, // Dummy min temperature in C
	.tempMinIndex = 2, // Dummy min temperature cell index
	.permit_charge = 1 // Start with permit charge true, can be changed in runtime via lcd interface
};

static GbtChargeState g_state = GBT27930_CHARGE_STATE_WAIT_CHM;
static uint32_t t_last_BHM = 0, t_last_BRM = 0, t_last_BCP = 0, t_last_BRO = 0;
static uint32_t t_last_BCL = 0, t_last_BCS = 0, t_last_BSM = 0;
static uint32_t t_last_CCS_seen = 0;
static uint32_t t_last_BST = 0;

static uint8_t last_crm_confirm = 0;
static uint8_t g_bst_reason_3511 = 0x00u;
static uint16_t g_bst_reason_3512 = 0x0000u;
static uint8_t g_bst_reason_3513 = 0x00u;
static bool g_stop_sequence_active = false;
static bool g_bst_sent = false;
static bool g_tail_request_applied = false;

static void maybe_apply_tail_request(void)
{
	if (g_tail_request_applied) {
		return;
	}

	if (g_runtime.packVoltage_dV <= g_runtime.tailSwitchVoltage_dV) {
		return;
	}

	int32_t current_abs_dA = (int32_t)g_runtime.packCurrent_dA;
	if (current_abs_dA < 0) {
		current_abs_dA = -current_abs_dA;
	}

	if (current_abs_dA < g_runtime.tailSwitchCurrent_dA) {
		g_runtime.demandCurrent_dA = g_runtime.tailCurrent_dA;
		g_runtime.demandVoltage_dV = g_runtime.tailVoltage_dV;
		g_tail_request_applied = true;
	}
}

static uint32_t make_j1939_id(uint8_t prio, uint32_t pgn, uint8_t sa, uint8_t da)
{
	uint8_t pf = (uint8_t)((pgn >> 8) & 0xFFu);
	uint8_t dp = (uint8_t)((pgn >> 16) & 0x01u);
	uint8_t ps;
	uint32_t id;
	if (pf < 240u) {
		ps = da;
		id = ((uint32_t)(prio & 0x7u) << 26) |
		     ((uint32_t)dp << 24) |
		     ((uint32_t)pf << 16) |
		     ((uint32_t)ps << 8) |
		     (uint32_t)sa;
	} else {
		ps = (uint8_t)(pgn & 0xFFu);
		id = ((uint32_t)(prio & 0x7u) << 26) |
		     ((uint32_t)dp << 24) |
		     ((uint32_t)pf << 16) |
		     ((uint32_t)ps << 8) |
		     (uint32_t)sa;
	}
	return id;
}

static uint32_t decode_pgn_from_id(uint32_t id)
{
	uint8_t pf = (uint8_t)((id >> 16) & 0xFFu);
	uint8_t ps = (uint8_t)((id >> 8) & 0xFFu);
	uint8_t dp = (uint8_t)((id >> 24) & 0x01u);
	if (pf < 240u) {
		return ((uint32_t)dp << 16) | ((uint32_t)pf << 8) | 0x00u;
	}
	return ((uint32_t)dp << 16) | ((uint32_t)pf << 8) | (uint32_t)ps;
}

static void build_and_send_BHM(void)
{
	uint8_t d[2];
	put_u16le(d, g_batt.rated_V_dV);
	CanSendMsg(make_j1939_id(6, PGN_BHM, SA_BMS, SA_CHARGER), d, 2);
}

static void build_and_send_BRM(void)
{
	uint8_t out[49];
	memset(out, 0xFF, sizeof(out));
	out[0] = 0x01u; out[1] = 0x01u; out[2] = 0x00u;
	out[3] = g_batt.batt_type;
	put_u16le(&out[4], g_batt.rated_Ah_0p1);
	put_u16le(&out[6], g_batt.rated_V_dV);
	memcpy(&out[8], g_batt.manufacturer, 4);
	memcpy(&out[12], g_batt.pack_code, 4);
	uint8_t year_offset = (uint8_t)((g_batt.prod_year >= 1985u) ? (g_batt.prod_year - 1985u) : 0u);
	out[16] = year_offset;
	uint8_t month = g_batt.prod_month;
	uint8_t day = g_batt.prod_day;
	out[17] = (uint8_t)(((month / 10u) << 4) | (month % 10u));
	out[18] = (uint8_t)(((day / 10u) << 4) | (day % 10u));
	memcpy(&out[19], g_batt.plant, 3);
	out[22] = g_batt.ownership;
	out[23] = 0xFFu;
	memcpy(&out[24], g_batt.VIN, 17);
	memcpy(&out[41], g_batt.sw_ver, 8);
	/* send via transport protocol */
	(void)tp_send_rts_cts(7, PGN_BRM, SA_CHARGER, out, sizeof(out));
}

static void build_and_send_BCP(void)
{
	uint8_t p[13];
	memset(p, 0xFF, sizeof(p));
	put_u16le(&p[0], encode_cell_0p01V(4200)); // 4.2V max per cell
	put_u16le(&p[2], encode_current_off400((int16_t)500)); // 50A max
	put_u16le(&p[4], (uint16_t)200u); // Nomonal total energy in 0.1kWh, set to 20kWh
	put_u16le(&p[6], g_runtime.tailVoltage_dV + 50u); // Max charge voltage in dV, set to 5V above tail request voltage
	p[8] = (uint8_t)(50 + 50); // Max battery temperature in C, set to 50C
	put_u16le(&p[9], (uint16_t)(g_runtime.soc_percent * 10u)); // Remaining energy in 0.1%, set to current SOC * 10
	put_u16le(&p[11], g_runtime.packVoltage_dV); // Current battery voltage in dV
	(void)tp_send_rts_cts(7, PGN_BCP, SA_CHARGER, p, sizeof(p));
}

static void build_and_send_BRO(void)
{
	uint8_t p[1] = { 0xAAu };
	CanSendMsg(make_j1939_id(4, PGN_BRO, SA_BMS, SA_CHARGER), p, 1);
}

static void build_and_send_BCL(void)
{
	uint8_t p[5];
	put_u16le(&p[0], encode_voltage_dV(g_runtime.demandVoltage_dV));
	put_u16le(&p[2], encode_current_off400(g_runtime.demandCurrent_dA));
	p[4] = g_runtime.mode;
	CanSendMsg(make_j1939_id(6, PGN_BCL, SA_BMS, SA_CHARGER), p, 5);
}

static void build_and_send_BCS(void)
{
	uint8_t p[9];
	memset(p, 0xFF, sizeof(p));
	put_u16le(&p[0], g_runtime.packVoltage_dV);
	put_u16le(&p[2], encode_current_off400((int16_t)g_runtime.packCurrent_dA));
	uint16_t cell01 = encode_cell_0p01V(g_runtime.maxCell_mV) & 0x0FFFu;
	uint16_t pack3077 = (uint16_t)(((uint16_t)(g_runtime.maxCellGroup & 0x0Fu) << 12) | cell01);
	put_u16le(&p[4], pack3077);
	p[6] = (uint8_t)((g_runtime.soc_percent > 100u) ? 100u : g_runtime.soc_percent);
	put_u16le(&p[7], g_runtime.remaining_min);
	(void)tp_send_rts_cts(7, PGN_BCS, SA_CHARGER, p, sizeof(p));
}

static void build_and_send_BSM(void)
{
	uint8_t p[7];
	memset(p, 0xFF, sizeof(p));
	p[0] = (uint8_t)(g_runtime.maxCellIndex & 0xFFu);
	p[1] = (uint8_t)((int16_t)g_runtime.tempMax_C + 50);
	p[2] = g_runtime.tempMaxIndex;
	p[3] = (uint8_t)((int16_t)g_runtime.tempMin_C + 50);
	p[4] = g_runtime.tempMinIndex;
	uint8_t sp3096 = (uint8_t)(g_runtime.permit_charge & 0x03u);
	p[5] = 0x00u;
	p[6] = (uint8_t)((sp3096 & 0x03u) << 4);
	CanSendMsg(make_j1939_id(6, PGN_BSM, SA_BMS, SA_CHARGER), p, 7);
}

static void build_and_send_BST(void)
{
	uint8_t p[4];
	p[0] = g_bst_reason_3511;
	put_u16le(&p[1], g_bst_reason_3512);
	p[3] = g_bst_reason_3513;
	CanSendMsg(make_j1939_id(6, PGN_BST, SA_BMS, SA_CHARGER), p, 4);
}

static void stop_charge_on_timeout(void)
{
	if (!g_stop_sequence_active) {
		StopCharge(
			0x00u,
			GBT27930_BST_3512_COMM_TIMEOUT,
			0x00u);
	}
}

static int tp_send_rts_cts(uint8_t prio, uint32_t pgn, uint8_t dest, const uint8_t *payload, uint16_t len)
{
	if (len == 0 || payload == NULL) return -1;
	uint16_t total_bytes = len;
	uint8_t total_packets = (uint8_t)((len + 6u) / 7u);
	uint8_t max_pack_per_cts = 0xFFu;

	uint8_t rts[8];
	rts[0] = TP_CM_RTS;
	rts[1] = (uint8_t)(total_bytes & 0xFFu);
	rts[2] = (uint8_t)((total_bytes >> 8) & 0xFFu);
	rts[3] = total_packets;
	rts[4] = max_pack_per_cts;
	rts[5] = (uint8_t)(pgn & 0xFFu);
	rts[6] = (uint8_t)((pgn >> 8) & 0xFFu);
	rts[7] = (uint8_t)((pgn >> 16) & 0xFFu);

	uint32_t id_cm = make_j1939_id(prio, PGN_TP_CM, SA_BMS, dest);
	CanSendMsg(id_cm, rts, 8);

	uint8_t packets_sent = 0;
	uint32_t start_ts = GetTS();

	while (packets_sent < total_packets) {
		uint32_t t_wait_start = GetTS();
		int got_cts = 0;
		uint8_t allowed_packets = 0;
		uint8_t first_packet = 0;

		while ((GetTS() - t_wait_start) < TP_CTS_TIMEOUT_MS) {
			uint32_t id; uint8_t buf[8]; uint8_t lenb;
			if (CanRingBufferPop(&id, buf, &lenb)) {
				uint32_t rx_pgn = decode_pgn_from_id(id);
				if (rx_pgn == PGN_TP_CM) {
					uint8_t src = (uint8_t)(id & 0xFFu);
					if (src == dest) {
						uint8_t ctrl = buf[0];
						uint32_t cm_pgn = ((uint32_t)buf[5]) | ((uint32_t)buf[6] << 8) | ((uint32_t)buf[7] << 16);
						if (cm_pgn == pgn) {
							if (ctrl == TP_CM_CTS) {
								allowed_packets = buf[1];
								first_packet = buf[2];
								got_cts = 1;
								break;
							} else if (ctrl == TP_CM_EOM_ACK) {
								return 0;
							} else if (ctrl == TP_CM_ABORT) {
								return -1;
							}
						}
					}
				}
				handle_rx_message(id, buf, lenb);
			} else {
				DelayMs(1);
			}
		}

		if (!got_cts) {
			uint8_t abort[8]; memset(abort, 0xFF, sizeof(abort));
			abort[0] = TP_CM_ABORT; abort[1] = 0x02;
			put_u24le(&abort[5], pgn);
			CanSendMsg(id_cm, abort, 8);
			return -1;
		}

		uint8_t to_send = allowed_packets;
		if (to_send == 0) {
			continue;
		}

		uint8_t seq = first_packet;
		for (uint8_t i = 0; i < to_send && (seq <= total_packets); ++i, ++seq) {
			uint8_t dt[8];
			dt[0] = seq;
			uint16_t off = (uint16_t)((seq - 1u) * 7u);
			for (uint8_t b = 0; b < 7; ++b) {
				dt[1 + b] = ((off + b) < total_bytes) ? payload[off + b] : 0xFFu;
			}
			CanSendMsg(make_j1939_id(prio, PGN_TP_DT, SA_BMS, dest), dt, 8);
			++packets_sent;
			DelayMs(TP_FRAME_GAP_MS);
			uint32_t poll_start = GetTS();
			while ((GetTS() - poll_start) < 1u) {
				uint32_t id2; uint8_t b2[8]; uint8_t l2;
				if (CanRingBufferPop(&id2, b2, &l2)) {
					handle_rx_message(id2, b2, l2);
				} else {
					break;
				}
			}
		}

		if ((GetTS() - start_ts) > TP_OVERALL_TIMEOUT_MS) {
			uint8_t abort[8]; memset(abort, 0xFF, sizeof(abort));
			abort[0] = TP_CM_ABORT; abort[1] = 0x03;
			put_u24le(&abort[5], pgn);
			CanSendMsg(id_cm, abort, 8);
			return -1;
		}
	}

	uint32_t wait_eom_start = GetTS();
	while ((GetTS() - wait_eom_start) < TP_EOM_TIMEOUT_MS) {
		uint32_t id3; uint8_t b3[8]; uint8_t l3;
		if (CanRingBufferPop(&id3, b3, &l3)) {
			uint32_t rx_pgn = decode_pgn_from_id(id3);
			if (rx_pgn == PGN_TP_CM) {
				uint8_t src = (uint8_t)(id3 & 0xFFu);
				if (src == dest) {
					uint8_t ctrl = b3[0];
					uint32_t cm_pgn = ((uint32_t)b3[5]) | ((uint32_t)b3[6] << 8) | ((uint32_t)b3[7] << 16);
					if (cm_pgn == pgn) {
						if (ctrl == TP_CM_EOM_ACK) return 0;
						if (ctrl == TP_CM_ABORT) return -1;
					}
				}
			}
			handle_rx_message(id3, b3, l3);
		} else {
			DelayMs(1);
		}
	}
	return -1;
}

static void handle_rx_message(uint32_t id, uint8_t *buf, uint8_t len)
{
	uint32_t pgn = decode_pgn_from_id(id);
	(void)len;

	if (pgn == PGN_CHM) {
		if (g_state == GBT27930_CHARGE_STATE_WAIT_CHM) {
			g_state = GBT27930_CHARGE_STATE_SEND_BHM;
			t_last_BHM = 0;
		}
	} else if (pgn == PGN_CRM) {
		if (len >= 1) {
			uint8_t confirm = buf[0];
			last_crm_confirm = confirm;
			if (g_state == GBT27930_CHARGE_STATE_SEND_BHM) {
				g_state = GBT27930_CHARGE_STATE_SEND_BRM;
				t_last_BRM = 0;
			} else if (g_state == GBT27930_CHARGE_STATE_SEND_BRM && confirm == 0xAAu) {
				g_state = GBT27930_CHARGE_STATE_SEND_BCP;
				t_last_BCP = 0;
			}
		}
	} else if (pgn == PGN_CML) {
		if (g_state == GBT27930_CHARGE_STATE_SEND_BCP) {
			t_last_BCP = 0;
		}
	} else if (pgn == PGN_CRO) {
		if (len >= 1 && buf[0] == 0xAAu) {
			if (g_state == GBT27930_CHARGE_STATE_SEND_BCP || g_state == GBT27930_CHARGE_STATE_SEND_BRO) {
				g_state = GBT27930_CHARGE_STATE_CHARGING;
				t_last_BCL = t_last_BCS = t_last_BSM = 0;
				t_last_CCS_seen = GetTS();
			}
		}
	} else if (pgn == PGN_CCS) {
		if (g_state == GBT27930_CHARGE_STATE_CHARGING) {
			if (len >= 4) {
				uint16_t vout = (uint16_t)(buf[0] | (buf[1] << 8));
				int16_t iout_enc = (int16_t)(buf[2] | (buf[3] << 8));
				int16_t iout_dA = (int16_t)(iout_enc - 4000);
				g_runtime.packVoltage_dV = vout;
				g_runtime.packCurrent_dA = iout_dA;
			}
			t_last_CCS_seen = GetTS();
			if (len >= 7) {
				uint8_t permit_bits = buf[6] & 0x03u;
				g_runtime.permit_charge = (permit_bits == 0x01u) ? 1u : 0u;
			}
		}
	} else if (pgn == PGN_CST) {
		g_stop_sequence_active = false;
		g_bst_sent = false;
		g_state = GBT27930_CHARGE_STATE_END;
	}
}

void SetupCharge(void)
{
	g_state = GBT27930_CHARGE_STATE_WAIT_CHM;
	t_last_BHM = t_last_BRM = t_last_BCP = t_last_BRO = 0;
	t_last_BCL = t_last_BCS = t_last_BSM = 0;
	t_last_CCS_seen = 0;
	t_last_BST = 0;
	last_crm_confirm = 0;
	g_bst_reason_3511 = 0x00u;
	g_bst_reason_3512 = 0x0000u;
	g_bst_reason_3513 = 0x00u;
	g_stop_sequence_active = false;
	g_bst_sent = false;
	g_tail_request_applied = false;
	build_and_send_BHM();
	t_last_BHM = GetTS();
}

void StopCharge(uint8_t stop_reason_3511, uint16_t stop_reason_3512, uint8_t stop_reason_3513)
{
	g_bst_reason_3511 = stop_reason_3511;
	g_bst_reason_3512 = stop_reason_3512;
	g_bst_reason_3513 = stop_reason_3513;
	g_stop_sequence_active = true;
	g_bst_sent = false;
	t_last_BST = 0;
	g_state = GBT27930_CHARGE_STATE_END;
}

void StopChargeManual(void)
{
	StopCharge(GBT27930_BST_3511_NONE, GBT27930_BST_3512_NONE, GBT27930_BST_3513_NONE);
}

void LoopCharge(void)
{
	uint32_t now = GetTS();
	uint32_t id; uint8_t buf[8]; uint8_t len;

	while (CanRingBufferPop(&id, buf, &len)) {
		LED(1);
		handle_rx_message(id, buf, len);
	}

	switch (g_state) {
		case GBT27930_CHARGE_STATE_WAIT_CHM:
			if ((now - t_last_BHM) >= PERIOD_BHM_MS) {
				build_and_send_BHM();
				t_last_BHM = now;
			}
			break;
		case GBT27930_CHARGE_STATE_SEND_BHM:
			if ((now - t_last_BHM) >= PERIOD_BHM_MS) {
				build_and_send_BHM();
				t_last_BHM = now;
			}
			break;
		case GBT27930_CHARGE_STATE_SEND_BRM:
			LED(4);
			if ((now - t_last_BRM) >= PERIOD_BRM_MS) {
				build_and_send_BRM();
				t_last_BRM = now;
			}
			break;
		case GBT27930_CHARGE_STATE_SEND_BCP:
			LED(2);
			if ((now - t_last_BCP) >= PERIOD_BCP_MS) {
				build_and_send_BCP();
				t_last_BCP = now;
			}
			if ((now - t_last_BRO) >= PERIOD_BRO_MS) {
				build_and_send_BRO();
				t_last_BRO = now;
			}
			break;
		case GBT27930_CHARGE_STATE_SEND_BRO:
			if ((now - t_last_BRO) >= PERIOD_BRO_MS) {
				build_and_send_BRO();
				t_last_BRO = now;
			}
			break;
		case GBT27930_CHARGE_STATE_CHARGING:
			LED(3);
			maybe_apply_tail_request();
			// Revert this back after debug, on some stations this triggered stop, need more investigation
			//if ((now - t_last_CCS_seen) > 10000u) {
			//	stop_charge_on_timeout();
			//	break;
			//}
			if ((now - t_last_BCL) >= PERIOD_BCL_MS) {
				build_and_send_BCL();
				t_last_BCL = now;
			}
			if ((now - t_last_BSM) >= PERIOD_BSM_MS) {
				build_and_send_BSM();
				t_last_BSM = now;
			}
			if ((now - t_last_BCS) >= PERIOD_BCS_MS) {
				build_and_send_BCS();
				t_last_BCS = now;
			}
			break;
		case GBT27930_CHARGE_STATE_END:
			if (g_stop_sequence_active) {
				if (!g_bst_sent || ((now - t_last_BST) >= PERIOD_BST_MS)) {
					build_and_send_BST();
					g_bst_sent = true;
					t_last_BST = now;
				}
			}
			break;
		default:
			break;
	}
}

GbtChargeState GbtGetChargeState(void)
{
	return g_state;
}

const char *GbtChargeStateName(GbtChargeState state)
{
	switch (state) {
		case GBT27930_CHARGE_STATE_WAIT_CHM: return "WAIT";
		case GBT27930_CHARGE_STATE_SEND_BHM: return "BHM";
		case GBT27930_CHARGE_STATE_SEND_BRM: return "BRM";
		case GBT27930_CHARGE_STATE_SEND_BCP: return "BCP";
		case GBT27930_CHARGE_STATE_SEND_BRO: return "BRO";
		case GBT27930_CHARGE_STATE_CHARGING: return "CHG";
		case GBT27930_CHARGE_STATE_END: return "END";
		default: return "UNK";
	}
}

void GbtGetRuntime(GbtRuntime *out)
{
	if (out != NULL) {
		*out = g_runtime;
	}
}

void GbtSetRuntime(const GbtRuntime *in)
{
	if (in != NULL) {
		g_runtime = *in;
	}
}

int32_t GbtGetRuntimeField(GbtRuntimeField field)
{
	switch (field) {
		case GBT27930_RUNTIME_DEMAND_VOLTAGE: return (int32_t)g_runtime.demandVoltage_dV;
		case GBT27930_RUNTIME_DEMAND_CURRENT: return (int32_t)g_runtime.demandCurrent_dA;
		case GBT27930_RUNTIME_MODE: return (int32_t)g_runtime.mode;
		case GBT27930_RUNTIME_PACK_VOLTAGE: return (int32_t)g_runtime.packVoltage_dV;
		case GBT27930_RUNTIME_PACK_CURRENT: return (int32_t)g_runtime.packCurrent_dA;
		case GBT27930_RUNTIME_TAIL_SWITCH_VOLTAGE: return (int32_t)g_runtime.tailSwitchVoltage_dV;
		case GBT27930_RUNTIME_TAIL_SWITCH_CURRENT: return (int32_t)g_runtime.tailSwitchCurrent_dA;
		case GBT27930_RUNTIME_TAIL_VOLTAGE: return (int32_t)g_runtime.tailVoltage_dV;
		case GBT27930_RUNTIME_TAIL_CURRENT: return (int32_t)g_runtime.tailCurrent_dA;
		case GBT27930_RUNTIME_MAX_CELL_MV: return (int32_t)g_runtime.maxCell_mV;
		case GBT27930_RUNTIME_MAX_CELL_GROUP: return (int32_t)g_runtime.maxCellGroup;
		case GBT27930_RUNTIME_SOC_PERCENT: return (int32_t)g_runtime.soc_percent;
		case GBT27930_RUNTIME_REMAINING_MIN: return (int32_t)g_runtime.remaining_min;
		case GBT27930_RUNTIME_MAX_CELL_INDEX: return (int32_t)g_runtime.maxCellIndex;
		case GBT27930_RUNTIME_TEMP_MAX_C: return (int32_t)g_runtime.tempMax_C;
		case GBT27930_RUNTIME_TEMP_MAX_INDEX: return (int32_t)g_runtime.tempMaxIndex;
		case GBT27930_RUNTIME_TEMP_MIN_C: return (int32_t)g_runtime.tempMin_C;
		case GBT27930_RUNTIME_TEMP_MIN_INDEX: return (int32_t)g_runtime.tempMinIndex;
		case GBT27930_RUNTIME_PERMIT_CHARGE: return (int32_t)g_runtime.permit_charge;
		default: return 0;
	}
}

void GbtSetRuntimeField(GbtRuntimeField field, int32_t value)
{
	switch (field) {
		case GBT27930_RUNTIME_DEMAND_VOLTAGE:
			if (value < 0) value = 0;
			if (value > 10000) value = 10000;
			g_runtime.demandVoltage_dV = (uint16_t)value;
			break;
		case GBT27930_RUNTIME_DEMAND_CURRENT:
			if (value < -4000) value = -4000;
			if (value > 4000) value = 4000;
			g_runtime.demandCurrent_dA = (int16_t)value;
			break;
		case GBT27930_RUNTIME_MODE:
			g_runtime.mode = (value == 0x02) ? 0x02u : 0x01u;
			break;
		case GBT27930_RUNTIME_PACK_VOLTAGE:
			if (value < 0) value = 0;
			if (value > 10000) value = 10000;
			g_runtime.packVoltage_dV = (uint16_t)value;
			break;
		case GBT27930_RUNTIME_PACK_CURRENT:
			if (value < -4000) value = -4000;
			if (value > 4000) value = 4000;
			g_runtime.packCurrent_dA = (int16_t)value;
			break;
		case GBT27930_RUNTIME_TAIL_SWITCH_VOLTAGE:
			if (value < 0) value = 0;
			if (value > 10000) value = 10000;
			g_runtime.tailSwitchVoltage_dV = (uint16_t)value;
			break;
		case GBT27930_RUNTIME_TAIL_SWITCH_CURRENT:
			if (value < -4000) value = -4000;
			if (value > 4000) value = 4000;
			g_runtime.tailSwitchCurrent_dA = (int16_t)value;
			break;
		case GBT27930_RUNTIME_TAIL_VOLTAGE:
			if (value < 0) value = 0;
			if (value > 10000) value = 10000;
			g_runtime.tailVoltage_dV = (uint16_t)value;
			break;
		case GBT27930_RUNTIME_TAIL_CURRENT:
			if (value < -4000) value = -4000;
			if (value > 4000) value = 4000;
			g_runtime.tailCurrent_dA = (int16_t)value;
			break;
		case GBT27930_RUNTIME_MAX_CELL_MV:
			if (value < 0) value = 0;
			if (value > 24000) value = 24000;
			g_runtime.maxCell_mV = (uint16_t)value;
			break;
		case GBT27930_RUNTIME_MAX_CELL_GROUP:
			if (value < 0) value = 0;
			if (value > 15) value = 15;
			g_runtime.maxCellGroup = (uint8_t)value;
			break;
		case GBT27930_RUNTIME_SOC_PERCENT:
			if (value < 0) value = 0;
			if (value > 100) value = 100;
			g_runtime.soc_percent = (uint8_t)value;
			break;
		case GBT27930_RUNTIME_REMAINING_MIN:
			if (value < 0) value = 0;
			if (value > 600) value = 600;
			g_runtime.remaining_min = (uint16_t)value;
			break;
		case GBT27930_RUNTIME_MAX_CELL_INDEX:
			if (value < 0) value = 0;
			if (value > 255) value = 255;
			g_runtime.maxCellIndex = (uint8_t)value;
			break;
		case GBT27930_RUNTIME_TEMP_MAX_C:
			if (value < -50) value = -50;
			if (value > 120) value = 120;
			g_runtime.tempMax_C = (int8_t)value;
			break;
		case GBT27930_RUNTIME_TEMP_MAX_INDEX:
			if (value < 0) value = 0;
			if (value > 255) value = 255;
			g_runtime.tempMaxIndex = (uint8_t)value;
			break;
		case GBT27930_RUNTIME_TEMP_MIN_C:
			if (value < -50) value = -50;
			if (value > 120) value = 120;
			g_runtime.tempMin_C = (int8_t)value;
			break;
		case GBT27930_RUNTIME_TEMP_MIN_INDEX:
			if (value < 0) value = 0;
			if (value > 255) value = 255;
			g_runtime.tempMinIndex = (uint8_t)value;
			break;
		case GBT27930_RUNTIME_PERMIT_CHARGE:
			g_runtime.permit_charge = (value != 0) ? 1u : 0u;
			break;
		default:
			break;
	}
}
