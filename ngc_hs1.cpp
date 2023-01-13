#include "./ngc_hs1.hpp"

#include <cstdint>
#include <cassert>
#include <new>
#include <map>
#include <list>
#include <set>
#include <optional>
#include <algorithm>

void NGC_HS1::Peer::append(uint32_t msg_id, Tox_Message_Type type, const std::string& text) {
	order.push_back(msg_id);

	// overwrites
	auto& new_msg = dict[msg_id];
	new_msg.msg_id = msg_id;
	new_msg.type = type;
	new_msg.text = text;

	if (heard_of.count(msg_id)) {
		// we got history before we got the message
		heard_of.erase(msg_id);
	}

	fprintf(stderr, "HS: ######## last msgs ########\n");
	auto rit = order.crbegin();
	for (size_t i = 0; i < 10 && rit != order.crend(); i++, rit++) {
		fprintf(stderr, "  %08X - %s\n", *rit, dict.at(*rit).text.c_str());
	}
}

bool NGC_HS1::Peer::hear(uint32_t msg_id, uint32_t peer_number) {
	if (dict.count(msg_id)) {
		// we know
		return false;
	}

	if (heard_of.count(msg_id) && heard_of.at(msg_id).count(peer_number)) {
		// we heard it from that peer before
		return false;
	}

	heard_of[msg_id].emplace(peer_number);

	return true;
}

void _handle_HS1_ft_recv_request(
	Tox *tox,
	uint32_t group_number,
	uint32_t peer_number,
	const uint8_t* file_id, size_t file_id_size,
	void* user_data
);

bool _handle_HS1_ft_recv_init(
	Tox *tox,
	uint32_t group_number,
	uint32_t peer_number,
	const uint8_t* file_id, size_t file_id_size,
	const uint8_t transfer_id,
	const size_t file_size,
	void* user_data
);

void _handle_HS1_ft_recv_data(
	Tox *tox,
	uint32_t group_number,
	uint32_t peer_number,
	uint8_t transfer_id,
	size_t data_offset,
	const uint8_t* data, size_t data_size,
	void* user_data
);

void _handle_HS1_ft_send_data(
	Tox *tox,

	uint32_t group_number,
	uint32_t peer_number,
	uint8_t transfer_id,

	size_t data_offset, uint8_t* data, size_t data_size,
	void* user_data
);

NGC_HS1* NGC_HS1_new(const struct NGC_HS1_options* options) {
	auto* ngc_hs1_ctx = new NGC_HS1;
	ngc_hs1_ctx->options = *options;
	return ngc_hs1_ctx;
}

bool NGC_HS1_register_ext(NGC_HS1* ngc_hs1_ctx, NGC_EXT_CTX* ngc_ext_ctx) {
	ngc_ext_ctx->callbacks[NGC_EXT::HS1_REQUEST_LAST_IDS] = _handle_HS1_REQUEST_LAST_IDS;
	ngc_ext_ctx->callbacks[NGC_EXT::HS1_RESPONSE_LAST_IDS] = _handle_HS1_RESPONSE_LAST_IDS;

	ngc_ext_ctx->user_data[NGC_EXT::HS1_REQUEST_LAST_IDS] = ngc_hs1_ctx;
	ngc_ext_ctx->user_data[NGC_EXT::HS1_RESPONSE_LAST_IDS] = ngc_hs1_ctx;

	return true;
}

bool NGC_HS1_register_ft1(NGC_HS1* ngc_hs1_ctx, NGC_FT1* ngc_ft1_ctx) {
	ngc_hs1_ctx->ngc_ft1_ctx = ngc_ft1_ctx;

	NGC_FT1_register_callback_recv_request(ngc_ft1_ctx, NGC_FT1_file_kind::NGC_HS1_MESSAGE_BY_ID, _handle_HS1_ft_recv_request, ngc_hs1_ctx);
	NGC_FT1_register_callback_recv_init(ngc_ft1_ctx, NGC_FT1_file_kind::NGC_HS1_MESSAGE_BY_ID, _handle_HS1_ft_recv_init, ngc_hs1_ctx);
	NGC_FT1_register_callback_recv_data(ngc_ft1_ctx, NGC_FT1_file_kind::NGC_HS1_MESSAGE_BY_ID, _handle_HS1_ft_recv_data, ngc_hs1_ctx);
	NGC_FT1_register_callback_send_data(ngc_ft1_ctx, NGC_FT1_file_kind::NGC_HS1_MESSAGE_BY_ID, _handle_HS1_ft_send_data, ngc_hs1_ctx);

	return true;
}

