/*
 * rtp.c
 *
 * Copyright (C) 2009-11 - ipoque GmbH
 * Copyright (C) 2011-22 - ntop.org
 *
 * This file is part of nDPI, an open source deep packet inspection
 * library based on the OpenDPI and PACE technology by ipoque GmbH
 *
 * nDPI is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * nDPI is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with nDPI.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "ndpi_protocol_ids.h"

#define NDPI_CURRENT_PROTO NDPI_PROTOCOL_RTP

#include "ndpi_api.h"
#include "ndpi_private.h"

#define RTP_MIN_HEADER	12
#define RTCP_MIN_HEADER	8

/* https://www.iana.org/assignments/rtp-parameters/rtp-parameters.xhtml */
int is_valid_rtp_payload_type(uint8_t type)
{
  if(!(type <= 34 || (type >= 96 && type <= 127)))
    return 0;
  return 1;
}

u_int8_t rtp_get_stream_type(u_int8_t payloadType, ndpi_multimedia_flow_type *s_type)
{
  switch(payloadType) {
  case 0: /* G.711 u-Law */
  case 3: /* GSM 6.10 */
  case 4: /* G.723.1  */
  case 8: /* G.711 A-Law */
  case 9: /* G.722 */
  case 13: /* Comfort Noise */
  case 96: /* Dynamic RTP */
  case 97: /* Redundant Audio Data Payload */
  case 98: /* DynamicRTP-Type-98 (Zoom) */
  case 101: /* DTMF */
  case 103: /* SILK Narrowband */
  case 104: /* SILK Wideband */
  case 111: /* Siren */
  case 112: /* G.722.1 */
  case 114: /* RT Audio Wideband */
  case 115: /* RT Audio Narrowband */
  case 116: /* G.726 */
  case 117: /* G.722 */
  case 118: /* Comfort Noise Wideband */
    *s_type = ndpi_multimedia_audio_flow;
    return(1);
    
  case 34: /* H.263 [MS-H26XPF] */
  case 121: /* RT Video */
  case 122: /* H.264 [MS-H264PF] */
  case 123: /* H.264 FEC [MS-H264PF] */
  case 127: /* x-data */
    *s_type = ndpi_multimedia_video_flow;
    return(1);

  default:
    *s_type = ndpi_multimedia_unknown_flow;
    return(0);
  }
}

static int is_valid_rtcp_payload_type(uint8_t type) {
  return (type >= 192 && type <= 213);
}

int is_rtp_or_rtcp(struct ndpi_detection_module_struct *ndpi_struct, u_int16_t *seq)
{
  struct ndpi_packet_struct *packet = &ndpi_struct->packet;
  u_int8_t csrc_count, ext_header;
  u_int16_t ext_len;
  u_int32_t min_len;
  const u_int8_t *payload = packet->payload;
  const u_int16_t payload_len = packet->payload_packet_len;

  if(payload_len < 2)
    return NO_RTP_RTCP;

  if((payload[0] & 0xC0) != 0x80) { /* Version 2 */
    NDPI_LOG_DBG(ndpi_struct, "Not version 2\n");
    return NO_RTP_RTCP;
  }

  if(is_valid_rtp_payload_type(payload[1] & 0x7F) &&
     payload_len >= RTP_MIN_HEADER) {
    /* RTP */
    csrc_count = payload[0] & 0x0F;
    ext_header =  !!(payload[0] & 0x10);
    min_len = RTP_MIN_HEADER + 4 * csrc_count + 4 * ext_header;
    if(ext_header) {
      if(min_len > payload_len) {
        NDPI_LOG_DBG(ndpi_struct, "Too short (a) %d vs %d\n", min_len, payload_len);
        return NO_RTP_RTCP;
      }
      ext_len = ntohs(*(unsigned short *)&payload[min_len - 2]);
      min_len += ext_len * 4;
    }
    if(min_len > payload_len) {
      NDPI_LOG_DBG(ndpi_struct, "Too short (b) %d vs %d\n", min_len, payload_len);
      return NO_RTP_RTCP;
    }
    /* Check on padding doesn't work because:
       * we may have multiple RTP packets in the same TCP/UDP datagram
       * with SRTP, padding_length field is encrypted */
    if(seq)
      *seq = ntohs(*(unsigned short *)&payload[2]);
    return IS_RTP;
  } else if(is_valid_rtcp_payload_type(payload[1]) &&
            payload_len >= RTCP_MIN_HEADER) {
    min_len = (ntohs(*(unsigned short *)&payload[2]) + 1) * 4;
    if(min_len > payload_len) {
      NDPI_LOG_DBG(ndpi_struct, "Too short (c) %d vs %d\n", min_len, payload_len);
      return NO_RTP_RTCP;
    }
    return IS_RTCP;
  }
  NDPI_LOG_DBG(ndpi_struct, "not RTP/RTCP\n");
  return NO_RTP_RTCP;
}

