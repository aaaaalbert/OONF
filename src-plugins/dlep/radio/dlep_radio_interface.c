
/*
 * The olsr.org Optimized Link-State Routing daemon version 2 (olsrd2)
 * Copyright (c) 2004-2013, the olsr.org team - see HISTORY file
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in
 *   the documentation and/or other materials provided with the
 *   distribution.
 * * Neither the name of olsr.org, olsrd nor the names of its
 *   contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * Visit http://www.olsr.org for more information.
 *
 * If you find this software useful feel free to make a donation
 * to the project. For more information see the website or contact
 * the copyright holders.
 *
 */

#include "common/avl.h"
#include "common/avl_comp.h"
#include "common/netaddr.h"

#include "subsystems/oonf_class.h"
#include "subsystems/oonf_packet_socket.h"
#include "subsystems/oonf_stream_socket.h"
#include "subsystems/oonf_timer.h"

#include "dlep/dlep_iana.h"
#include "dlep/dlep_parser.h"
#include "dlep/dlep_static_data.h"
#include "dlep/dlep_writer.h"
#include "dlep/radio/dlep_radio.h"
#include "dlep/radio/dlep_radio_interface.h"
#include "dlep/radio/dlep_radio_session.h"

static void _cb_receive_udp(struct oonf_packet_socket *,
    union netaddr_socket *from, void *ptr, size_t length);

static void _handle_peer_discovery(struct dlep_radio_if *interface,
    union netaddr_socket *dst, uint8_t *buffer, struct dlep_parser_index *idx);


/* tcp objects */
static struct avl_tree _interface_tree;

static struct oonf_class _interface_class = {
  .name = "DLEP radio session",
  .size = sizeof(struct dlep_radio_if),
};

int
dlep_radio_interface_init(void) {
  if (dlep_radio_session_init()) {
    return -1;
  }

  oonf_class_add(&_interface_class);
  avl_init(&_interface_tree, avl_comp_strcasecmp, false);
  return 0;
}

void
dlep_radio_interface_cleanup(void) {
  oonf_class_remove(&_interface_class);
  dlep_radio_session_cleanup();
}

struct dlep_radio_if *
dlep_radio_get_interface(const char *ifname) {
  struct dlep_radio_if *interf;

  return avl_find_element(&_interface_tree, ifname, interf, name);
}

struct dlep_radio_if *
dlep_radio_add_interface(const char *ifname) {
  struct dlep_radio_if *interface;

  interface = dlep_radio_get_interface(ifname);
  if (interface) {
    return interface;
  }

  interface = oonf_class_malloc(&_interface_class);
  if (!interface) {
    return NULL;
  }

  /* initialize key */
  strscpy(interface->name, ifname, sizeof(interface->name));
  interface->_node.key = interface->name;

  /* add to global tree of sessions */
  avl_insert(&_interface_tree, &interface->_node);

  /* initialize discovery socket */
  interface->udp.config.user = interface;
  interface->udp.config.receive_data = _cb_receive_udp;
  oonf_packet_add_managed(&interface->udp);

  /* configure TCP server socket */
  interface->tcp.config.session_timeout = 120000; /* 120 seconds */
  interface->tcp.config.maximum_input_buffer = 4096;
  interface->tcp.config.allowed_sessions = 3;
  dlep_radio_session_initialize_tcp_callbacks(&interface->tcp.config);

  oonf_stream_add_managed(&interface->tcp);

  return interface;
}

void
dlep_radio_remove_interface(struct dlep_radio_if *interface) {
  /* cleanup discovery socket */
  oonf_packet_remove_managed(&interface->udp, true);

  /* cleanup tcp socket */
  oonf_stream_remove_managed(&interface->tcp, true);

  /* remove tcp */
  avl_remove(&_interface_tree, &interface->_node);
  oonf_class_free(&_interface_class, interface);
}

void
dlep_radio_apply_interface_settings(struct dlep_radio_if *interface) {
  oonf_packet_apply_managed(&interface->udp, &interface->udp_config);
  oonf_stream_apply_managed(&interface->tcp, &interface->tcp_config);
}

static void
_cb_receive_udp(struct oonf_packet_socket *pkt,
    union netaddr_socket *from, void *ptr, size_t length) {
  struct dlep_radio_if *session;
  struct dlep_parser_index idx;
  int signal;
  struct netaddr_str nbuf;

  session = pkt->config.user;

  if ((signal = dlep_parser_read(&idx, ptr, length, NULL)) < 0) {
    OONF_WARN_HEX(LOG_DLEP_RADIO, ptr, length,
        "Could not parse incoming UDP signal from %s: %d",
        netaddr_socket_to_string(&nbuf, from), signal);
    return;
  }

  OONF_INFO(LOG_DLEP_RADIO, "Received UDP Signal %u from %s",
      signal, netaddr_socket_to_string(&nbuf, from));

  if (signal != DLEP_PEER_DISCOVERY) {
    OONF_WARN(LOG_DLEP_RADIO,
        "Received illegal signal in UDP from %s: %u",
        netaddr_socket_to_string(&nbuf, from), signal);
    return;
  }

  _handle_peer_discovery(session, from, ptr, &idx);
}

static void
_handle_peer_discovery(struct dlep_radio_if *interface,
    union netaddr_socket *dst, uint8_t *buffer, struct dlep_parser_index *idx) {
  struct netaddr addr;
  int pos, ipv4, ipv6;
  struct netaddr_str nbuf;

  /* get heartbeat interval */
  pos = idx->idx[DLEP_HEARTBEAT_INTERVAL_TLV];
  dlep_parser_get_heartbeat_interval(
      &interface->remote_heartbeat_interval, &buffer[pos]);

  OONF_DEBUG(LOG_DLEP_RADIO, "Heartbeat interval is %"PRIu64,
      interface->remote_heartbeat_interval);

  /* create Peer Offer */
  OONF_INFO(LOG_DLEP_RADIO, "Send UDP Peer Offer to %s",
      netaddr_socket_to_string(&nbuf, dst));

  dlep_writer_start_signal(DLEP_PEER_OFFER, &dlep_mandatory_tlvs);
  dlep_writer_add_heartbeat_tlv(interface->local_heartbeat_interval);
  dlep_writer_add_port_tlv(interface->tcp_config.port);

  netaddr_from_socket(&addr, &interface->tcp.socket_v4.local_socket);
  ipv4 = dlep_writer_add_ipv4_tlv(&addr, true);

  netaddr_from_socket(&addr, &interface->tcp.socket_v6.local_socket);
  ipv6 = dlep_writer_add_ipv6_tlv(&addr, true);

  if (ipv4 != 0 && ipv6 != 0) {
    /* we did not offer any address */
    OONF_DEBUG(LOG_DLEP_RADIO, "Peer Offer without IP addresses");
    return;
  }

  if (dlep_writer_finish_signal(LOG_DLEP_RADIO)) {
    return;
  }
  dlep_writer_send_udp_unicast(&interface->udp, dst,
      &dlep_mandatory_signals, LOG_DLEP_RADIO);
}
