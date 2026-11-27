#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

#include <ufsrvcorelib/ufsrv_core/turn/codec/ns_turn_msg.h>
#include "turn/policy/turn_dispatch.h"

static void
CheckRequests(void)
{
  uint8_t packet[128];
  size_t length;
  TurnDatagram frame;
  const uint8_t udp_transport[4] = {17};
  const uint8_t peer[8] = {0, 1};
  const uint8_t channel[4] = {0x40, 0};
  const uint8_t integrity[20] = {0};

  stun_init_request_str(STUN_METHOD_BINDING, packet, &length);
  assert(TurnDecodeDatagram(packet, length, false, &frame) == TURN_DECODE_OK);
  assert(frame.kind == TURN_DATAGRAM_BINDING && frame.policy->auth_mode == TURN_AUTH_PUBLIC);

  stun_init_request_str(STUN_METHOD_ALLOCATE, packet, &length);
  assert(TurnDecodeDatagram(packet, length, true, &frame) != TURN_DECODE_OK);
  assert(stun_attr_add_str(packet, &length, STUN_ATTRIBUTE_REQUESTED_TRANSPORT, udp_transport, 4));
  assert(TurnDecodeDatagram(packet, length, false, &frame) != TURN_DECODE_OK);
  assert(TurnDecodeDatagram(packet, length, true, &frame) == TURN_DECODE_OK);
  assert(frame.kind == TURN_DATAGRAM_ALLOCATE && frame.policy->auth_mode == TURN_AUTH_LONG_TERM);
  assert(stun_attr_add_str(packet, &length, STUN_ATTRIBUTE_REQUESTED_TRANSPORT, udp_transport, 4));
  assert(TurnDecodeDatagram(packet, length, true, &frame) != TURN_DECODE_OK);

  stun_init_request_str(STUN_METHOD_ALLOCATE, packet, &length);
  assert(stun_attr_add_str(packet, &length, STUN_ATTRIBUTE_MESSAGE_INTEGRITY, integrity, 20));
  assert(stun_attr_add_str(packet, &length, STUN_ATTRIBUTE_REQUESTED_TRANSPORT, udp_transport, 4));
  assert(TurnDecodeDatagram(packet, length, true, &frame) != TURN_DECODE_OK);

  stun_init_request_str(STUN_METHOD_REFRESH, packet, &length);
  const size_t integrity_offset = length;
  assert(stun_attr_add_str(packet, &length, STUN_ATTRIBUTE_MESSAGE_INTEGRITY, integrity, 20));
  assert(stun_attr_add_fingerprint_str(packet, &length));
  assert(TurnDecodeDatagram(packet, length, true, &frame) == TURN_DECODE_OK);
  assert(frame.kind == TURN_DATAGRAM_REFRESH && frame.fingerprint && frame.attributes_end == integrity_offset);
  assert(TurnDecodeDatagram(packet, length - 1, true, &frame) != TURN_DECODE_OK);
  packet[length - 1] ^= 1;
  assert(TurnDecodeDatagram(packet, length, true, &frame) != TURN_DECODE_OK);

  stun_init_request_str(STUN_METHOD_CREATE_PERMISSION, packet, &length);
  assert(stun_attr_add_str(packet, &length, STUN_ATTRIBUTE_XOR_PEER_ADDRESS, peer, 8));
  assert(stun_attr_add_str(packet, &length, STUN_ATTRIBUTE_XOR_PEER_ADDRESS, peer, 8));
  assert(TurnDecodeDatagram(packet, length, true, &frame) && frame.kind == TURN_DATAGRAM_CREATE_PERMISSION);
  packet[STUN_HEADER_LENGTH + 5] = 2; // IPv6 family with an IPv4-sized value.
  assert(TurnDecodeDatagram(packet, length, true, &frame) != TURN_DECODE_OK);

  stun_init_request_str(STUN_METHOD_CHANNEL_BIND, packet, &length);
  assert(stun_attr_add_str(packet, &length, STUN_ATTRIBUTE_XOR_PEER_ADDRESS, peer, 8));
  assert(stun_attr_add_str(packet, &length, STUN_ATTRIBUTE_CHANNEL_NUMBER, channel, 4));
  assert(TurnDecodeDatagram(packet, length, true, &frame) && frame.kind == TURN_DATAGRAM_CHANNEL_BIND);
}

static void
CheckSendAndChannelData(void)
{
  uint8_t packet[128];
  const uint8_t peer[8] = {0, 1};
  const uint8_t content[] = {0x42};
  size_t length;
  TurnDatagram frame;

  stun_init_indication_str(STUN_METHOD_SEND, packet, &length);
  assert(stun_attr_add_str(packet, &length, STUN_ATTRIBUTE_XOR_PEER_ADDRESS, peer, 8));
  assert(stun_attr_add_str(packet, &length, STUN_ATTRIBUTE_DATA, content, sizeof(content)));
  assert(TurnDecodeDatagram(packet, length, true, &frame) == TURN_DECODE_OK);
  assert(frame.kind == TURN_DATAGRAM_SEND && frame.policy->auth_mode == TURN_AUTH_ALLOCATION);
  turn_write_u16(packet, STUN_METHOD_SEND); // Send as a request has no route.
  assert(TurnDecodeDatagram(packet, length, true, &frame) != TURN_DECODE_OK);

  uint8_t channel_data[] = {0x40, 0, 0, 1, 0x42, 0, 0, 0, 0};
  assert(TurnDecodeDatagram(channel_data, 5, false, &frame) != TURN_DECODE_OK);
  assert(TurnDecodeDatagram(channel_data, 5, true, &frame) == TURN_DECODE_OK);
  assert(frame.kind == TURN_DATAGRAM_CHANNEL_DATA && frame.channel == 0x4000 &&
         frame.channel_payload_length == 1 && !frame.policy);
  assert(TurnDecodeDatagram(channel_data, 8, true, &frame) == TURN_DECODE_OK); // Optional UDP alignment.
  assert(TurnDecodeDatagram(channel_data, 6, true, &frame) != TURN_DECODE_OK);
  assert(TurnDecodeDatagram(channel_data, 9, true, &frame) != TURN_DECODE_OK);
  channel_data[0] = 0x50;
  assert(TurnDecodeDatagram(channel_data, 5, true, &frame) != TURN_DECODE_OK);
}

int
main(void)
{
  CheckRequests();
  CheckSendAndChannelData();
  return 0;
}