/* *************************************************************** */

static void ndpi_rtp_search(struct ndpi_detection_module_struct *ndpi_struct,
			    struct ndpi_flow_struct *flow) {
  u_int8_t is_rtp;
  u_int16_t d_port = ntohs(ndpi_struct->packet.udp->dest);
  struct ndpi_packet_struct *packet = &ndpi_struct->packet;
  const u_int8_t *payload = packet->payload;
  u_int16_t seq;

  NDPI_LOG_DBG(ndpi_struct, "search RTP (stage %d/%d)\n", flow->l4.udp.rtp_stage, flow->l4.udp.rtcp_stage);

  if(d_port == 5355 || /* LLMNR_PORT */
     d_port == 5353 || /* MDNS_PORT */
     d_port == 9600    /* FINS_PORT */) {
    NDPI_EXCLUDE_PROTO(ndpi_struct, flow);
    NDPI_EXCLUDE_PROTO_EXT(ndpi_struct, flow, NDPI_PROTOCOL_RTCP);
    return;
  }

  /* * Let some "unknown" packets at the beginning:
     * search for 3/4 consecutive RTP/RTCP packets.
     * Wait a little longer (4 vs 3 pkts) for RTCP to try to tell if there are only
     * RTCP packets in the flow or if RTP/RTCP are multiplexed together */

  if(flow->packet_counter > 3 &&
     flow->l4.udp.rtp_stage == 0 &&
     flow->l4.udp.rtcp_stage == 0) {
    NDPI_EXCLUDE_PROTO(ndpi_struct, flow);
    NDPI_EXCLUDE_PROTO_EXT(ndpi_struct, flow, NDPI_PROTOCOL_RTCP);
    return;
  }

  is_rtp = is_rtp_or_rtcp(ndpi_struct, &seq);

  if(is_rtp == IS_RTP) {
    if(flow->l4.udp.rtp_stage == 2) {
      if(flow->l4.udp.line_pkts[0] >= 2 && flow->l4.udp.line_pkts[1] >= 2) {
        /* It seems that it is a LINE stuff; let its dissector to evaluate */
      } else if(flow->l4.udp.epicgames_stage > 0) {
        /* It seems that it is a EpicGames stuff; let its dissector to evaluate */
      } else if(flow->l4.udp.rtp_seq_set[packet->packet_direction] &&
                flow->l4.udp.rtp_seq[packet->packet_direction] == seq) {
        /* Simple heuristic to avoid false positives. tradeoff between:
	   * consecutive RTP packets should have different sequence number
	   * we should handle duplicated traffic */
        NDPI_LOG_DBG(ndpi_struct, "Same seq on consecutive pkts\n");
        flow->l4.udp.rtp_stage = 0;
        flow->l4.udp.rtcp_stage = 0;
        NDPI_EXCLUDE_PROTO(ndpi_struct, flow);
        NDPI_EXCLUDE_PROTO_EXT(ndpi_struct, flow, NDPI_PROTOCOL_RTCP);
      } else {
        rtp_get_stream_type(payload[1] & 0x7F, &flow->flow_multimedia_type);

        NDPI_LOG_INFO(ndpi_struct, "Found RTP\n");
        ndpi_set_detected_protocol(ndpi_struct, flow,
                                   NDPI_PROTOCOL_UNKNOWN, NDPI_PROTOCOL_RTP,
                                   NDPI_CONFIDENCE_DPI);
      }
      return;
    }
    if(flow->l4.udp.rtp_stage == 0) {
      flow->l4.udp.rtp_seq[packet->packet_direction] = seq;
      flow->l4.udp.rtp_seq_set[packet->packet_direction] = 1;
    }
    flow->l4.udp.rtp_stage += 1;
  } else if(is_rtp == IS_RTCP && flow->l4.udp.rtp_stage > 0) {
    /* RTCP after (some) RTP. Keep looking for RTP */
  } else if(is_rtp == IS_RTCP && flow->l4.udp.rtp_stage == 0) {
    if(flow->l4.udp.rtcp_stage == 3) {
      NDPI_LOG_INFO(ndpi_struct, "Found RTCP\n");
      ndpi_set_detected_protocol(ndpi_struct, flow,
                                 NDPI_PROTOCOL_UNKNOWN, NDPI_PROTOCOL_RTCP,
                                 NDPI_CONFIDENCE_DPI);
      return;
    }
    flow->l4.udp.rtcp_stage += 1;
  } else {
    if(flow->l4.udp.rtp_stage || flow->l4.udp.rtcp_stage) {
      u_int16_t app_proto; /* unused */
      u_int32_t unused;

      /* TODO: we should switch to the demultiplexing-code in stun dissector */
      if(is_stun(ndpi_struct, flow, &app_proto) != 0 &&
         !is_dtls(packet->payload, packet->payload_packet_len, &unused)) {
        flow->l4.udp.rtp_stage = 0;
        flow->l4.udp.rtcp_stage = 0;
        NDPI_EXCLUDE_PROTO(ndpi_struct, flow);
        NDPI_EXCLUDE_PROTO_EXT(ndpi_struct, flow, NDPI_PROTOCOL_RTCP);
      }
    } else if(flow->packet_counter > 3) {
        NDPI_EXCLUDE_PROTO(ndpi_struct, flow);
        NDPI_EXCLUDE_PROTO_EXT(ndpi_struct, flow, NDPI_PROTOCOL_RTCP);
    }      
  }
}