void NGC_HS1_kill(NGC_HS1* ngc_hs1_ctx) {
	delete ngc_hs1_ctx;
}

static void _iterate_group(Tox *tox, NGC_HS1* ngc_hs1_ctx, uint32_t group_number, float time_delta) {
	NGC_EXT::GroupKey g_id{};
	{ // TODO: error
		tox_group_get_chat_id(tox, group_number, g_id.data.data(), nullptr);
	}

	if (ngc_hs1_ctx->history.count(g_id) == 0) {
		fprintf(stderr, "HS: adding new group: %u %X%X%X%X\n",
			group_number,
			g_id.data.data()[0],
			g_id.data.data()[1],
			g_id.data.data()[2],
			g_id.data.data()[3]
		);
		ngc_hs1_ctx->history[g_id];
	} else {
		auto& group = ngc_hs1_ctx->history[g_id];

		// check if transfers have timed out
		for (auto it = group.transfers.begin(); it != group.transfers.end();) {
			it->second.time_since_ft_activity += time_delta;
			if (it->second.time_since_ft_activity >= ngc_hs1_ctx->options.ft_activity_timeout) {
				// timed out
				fprintf(stderr, "HS: !!! ft timed out (%08X)\n", it->first.first);
				it = group.transfers.erase(it);
			} else {
				it++;
			}
		}

		// for each peer
		for (auto& [peer_key, peer] : group.peers) {
			//fprintf(stderr, "  p: %X%X%X%X\n", key.data.data()[0], key.data.data()[1], key.data.data()[2], key.data.data()[3]);
			peer.time_since_last_request_sent += time_delta;
			if (peer.time_since_last_request_sent > ngc_hs1_ctx->options.query_interval_per_peer) {
				peer.time_since_last_request_sent = 0.f;

				//fprintf(stderr, "HS: requesting ids for %X%X%X%X\n", peer_key.data.data()[0], peer_key.data.data()[1], peer_key.data.data()[2], peer_key.data.data()[3]);

				// TODO: other way around?
				// ask everyone if they have newer stuff for this peer

				// - 1 byte packet id
				// - peer_key bytes (peer key we want to know ids for)
				// - 1 byte (uint8_t count ids, atleast 1)
				std::array<uint8_t, 1+TOX_GROUP_PEER_PUBLIC_KEY_SIZE+1> pkg;
				pkg[0] = NGC_EXT::HS1_REQUEST_LAST_IDS;
				std::copy(peer_key.data.begin(), peer_key.data.end(), pkg.begin()+1);
				pkg[1+TOX_GROUP_PEER_PUBLIC_KEY_SIZE] = ngc_hs1_ctx->options.last_msg_ids_count; // request last (up to) 5 msg_ids

				tox_group_send_custom_packet(tox, group_number, true, pkg.data(), pkg.size(), nullptr);
			}

			// check if pending msg requests have timed out
			for (auto it = peer.pending.begin(); it != peer.pending.end();) {
				it->second.time_since_ft_activity += time_delta;
				if (it->second.time_since_ft_activity >= ngc_hs1_ctx->options.ft_activity_timeout) {
					// timed out
					fprintf(stderr, "HS: !!! pending ft request timed out (%08X)\n", it->first);
					it = peer.pending.erase(it);
				} else {
					it++;
				}
			}

			// request FT for only heard of message_ids
			size_t request_made_count = 0;
			for (const auto& [msg_id, remote_peer_numbers] : peer.heard_of) {
				if (request_made_count >= 2) { // 2 for test
					// TODO: limit requests per iterate option
					break;
				}

				if (peer.pending.count(msg_id)) {
					continue; // allready requested
				}

				if (remote_peer_numbers.empty()) {
					fprintf(stderr, "HS: !!! msg_id we heard of, but no remote peer !!!\n");
					continue;
				}

				const uint32_t remote_peer_number = *remote_peer_numbers.begin();

				// craft file id
				std::array<uint8_t, TOX_GROUP_PEER_PUBLIC_KEY_SIZE+sizeof(uint32_t)> file_id{};
				{
					std::copy(peer_key.data.cbegin(), peer_key.data.cend(), file_id.begin());

					// HACK: little endian
					const uint8_t* tmp_ptr = reinterpret_cast<const uint8_t*>(&msg_id);
					std::copy(tmp_ptr, tmp_ptr+sizeof(uint32_t), file_id.begin()+TOX_GROUP_PEER_PUBLIC_KEY_SIZE);
				}

				// send request
				NGC_FT1_send_request_private(
					tox, ngc_hs1_ctx->ngc_ft1_ctx,
					group_number, remote_peer_number,
					NGC_FT1_file_kind::NGC_HS1_MESSAGE_BY_ID,
					file_id.data(), file_id.size()
				);

				peer.pending[msg_id] = {remote_peer_number, 0.f};

				request_made_count++;
			}
		}
	}

	assert(ngc_hs1_ctx->history.size() != 0);
	assert(ngc_hs1_ctx->history.count(g_id));
}

