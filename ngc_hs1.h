#ifndef C_NGC_HS1_H
#define C_NGC_HS1_H

// this is a c header

//#include <stdbool.h>
//#include <stddef.h>
//#include <stdint.h>

#include <tox/tox.h>

#include "ngc_ext.h"
#include "ngc_ft1.h"

#ifdef __cplusplus
extern "C" {
#endif

// ========== struct / typedef ==========

typedef struct NGC_HS1 NGC_HS1;

struct NGC_HS1_options {
	// (and up)
	// 0 all
	// 1 users
	// 2 mods
	// 3 founders
	// 4 no one (above founder)
	uint8_t default_trust_level /*= 2*/; // TODO: unused right now

	// if false, will only record own messages
	bool record_others;

	float query_interval_per_peer; // 15.f

	// how many msg_ids to query from peers in the group
	size_t last_msg_ids_count; // 5

	// after which the filetransfer is canceled, and potentially restart, with maybe another peer
	float ft_activity_timeout; // seconds 60.f
};

// ========== init / kill ==========

NGC_HS1* NGC_HS1_new(const struct NGC_HS1_options* options);
bool NGC_HS1_register_ext(NGC_HS1* ngc_hs1_ctx, NGC_EXT_CTX* ngc_ext_ctx);
bool NGC_HS1_register_ft1(NGC_HS1* ngc_hs1_ctx, NGC_FT1* ngc_ft1_ctx);
void NGC_HS1_kill(NGC_HS1* ngc_hs1_ctx);

// ========== iterate ==========

void NGC_HS1_iterate(Tox *tox, NGC_HS1* ngc_hs1_ctx);

// ========== peer online/offline ==========

void NGC_HS1_peer_online(Tox* tox, NGC_HS1* ngc_hs1_ctx, uint32_t group_number, uint32_t peer_number, bool online);

// ========== send ==========

// shim
bool NGC_HS1_shim_group_send_message(
	const Tox *tox,
	NGC_HS1* ngc_hs1_ctx,

	uint32_t group_number,

	Tox_Message_Type type, const uint8_t *message, size_t length,

	uint32_t *message_id,
	Tox_Err_Group_Send_Message *error
);

// record own msg
void NGC_HS1_record_own_message(
	const Tox *tox,
	NGC_HS1* ngc_hs1_ctx,

	uint32_t group_number,

	Tox_Message_Type type, const uint8_t *message, size_t length, uint32_t message_id
);

// ========== receive message ==========

typedef void NGC_HS1_group_message_cb(
	Tox *tox,
	uint32_t group_number,
	uint32_t peer_id,
	Tox_Message_Type type,
	const uint8_t *message,
	size_t length,
	uint32_t message_id
);

// callback for when history sync has a new message
// fake tox interface variant that is limited to peers that have been observed since the program started
void NGC_HS1_register_callback_group_message(NGC_HS1* ngc_hs1_ctx, NGC_HS1_group_message_cb* callback); // TODO: userdata

// record others msg
void NGC_HS1_record_message(
	const Tox *tox,
	NGC_HS1* ngc_hs1_ctx,

	uint32_t group_number,
	uint32_t peer_number,

	Tox_Message_Type type, const uint8_t *message, size_t length, uint32_t message_id
);

#ifdef __cplusplus
}
#endif

#endif // C_NGC_HS1_H