/* *************************************************************** */

static void ndpi_search_rtp(struct ndpi_detection_module_struct *ndpi_struct, struct ndpi_flow_struct *flow)
{
  struct ndpi_packet_struct *packet = &ndpi_struct->packet;
  u_int16_t source = ntohs(packet->udp->source);
  u_int16_t dest = ntohs(packet->udp->dest);

  if((source != 30303) && (dest != 30303 /* Avoid to mix it with Ethereum that looks alike */)
     && (dest > 1023)
     )
    ndpi_rtp_search(ndpi_struct, flow);
  else {
    NDPI_EXCLUDE_PROTO(ndpi_struct, flow);
    NDPI_EXCLUDE_PROTO_EXT(ndpi_struct, flow, NDPI_PROTOCOL_RTCP);
  }
}

/* *************************************************************** */

void init_rtp_dissector(struct ndpi_detection_module_struct *ndpi_struct,
			u_int32_t *id) {
  ndpi_set_bitmask_protocol_detection("RTP", ndpi_struct, *id,
				      NDPI_PROTOCOL_RTP,
				      ndpi_search_rtp,
				      NDPI_SELECTION_BITMASK_PROTOCOL_V4_V6_UDP_WITH_PAYLOAD,
				      SAVE_DETECTION_BITMASK_AS_UNKNOWN,
				      ADD_TO_DETECTION_BITMASK);

  *id += 1;
}