void NGC_HS1_iterate(Tox *tox, NGC_HS1* ngc_hs1_ctx) {
	assert(ngc_hs1_ctx);

	uint32_t group_count = tox_group_get_number_groups(tox);
	// this can loop endless if toxcore misbehaves
	for (uint32_t g_i = 0, g_c_done = 0; g_c_done < group_count; g_i++) {
		Tox_Err_Group_Is_Connected g_err;
		if (tox_group_is_connected(tox, g_i, &g_err)) {
			// valid and connected here
			// TODO: delta time, or other timers
			_iterate_group(tox, ngc_hs1_ctx, g_i, 0.02f);
			g_c_done++;
		} else if (g_err != TOX_ERR_GROUP_IS_CONNECTED_GROUP_NOT_FOUND) {
			g_c_done++;
		} // else do nothing

		// safety
		if (g_i > group_count + 1000) {
			fprintf(stderr, "HS: WAY PAST GOUPS in iterate\n");
			break;
		}
	}
}

void NGC_HS1_peer_online(Tox* tox, NGC_HS1* ngc_hs1_ctx, uint32_t group_number, uint32_t peer_number, bool online) {
	// get group id
	NGC_EXT::GroupKey g_id{};
	{ // TODO: error
		tox_group_get_chat_id(tox, group_number, g_id.data.data(), nullptr);
	}

	auto& group = ngc_hs1_ctx->history[g_id];

	if (online) {
		// get peer id
		NGC_EXT::PeerKey p_id{};
		{ // TODO: error
			tox_group_peer_get_public_key(tox, group_number, peer_number, p_id.data.data(), nullptr);
		}

		auto& peer = group.peers[p_id];
		peer.id = peer_number;
	} else { // offline
		// search
		for (auto& [key, peer] : group.peers) {
			if (peer.id.has_value() && peer.id.value() == peer_number) {
				peer.id = {}; // reset
				break;
			}
		}
	}
}

bool NGC_HS1_shim_group_send_message(
	const Tox *tox,
	NGC_HS1* ngc_hs1_ctx,

	uint32_t group_number,

	Tox_Message_Type type, const uint8_t *message, size_t length,

	uint32_t *message_id,
	Tox_Err_Group_Send_Message *error
) {
	uint32_t* msg_id_ptr = message_id;
	uint32_t msg_id_placeholder = 0;
	if (msg_id_ptr == nullptr) {
		msg_id_ptr = &msg_id_placeholder;
	}

	bool ret = tox_group_send_message(tox, group_number, type, message, length, msg_id_ptr, error);

	NGC_HS1_record_own_message(tox, ngc_hs1_ctx, group_number, type, message, length, *msg_id_ptr);

	return ret;
}

