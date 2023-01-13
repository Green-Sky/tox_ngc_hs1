#pragma once

#include "./ngc_hs1.h"

#include "ngc_ext.hpp"

#include <cstdint>
#include <map>
#include <list>
#include <set>
#include <vector>
#include <optional>

struct NGC_HS1 {
	NGC_HS1_options options;

	NGC_FT1* ngc_ft1_ctx {nullptr};

	// callbacks
	NGC_HS1_group_message_cb* cb_group_message {nullptr};

	// key			- key			- key		- value store
	// group pubkey - peer pubkey	- msg_id	- message(type + text)
	struct Message {
		uint32_t msg_id{};
		Tox_Message_Type type{};
		std::string text{};
	};

	struct Peer {
		std::optional<uint32_t> id;
		std::map<uint32_t, Message> dict;
		std::list<uint32_t> order; // ordered list of message ids

		// msg_ids we have only heard of, with peer_number of who we heard it from
		std::map<uint32_t, std::set<uint32_t>> heard_of;

		struct PendingFTRequest {
			uint32_t peer_number; // the peer we requested the message from
			float time_since_ft_activity {0.f};
		};
		std::map<uint32_t, PendingFTRequest> pending; // key msg_id

		// dont start immediatly
		float time_since_last_request_sent {0.f};

		void append(uint32_t msg_id, Tox_Message_Type type, const std::string& text);

		// returns if new (from that peer)
		bool hear(uint32_t msg_id, uint32_t peer_number);
	};

	struct Group {
		std::map<NGC_EXT::PeerKey, Peer> peers;

		struct FileTransfers {
			NGC_EXT::PeerKey msg_peer;
			uint32_t msg_id;
			float time_since_ft_activity {0.f};
			std::vector<uint8_t> recv_buffer; // message gets dumped into here
			size_t file_size {0};
		};
		// key: peer_number + transfer_id
		std::map<std::pair<uint32_t, uint8_t>, FileTransfers> transfers;

		struct Sending {
			NGC_EXT::PeerKey msg_peer;
			uint32_t msg_id;
		};
		std::map<std::pair<uint32_t, uint8_t>, Sending> sending;
	};

	std::map<NGC_EXT::GroupKey, Group> history;
};

void _handle_HS1_REQUEST_LAST_IDS(
	Tox* tox,
	NGC_EXT_CTX* ngc_ext_ctx,

	uint32_t group_number,
	uint32_t peer_number,

	const uint8_t *data,
	size_t length,
	void* user_data
);

void _handle_HS1_RESPONSE_LAST_IDS(
	Tox* tox,
	NGC_EXT_CTX* ngc_ext_ctx,

	uint32_t group_number,
	uint32_t peer_number,

	const uint8_t *data,
	size_t length,
	void* user_data
);

void _handle_HS1_ft_request_message(
	Tox *tox, NGC_EXT_CTX* ngc_ext_ctx,
	uint32_t group_number,
	uint32_t peer_number,
	const uint8_t* file_id, size_t file_id_size
);

bool _handle_HS1_ft_init_message(
	Tox *tox, NGC_EXT_CTX* ngc_ext_ctx,
	uint32_t group_number,
	uint32_t peer_number,
	const uint8_t* file_id, size_t file_id_size,
	const uint8_t transfer_id,
	const size_t file_size
);