// record own msg
void NGC_HS1_record_own_message(
	const Tox *tox,
	NGC_HS1* ngc_hs1_ctx,

	uint32_t group_number,

	Tox_Message_Type type, const uint8_t *message, size_t length, uint32_t message_id
) {
	fprintf(stderr, "HS: record_own_message %08X\n", message_id);
	// get group id
	NGC_EXT::GroupKey g_id{};
	{ // TODO: error
		tox_group_get_chat_id(tox, group_number, g_id.data.data(), nullptr);
	}

	// get peer id
	NGC_EXT::PeerKey p_id{};
	{ // TODO: error
		tox_group_self_get_public_key(tox, group_number, p_id.data.data(), nullptr);
	}

	ngc_hs1_ctx->history[g_id].peers[p_id].append(message_id, type, std::string{message, message+length});
	assert(ngc_hs1_ctx->history.size() != 0);
	assert(ngc_hs1_ctx->history.count(g_id));
}

void NGC_HS1_register_callback_group_message(NGC_HS1* ngc_hs1_ctx, NGC_HS1_group_message_cb* callback) {
	assert(ngc_hs1_ctx);

	ngc_hs1_ctx->cb_group_message = callback;
}

// record others msg
void NGC_HS1_record_message(
	const Tox *tox,
	NGC_HS1* ngc_hs1_ctx,

	uint32_t group_number,
	uint32_t peer_number,

	Tox_Message_Type type, const uint8_t *message, size_t length, uint32_t message_id
) {
	if (!ngc_hs1_ctx->options.record_others) {
		return;
	}

	fprintf(stderr, "HS: record_message %08X\n", message_id);
	// get group id
	NGC_EXT::GroupKey g_id{};
	{ // TODO: error
		tox_group_get_chat_id(tox, group_number, g_id.data.data(), nullptr);
	}

	// get peer id
	NGC_EXT::PeerKey p_id{};
	{ // TODO: error
		tox_group_peer_get_public_key(tox, group_number, peer_number, p_id.data.data(), nullptr);
	}

	ngc_hs1_ctx->history[g_id].peers[p_id].append(message_id, type, std::string{message, message+length});
}

void _handle_HS1_ft_recv_request(
	Tox *tox,
	uint32_t group_number,
	uint32_t peer_number,
	const uint8_t* file_id, size_t file_id_size,
	void* user_data
) {
	assert(user_data);
	NGC_HS1* ngc_hs1_ctx = static_cast<NGC_HS1*>(user_data);
	assert(file_id_size == TOX_GROUP_PEER_PUBLIC_KEY_SIZE+sizeof(uint32_t));

	// get peer_key from file_id
	NGC_EXT::PeerKey peer_key;
	std::copy(file_id, file_id+peer_key.size(), peer_key.data.begin());

	// get msg_id from file_id
	// HACK: little endian
	uint32_t msg_id;
	uint8_t* tmp_ptr = reinterpret_cast<uint8_t*>(&msg_id);
	std::copy(file_id+TOX_GROUP_PEER_PUBLIC_KEY_SIZE, file_id+TOX_GROUP_PEER_PUBLIC_KEY_SIZE+sizeof(uint32_t), tmp_ptr);

	fprintf(stderr, "HS: got a ft request for xxx msg_id %08X\n", msg_id);

	// get group id
	NGC_EXT::GroupKey group_id{};
	{ // TODO: error
		tox_group_get_chat_id(tox, group_number, group_id.data.data(), nullptr);
	}

	const auto& peers = ngc_hs1_ctx->history[group_id].peers;

	// do we have that message

	if (!peers.count(peer_key)) {
		fprintf(stderr, "HS: got ft request for unknown peer\n");
		return;
	}

	const auto& peer = peers.at(peer_key);
	if (!peer.dict.count(msg_id)) {
		fprintf(stderr, "HS: got ft request for unknown message_id %08X\n", msg_id);
		return;
	}

	// yes we do. now we need to init ft?

	//fprintf(stderr, "TODO: init ft for %08X\n", msg_id);

	// filesize is
	// - 1 byte msg_type (normal / action)
	// - x bytes msg_text
	// msg_id is part of file_id
	const auto& msg = peer.dict.at(msg_id);
	size_t file_size = 1 + msg.text.size();

	uint8_t transfer_id {0};

	NGC_FT1_send_init_private(
		tox, ngc_hs1_ctx->ngc_ft1_ctx,
		group_number, peer_number,
		NGC_HS1_MESSAGE_BY_ID,
		file_id, file_id_size,
		file_size,
		&transfer_id
	);

	//TODO: can fail

	ngc_hs1_ctx->history[group_id].sending[std::make_pair(peer_number, transfer_id)] = {peer_key, msg_id};
}

bool _handle_HS1_ft_recv_init(
	Tox *tox,
	uint32_t group_number,
	uint32_t peer_number,
	const uint8_t* file_id, size_t file_id_size,
	const uint8_t transfer_id,
	const size_t file_size,
	void* user_data
) {
	assert(user_data);
	NGC_HS1* ngc_hs1_ctx = static_cast<NGC_HS1*>(user_data);
	//fprintf(stderr, "HS: -------hs handle ft init\n");

	// peer id and msg id from file id
	// TODO: replace, remote crash
	assert(file_id_size == TOX_GROUP_PEER_PUBLIC_KEY_SIZE+sizeof(uint32_t));

	// get peer_key from file_id
	NGC_EXT::PeerKey peer_key;
	std::copy(file_id, file_id+peer_key.size(), peer_key.data.begin());

	// get msg_id from file_id
	// HACK: little endian
	uint32_t msg_id;
	uint8_t* tmp_ptr = reinterpret_cast<uint8_t*>(&msg_id);
	std::copy(file_id+TOX_GROUP_PEER_PUBLIC_KEY_SIZE, file_id+TOX_GROUP_PEER_PUBLIC_KEY_SIZE+sizeof(uint32_t), tmp_ptr);

	// did we ask for this?

	// get group id
	NGC_EXT::GroupKey g_id{};
	{ // TODO: error
		tox_group_get_chat_id(tox, group_number, g_id.data.data(), nullptr);
	}

	auto& group = ngc_hs1_ctx->history[g_id];

	auto& pending = group.peers[peer_key].pending;

	if (!pending.count(msg_id)) {
		// we did not ask for this
		// TODO: accept?
		fprintf(stderr, "HS: ft init from peer we did not ask\n");
		return false; // deny
	}

	if (pending.at(msg_id).peer_number != peer_number) {
		// wrong peer ?
		fprintf(stderr, "HS: ft init from peer we did not ask while asking someone else\n");
		return false; // deny
	}

	// TODO: if allready acked but got init again, they did not get the ack

	// move from pending to transfers
	group.transfers[std::make_pair(peer_number, transfer_id)] = {
		peer_key,
		msg_id,
		0.f,
		{}, // empty buffer
		file_size,
	};

	pending.at(msg_id).time_since_ft_activity = 0.f;

	// keep the pending until later

	return true; // accept
}

void _handle_HS1_ft_recv_data(
	Tox *tox,
	uint32_t group_number,
	uint32_t peer_number,
	uint8_t transfer_id,
	size_t data_offset,
	const uint8_t* data, size_t data_size,
	void* user_data
) {
	assert(user_data);
	NGC_HS1* ngc_hs1_ctx = static_cast<NGC_HS1*>(user_data);

	// get group id
	NGC_EXT::GroupKey g_id{};
	{ // TODO: error
		tox_group_get_chat_id(tox, group_number, g_id.data.data(), nullptr);
	}

	auto& group = ngc_hs1_ctx->history[g_id];

	// get based on transfer_id
	if (!group.transfers.count(std::make_pair(peer_number, transfer_id))) {
		if (data_offset != 0) {
			fprintf(stderr, "HS: !! got stray tf data from %d tid:%d\n", peer_number, transfer_id);
			return;
		}

		// new transfer?
		fprintf(stderr, "HS: !! got new transfer from %d tid:%d\n", peer_number, transfer_id);
	}

	fprintf(stderr, "HS: recv_data from %d tid:%d\n", peer_number, transfer_id);

	auto& transfer = group.transfers.at(std::make_pair(peer_number, transfer_id));
	transfer.time_since_ft_activity = 0.f;
	// TODO: also timer for pending?

	// TODO: optimize
	for (size_t i = 0; i < data_size; i++) {
		transfer.recv_buffer.push_back(data[i]);
	}

	// TODO: data done?
	if (data_offset + data_size == transfer.file_size) {
		fprintf(stderr, "HS: transfer done %d:%d\n", peer_number, transfer_id);
		transfer.recv_buffer.push_back('\0');
		fprintf(stderr, "    message was %s\n", transfer.recv_buffer.data()+1);

		auto& peer = group.peers[transfer.msg_peer];
		peer.pending.erase(transfer.msg_id);
		peer.append(transfer.msg_id, static_cast<Tox_Message_Type>(transfer.recv_buffer.front()), std::string(reinterpret_cast<const char*>(transfer.recv_buffer.data()+1)));

		assert(ngc_hs1_ctx->cb_group_message);
		// we dont notify if we dont know the peer id. this kinda breaks some stuff
		if (peer.id.has_value()) {
			ngc_hs1_ctx->cb_group_message(
				tox,
				group_number, peer.id.value(),
				static_cast<Tox_Message_Type>(transfer.recv_buffer.front()),
				transfer.recv_buffer.data()+1,
				transfer.recv_buffer.size()-2,
				transfer.msg_id
			);
		}

		group.transfers.erase(std::make_pair(peer_number, transfer_id));
	}
}

void _handle_HS1_ft_send_data(
	Tox *tox,

	uint32_t group_number,
	uint32_t peer_number,
	uint8_t transfer_id,

	size_t data_offset, uint8_t* data, size_t data_size,
	void* user_data
) {
	assert(user_data);
	NGC_HS1* ngc_hs1_ctx = static_cast<NGC_HS1*>(user_data);

	// get group id
	NGC_EXT::GroupKey g_id{};
	{ // TODO: error
		tox_group_get_chat_id(tox, group_number, g_id.data.data(), nullptr);
	}

	auto& group = ngc_hs1_ctx->history[g_id];

	if (!group.sending.count(std::make_pair(peer_number, transfer_id))) {
		fprintf(stderr, "HS: error, unknown sending transfer %d:%d\n", peer_number, transfer_id);
		return;
	}

	// map peer_number and transfer_id to peer_key and message_id
	const auto& [msg_peer, msg_id] = group.sending.at(std::make_pair(peer_number, transfer_id));

	// get msg
	const auto& message = group.peers.at(msg_peer).dict.at(msg_id);

	size_t data_i = 0;
	if (data_offset == 0) {
		// serl type
		data[data_i++] = message.type;
		data_offset += 1;
	}

	for (size_t i = 0; data_i < data_size; i++, data_i++) {
		data[data_i] = message.text.at(data_offset+i-1);
	}

	if (data_offset + data_size == 1 + message.text.size()) {
		// done
		fprintf(stderr, "HS: done %d:%d\n", peer_number, transfer_id);
		group.sending.erase(std::make_pair(peer_number, transfer_id));
	}
}

#define _HS1_HAVE(x, error) if ((length - curser) < (x)) { error; }

void _handle_HS1_REQUEST_LAST_IDS(
	Tox* tox,
	NGC_EXT_CTX* ngc_ext_ctx,

	uint32_t group_number,
	uint32_t peer_number,

	const uint8_t *data,
	size_t length,
	void* user_data
) {
	assert(user_data);
	NGC_HS1* ngc_hs1_ctx = static_cast<NGC_HS1*>(user_data);
	size_t curser = 0;

	NGC_EXT::PeerKey p_key;
	_HS1_HAVE(p_key.data.size(), fprintf(stderr, "HS: packet too small, missing pkey\n"); return)

	std::copy(data+curser, data+curser+p_key.data.size(), p_key.data.begin());
	curser += p_key.data.size();

	_HS1_HAVE(1, fprintf(stderr, "HS: packet too small, missing count\n"); return)
	uint8_t last_msg_id_count = data[curser++];

	//fprintf(stderr, "HS: got request for last %u ids\n", last_msg_id_count);

	// get group id
	NGC_EXT::GroupKey g_id{};
	{ // TODO: error
		tox_group_get_chat_id(tox, group_number, g_id.data.data(), nullptr);
	}

	auto& group = ngc_hs1_ctx->history[g_id];

	std::vector<uint32_t> message_ids{};

	if (!group.peers.empty() && group.peers.count(p_key)) {
		const auto& peer = group.peers.at(p_key);
		auto rit = peer.order.crbegin();
		for (size_t c = 0; c < last_msg_id_count && rit != peer.order.crend(); c++, rit++) {
			message_ids.push_back(*rit);
		}
	}

	// - 1 byte packet id
	// respond to a request with 0 or more message ids, sorted by newest first
	// - peer_key bytes (the msg_ids are from)
	// - 1 byte (uint8_t count ids, can be 0)
	// - array [
	//   - msg_id bytes (the message id
	// - ]
	//std::array<uint8_t, 1+TOX_GROUP_PEER_PUBLIC_KEY_SIZE+1+> pkg;
	std::vector<uint8_t> pkg;
	pkg.resize(1+TOX_GROUP_PEER_PUBLIC_KEY_SIZE+1+sizeof(uint32_t)*message_ids.size());

	size_t packing_curser = 0;

	pkg[packing_curser++] = NGC_EXT::HS1_RESPONSE_LAST_IDS;

	std::copy(p_key.data.begin(), p_key.data.end(), pkg.begin()+packing_curser);
	packing_curser += p_key.data.size();

	pkg[packing_curser++] = message_ids.size();

	for (size_t i = 0; i < message_ids.size(); i++) {
		const uint8_t* tmp_ptr = reinterpret_cast<uint8_t*>(message_ids.data()+i);
		// HACK: little endian
		//std::copy(tmp_ptr, tmp_ptr+sizeof(uint32_t), pkg.begin()+1+TOX_GROUP_PEER_PUBLIC_KEY_SIZE+1+i*sizeof(uint32_t));
		std::copy(tmp_ptr, tmp_ptr+sizeof(uint32_t), pkg.begin()+packing_curser);
		packing_curser += sizeof(uint32_t);
	}

	tox_group_send_custom_private_packet(tox, group_number, peer_number, true, pkg.data(), pkg.size(), nullptr);
}

void _handle_HS1_RESPONSE_LAST_IDS(
	Tox* tox,
	NGC_EXT_CTX* ngc_ext_ctx,

	uint32_t group_number,
	uint32_t peer_number,

	const uint8_t *data,
	size_t length,
	void* user_data
) {
	assert(user_data);
	NGC_HS1* ngc_hs1_ctx = static_cast<NGC_HS1*>(user_data);
	size_t curser = 0;

	NGC_EXT::PeerKey p_key;
	_HS1_HAVE(p_key.data.size(), fprintf(stderr, "HS: packet too small, missing pkey\n"); return)

	std::copy(data+curser, data+curser+p_key.data.size(), p_key.data.begin());
	curser += p_key.data.size();

	// TODO: did we ask?

	_HS1_HAVE(1, fprintf(stderr, "HS: packet too small, missing count\n"); return)
	uint8_t last_msg_id_count = data[curser++];

	fprintf(stderr, "HS: got response with last %u ids:\n", last_msg_id_count);

	if (last_msg_id_count == 0) {
		return;
	}

	// get group id
	NGC_EXT::GroupKey g_id{};
	{ // TODO: error
		tox_group_get_chat_id(tox, group_number, g_id.data.data(), nullptr);
	}

	// get peer
	auto& peer = ngc_hs1_ctx->history[g_id].peers[p_key];

	//std::vector<uint32_t> message_ids{};

	for (size_t i = 0; i < last_msg_id_count && curser+sizeof(uint32_t) <= length; i++) {
		uint32_t msg_id;

		// HACK: little endian
		std::copy(data+curser, data+curser+sizeof(uint32_t), reinterpret_cast<uint8_t*>(&msg_id));
		curser += sizeof(uint32_t);

		//message_ids.push_back(msg_id);

		fprintf(stderr, "  %08X", msg_id);

		if (peer.hear(msg_id, peer_number)) { // <-- the important code is here
			fprintf(stderr, " - NEW");
		}

		fprintf(stderr, "\n");
	}

	// TODO: replace, remote crash
	assert(curser == length);
}

#undef _HS1_HAVE

