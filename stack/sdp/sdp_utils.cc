/******************************************************************************
 *
 *  Copyright 1999-2012 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/******************************************************************************
 *
 *  This file contains SDP utility functions
 *
 ******************************************************************************/

#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utility>
#include <vector>

#include "bt_common.h"
#include "bt_types.h"

#include "hcidefs.h"
#include "hcimsgs.h"
#include "l2cdefs.h"

#include "sdp_api.h"
#include "sdpint.h"

#include "btu.h"
#include "common/metrics.h"

using bluetooth::Uuid;
static const uint8_t sdp_base_uuid[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                        0x10, 0x00, 0x80, 0x00, 0x00, 0x80,
                                        0x5F, 0x9B, 0x34, 0xFB};

template <typename T>
static std::array<char, sizeof(T)> to_little_endian_array(T x) {
  static_assert(std::is_integral<T>::value,
                "to_little_endian_array parameter must be integral.");
  std::array<char, sizeof(T)> array = {};
  for (size_t i = 0; i < array.size(); i++) {
    array[i] = static_cast<char>((x >> (8 * i)) & 0xFF);
  }
  return array;
}

/**
 * Find the list of profile versions from Bluetooth Profile Descriptor list
 * attribute in a SDP record
 *
 * @param p_rec SDP record to search
 * @return a vector of <UUID, VERSION> pairs, empty if not found
 */
static std::vector<std::pair<uint16_t, uint16_t>> sdpu_find_profile_version(
    tSDP_DISC_REC* p_rec) {
  std::vector<std::pair<uint16_t, uint16_t>> result;
  for (tSDP_DISC_ATTR* p_attr = p_rec->p_first_attr; p_attr != nullptr;
       p_attr = p_attr->p_next_attr) {
    // Find the profile descriptor list */
    if (p_attr->attr_id != ATTR_ID_BT_PROFILE_DESC_LIST ||
        SDP_DISC_ATTR_TYPE(p_attr->attr_len_type) != DATA_ELE_SEQ_DESC_TYPE) {
      continue;
    }
    // Walk through the protocol descriptor list
    for (tSDP_DISC_ATTR* p_sattr = p_attr->attr_value.v.p_sub_attr;
         p_sattr != nullptr; p_sattr = p_sattr->p_next_attr) {
      // Safety check - each entry should itself be a sequence
      if (SDP_DISC_ATTR_TYPE(p_sattr->attr_len_type) !=
          DATA_ELE_SEQ_DESC_TYPE) {
        LOG(WARNING) << __func__ << ": Descriptor type is not sequence: "
                     << loghex(SDP_DISC_ATTR_TYPE(p_sattr->attr_len_type));
        return std::vector<std::pair<uint16_t, uint16_t>>();
      }
      // Now, see if the entry contains the profile UUID we are interested in
      for (tSDP_DISC_ATTR* p_ssattr = p_sattr->attr_value.v.p_sub_attr;
           p_ssattr != nullptr; p_ssattr = p_ssattr->p_next_attr) {
        if (SDP_DISC_ATTR_TYPE(p_ssattr->attr_len_type) != UUID_DESC_TYPE ||
            SDP_DISC_ATTR_LEN(p_ssattr->attr_len_type) != 2) {
          continue;
        }
        uint16_t uuid = p_ssattr->attr_value.v.u16;
        // Next attribute should be the version attribute
        tSDP_DISC_ATTR* version_attr = p_ssattr->p_next_attr;
        if (SDP_DISC_ATTR_TYPE(version_attr->attr_len_type) != UINT_DESC_TYPE ||
            SDP_DISC_ATTR_LEN(version_attr->attr_len_type) != 2) {
          LOG(WARNING) << __func__ << ": Bad version type "
                       << loghex(
                              SDP_DISC_ATTR_TYPE(version_attr->attr_len_type))
                       << ", or length "
                       << SDP_DISC_ATTR_LEN(version_attr->attr_len_type);
          return std::vector<std::pair<uint16_t, uint16_t>>();
        }
        // High order 8 bits is the major number, low order is the
        // minor number (big endian)
        uint16_t version = version_attr->attr_value.v.u16;
        result.emplace_back(uuid, version);
      }
    }
  }
  return result;
}

/**
 * Find the most specific 16-bit service uuid represented by a SDP record
 *
 * @param p_rec pointer to a SDP record
 * @return most specific 16-bit service uuid, 0 if not found
 */
static uint16_t sdpu_find_most_specific_service_uuid(tSDP_DISC_REC* p_rec) {
  for (tSDP_DISC_ATTR* p_attr = p_rec->p_first_attr; p_attr != nullptr;
       p_attr = p_attr->p_next_attr) {
    if (p_attr->attr_id == ATTR_ID_SERVICE_CLASS_ID_LIST &&
        SDP_DISC_ATTR_TYPE(p_attr->attr_len_type) == DATA_ELE_SEQ_DESC_TYPE) {
      tSDP_DISC_ATTR* p_first_attr = p_attr->attr_value.v.p_sub_attr;
      if (SDP_DISC_ATTR_TYPE(p_first_attr->attr_len_type) == UUID_DESC_TYPE &&
          SDP_DISC_ATTR_LEN(p_first_attr->attr_len_type) == 2) {
        return p_first_attr->attr_value.v.u16;
      } else if (SDP_DISC_ATTR_TYPE(p_first_attr->attr_len_type) ==
                 DATA_ELE_SEQ_DESC_TYPE) {
        // Workaround for Toyota G Block car kit:
        // It incorrectly puts an extra data element sequence in this attribute
        for (tSDP_DISC_ATTR* p_extra_sattr =
                 p_first_attr->attr_value.v.p_sub_attr;
             p_extra_sattr != nullptr;
             p_extra_sattr = p_extra_sattr->p_next_attr) {
          // Return the first UUID data element
          if (SDP_DISC_ATTR_TYPE(p_extra_sattr->attr_len_type) ==
                  UUID_DESC_TYPE &&
              SDP_DISC_ATTR_LEN(p_extra_sattr->attr_len_type) == 2) {
            return p_extra_sattr->attr_value.v.u16;
          }
        }
      } else {
        LOG(WARNING) << __func__ << ": Bad Service Class ID list attribute";
        return 0;
      }
    } else if (p_attr->attr_id == ATTR_ID_SERVICE_ID) {
      if (SDP_DISC_ATTR_TYPE(p_attr->attr_len_type) == UUID_DESC_TYPE &&
          SDP_DISC_ATTR_LEN(p_attr->attr_len_type) == 2) {
        return p_attr->attr_value.v.u16;
      }
    }
  }
  return 0;
}

void sdpu_log_attribute_metrics(const RawAddress& bda,
                                tSDP_DISCOVERY_DB* p_db) {
  CHECK_NE(p_db, nullptr);
  bool has_di_record = false;
  for (tSDP_DISC_REC* p_rec = p_db->p_first_rec; p_rec != nullptr;
       p_rec = p_rec->p_next_rec) {
    uint16_t service_uuid = sdpu_find_most_specific_service_uuid(p_rec);
    if (service_uuid == 0) {
      LOG(INFO) << __func__ << ": skipping record without service uuid " << bda;
      continue;
    }
    // Log the existence of a profile role
    // This can be different from Bluetooth Profile Descriptor List
    bluetooth::common::LogSdpAttribute(bda, service_uuid, 0, 0, nullptr);
    // Log profile version from Bluetooth Profile Descriptor List
    auto uuid_version_array = sdpu_find_profile_version(p_rec);
    for (const auto& uuid_version_pair : uuid_version_array) {
      uint16_t profile_uuid = uuid_version_pair.first;
      uint16_t version = uuid_version_pair.second;
      auto version_array = to_little_endian_array(version);
      bluetooth::common::LogSdpAttribute(
          bda, profile_uuid, ATTR_ID_BT_PROFILE_DESC_LIST, version_array.size(),
          version_array.data());
    }
    // Log protocol version from Protocol Descriptor List
    uint16_t protocol_uuid = 0;
    switch (service_uuid) {
      case UUID_SERVCLASS_AUDIO_SOURCE:
      case UUID_SERVCLASS_AUDIO_SINK:
        protocol_uuid = UUID_PROTOCOL_AVDTP;
        break;
      case UUID_SERVCLASS_AV_REMOTE_CONTROL:
      case UUID_SERVCLASS_AV_REM_CTRL_CONTROL:
      case UUID_SERVCLASS_AV_REM_CTRL_TARGET:
        protocol_uuid = UUID_PROTOCOL_AVCTP;
        break;
      case UUID_SERVCLASS_PANU:
      case UUID_SERVCLASS_GN:
        protocol_uuid = UUID_PROTOCOL_BNEP;
        break;
    }
    if (protocol_uuid != 0) {
      tSDP_PROTOCOL_ELEM protocol_elements = {};
      if (SDP_FindProtocolListElemInRec(p_rec, protocol_uuid,
                                        &protocol_elements)) {
        if (protocol_elements.num_params >= 1) {
          uint16_t version = protocol_elements.params[0];
          auto version_array = to_little_endian_array(version);
          bluetooth::common::LogSdpAttribute(
              bda, protocol_uuid, ATTR_ID_PROTOCOL_DESC_LIST,
              version_array.size(), version_array.data());
        }
      }
    }
    // Log profile supported features from various supported feature attributes
    switch (service_uuid) {
      case UUID_SERVCLASS_AG_HANDSFREE:
      case UUID_SERVCLASS_HF_HANDSFREE:
      case UUID_SERVCLASS_AV_REMOTE_CONTROL:
      case UUID_SERVCLASS_AV_REM_CTRL_CONTROL:
      case UUID_SERVCLASS_AV_REM_CTRL_TARGET:
      case UUID_SERVCLASS_AUDIO_SOURCE:
      case UUID_SERVCLASS_AUDIO_SINK: {
        tSDP_DISC_ATTR* p_attr =
            SDP_FindAttributeInRec(p_rec, ATTR_ID_SUPPORTED_FEATURES);
        if (p_attr == nullptr) {
          break;
        }
        uint16_t supported_features = p_attr->attr_value.v.u16;
        auto version_array = to_little_endian_array(supported_features);
        bluetooth::common::LogSdpAttribute(
            bda, service_uuid, ATTR_ID_SUPPORTED_FEATURES, version_array.size(),
            version_array.data());
        break;
      }
      case UUID_SERVCLASS_MESSAGE_NOTIFICATION:
      case UUID_SERVCLASS_MESSAGE_ACCESS: {
        tSDP_DISC_ATTR* p_attr =
            SDP_FindAttributeInRec(p_rec, ATTR_ID_MAP_SUPPORTED_FEATURES);
        if (p_attr == nullptr) {
          break;
        }
        uint32_t map_supported_features = p_attr->attr_value.v.u32;
        auto features_array = to_little_endian_array(map_supported_features);
        bluetooth::common::LogSdpAttribute(
            bda, service_uuid, ATTR_ID_MAP_SUPPORTED_FEATURES,
            features_array.size(), features_array.data());
        break;
      }
      case UUID_SERVCLASS_PBAP_PCE:
      case UUID_SERVCLASS_PBAP_PSE: {
        tSDP_DISC_ATTR* p_attr =
            SDP_FindAttributeInRec(p_rec, ATTR_ID_PBAP_SUPPORTED_FEATURES);
        if (p_attr == nullptr) {
          break;
        }
        uint32_t pbap_supported_features = p_attr->attr_value.v.u32;
        auto features_array = to_little_endian_array(pbap_supported_features);
        bluetooth::common::LogSdpAttribute(
            bda, service_uuid, ATTR_ID_PBAP_SUPPORTED_FEATURES,
            features_array.size(), features_array.data());
        break;
      }
    }
    if (service_uuid == UUID_SERVCLASS_PNP_INFORMATION) {
      has_di_record = true;
    }
  }
  // Log the first DI record if there is one
  if (has_di_record) {
    tSDP_DI_GET_RECORD di_record = {};
    if (SDP_GetDiRecord(1, &di_record, p_db) == SDP_SUCCESS) {
      auto version_array = to_little_endian_array(di_record.spec_id);
      bluetooth::common::LogSdpAttribute(
          bda, UUID_SERVCLASS_PNP_INFORMATION, ATTR_ID_SPECIFICATION_ID,
          version_array.size(), version_array.data());
      std::stringstream ss;
      // [N - native]::SDP::[DIP - Device ID Profile]
      ss << "N:SDP::DIP::" << loghex(di_record.rec.vendor_id_source);
      bluetooth::common::LogManufacturerInfo(
          bda, android::bluetooth::DeviceInfoSrcEnum::DEVICE_INFO_INTERNAL,
          ss.str(), loghex(di_record.rec.vendor), loghex(di_record.rec.product),
          loghex(di_record.rec.version), "");
    }
  }
}

/*******************************************************************************
 *
 * Function         sdpu_find_ccb_by_cid
 *
 * Description      This function searches the CCB table for an entry with the
 *                  passed CID.
 *
 * Returns          the CCB address, or NULL if not found.
 *
 ******************************************************************************/
tCONN_CB* sdpu_find_ccb_by_cid(uint16_t cid) {
  uint16_t xx;
  tCONN_CB* p_ccb;

  /* Look through each connection control block */
  for (xx = 0, p_ccb = sdp_cb.ccb; xx < SDP_MAX_CONNECTIONS; xx++, p_ccb++) {
    if ((p_ccb->con_state != SDP_STATE_IDLE) && (p_ccb->connection_id == cid))
      return (p_ccb);
  }

  /* If here, not found */
  return (NULL);
}

/*******************************************************************************
 *
 * Function         sdpu_find_ccb_by_db
 *
 * Description      This function searches the CCB table for an entry with the
 *                  passed discovery db.
 *
 * Returns          the CCB address, or NULL if not found.
 *
 ******************************************************************************/
tCONN_CB* sdpu_find_ccb_by_db(tSDP_DISCOVERY_DB* p_db) {
  uint16_t xx;
  tCONN_CB* p_ccb;

  if (p_db) {
    /* Look through each connection control block */
    for (xx = 0, p_ccb = sdp_cb.ccb; xx < SDP_MAX_CONNECTIONS; xx++, p_ccb++) {
      if ((p_ccb->con_state != SDP_STATE_IDLE) && (p_ccb->p_db == p_db))
        return (p_ccb);
    }
  }
  /* If here, not found */
  return (NULL);
}

/*******************************************************************************
 *
 * Function         sdpu_allocate_ccb
 *
 * Description      This function allocates a new CCB.
 *
 * Returns          CCB address, or NULL if none available.
 *
 ******************************************************************************/
tCONN_CB* sdpu_allocate_ccb(void) {
  uint16_t xx;
  tCONN_CB* p_ccb;

  /* Look through each connection control block for a free one */
  for (xx = 0, p_ccb = sdp_cb.ccb; xx < SDP_MAX_CONNECTIONS; xx++, p_ccb++) {
    if (p_ccb->con_state == SDP_STATE_IDLE) {
      alarm_t* alarm = p_ccb->sdp_conn_timer;
      memset(p_ccb, 0, sizeof(tCONN_CB));
      p_ccb->sdp_conn_timer = alarm;
      return (p_ccb);
    }
  }

  /* If here, no free CCB found */
  return (NULL);
}

/*******************************************************************************
 *
 * Function         sdpu_release_ccb
 *
 * Description      This function releases a CCB.
 *
 * Returns          void
 *
 ******************************************************************************/
void sdpu_release_ccb(tCONN_CB* p_ccb) {
  /* Ensure timer is stopped */
  alarm_cancel(p_ccb->sdp_conn_timer);

  /* Drop any response pointer we may be holding */
  p_ccb->con_state = SDP_STATE_IDLE;
  p_ccb->is_attr_search = false;

  /* Free the response buffer */
  if (p_ccb->rsp_list) SDP_TRACE_DEBUG("releasing SDP rsp_list");
  osi_free_and_reset((void**)&p_ccb->rsp_list);
}

/*******************************************************************************
 *
 * Function         sdpu_build_attrib_seq
 *
 * Description      This function builds an attribute sequence from the list of
 *                  passed attributes. It is also passed the address of the
 *                  output buffer.
 *
 * Returns          Pointer to next byte in the output buffer.
 *
 ******************************************************************************/
uint8_t* sdpu_build_attrib_seq(uint8_t* p_out, uint16_t* p_attr,
                               uint16_t num_attrs) {
  uint16_t xx;

  /* First thing is the data element header. See if the length fits 1 byte */
  /* If no attributes, assume a 4-byte wildcard */
  if (!p_attr)
    xx = 5;
  else
    xx = num_attrs * 3;

  if (xx > 255) {
    UINT8_TO_BE_STREAM(p_out,
                       (DATA_ELE_SEQ_DESC_TYPE << 3) | SIZE_IN_NEXT_WORD);
    UINT16_TO_BE_STREAM(p_out, xx);
  } else {
    UINT8_TO_BE_STREAM(p_out,
                       (DATA_ELE_SEQ_DESC_TYPE << 3) | SIZE_IN_NEXT_BYTE);
    UINT8_TO_BE_STREAM(p_out, xx);
  }

  /* If there are no attributes specified, assume caller wants wildcard */
  if (!p_attr) {
    UINT8_TO_BE_STREAM(p_out, (UINT_DESC_TYPE << 3) | SIZE_FOUR_BYTES);
    UINT16_TO_BE_STREAM(p_out, 0);
    UINT16_TO_BE_STREAM(p_out, 0xFFFF);
  } else {
    /* Loop through and put in all the attributes(s) */
    for (xx = 0; xx < num_attrs; xx++, p_attr++) {
      UINT8_TO_BE_STREAM(p_out, (UINT_DESC_TYPE << 3) | SIZE_TWO_BYTES);
      UINT16_TO_BE_STREAM(p_out, *p_attr);
    }
  }

  return (p_out);
}

/*******************************************************************************
 *
 * Function         sdpu_build_attrib_entry
 *
 * Description      This function builds an attribute entry from the passed
 *                  attribute record. It is also passed the address of the
 *                  output buffer.
 *
 * Returns          Pointer to next byte in the output buffer.
 *
 ******************************************************************************/
uint8_t* sdpu_build_attrib_entry(uint8_t* p_out, tSDP_ATTRIBUTE* p_attr) {
  /* First, store the attribute ID. Goes as a UINT */
  UINT8_TO_BE_STREAM(p_out, (UINT_DESC_TYPE << 3) | SIZE_TWO_BYTES);
  UINT16_TO_BE_STREAM(p_out, p_attr->id);

  /* the attribute is in the db record.
   * assuming the attribute len is less than SDP_MAX_ATTR_LEN */
  switch (p_attr->type) {
    case TEXT_STR_DESC_TYPE:     /* 4 */
    case DATA_ELE_SEQ_DESC_TYPE: /* 6 */
    case DATA_ELE_ALT_DESC_TYPE: /* 7 */
    case URL_DESC_TYPE:          /* 8 */
#if (SDP_MAX_ATTR_LEN > 0xFFFF)
      if (p_attr->len > 0xFFFF) {
        UINT8_TO_BE_STREAM(p_out, (p_attr->type << 3) | SIZE_IN_NEXT_LONG);
        UINT32_TO_BE_STREAM(p_out, p_attr->len);
      } else
#endif /* 0xFFFF - 0xFF */
#if (SDP_MAX_ATTR_LEN > 0xFF)
          if (p_attr->len > 0xFF) {
        UINT8_TO_BE_STREAM(p_out, (p_attr->type << 3) | SIZE_IN_NEXT_WORD);
        UINT16_TO_BE_STREAM(p_out, p_attr->len);
      } else
#endif /* 0xFF and less*/
      {
        UINT8_TO_BE_STREAM(p_out, (p_attr->type << 3) | SIZE_IN_NEXT_BYTE);
        UINT8_TO_BE_STREAM(p_out, p_attr->len);
      }

      if (p_attr->value_ptr != NULL) {
        ARRAY_TO_BE_STREAM(p_out, p_attr->value_ptr, (int)p_attr->len);
      }

      return (p_out);
  }

  /* Now, store the attribute value */
  switch (p_attr->len) {
    case 1:
      UINT8_TO_BE_STREAM(p_out, (p_attr->type << 3) | SIZE_ONE_BYTE);
      break;
    case 2:
      UINT8_TO_BE_STREAM(p_out, (p_attr->type << 3) | SIZE_TWO_BYTES);
      break;
    case 4:
      UINT8_TO_BE_STREAM(p_out, (p_attr->type << 3) | SIZE_FOUR_BYTES);
      break;
    case 8:
      UINT8_TO_BE_STREAM(p_out, (p_attr->type << 3) | SIZE_EIGHT_BYTES);
      break;
    case 16:
      UINT8_TO_BE_STREAM(p_out, (p_attr->type << 3) | SIZE_SIXTEEN_BYTES);
      break;
    default:
      UINT8_TO_BE_STREAM(p_out, (p_attr->type << 3) | SIZE_IN_NEXT_BYTE);
      UINT8_TO_BE_STREAM(p_out, p_attr->len);
      break;
  }

  if (p_attr->value_ptr != NULL) {
    ARRAY_TO_BE_STREAM(p_out, p_attr->value_ptr, (int)p_attr->len);
  }

  return (p_out);
}

/*******************************************************************************
 *
 * Function         sdpu_build_n_send_error
 *
 * Description      This function builds and sends an error packet.
 *
 * Returns          void
 *
 ******************************************************************************/
void sdpu_build_n_send_error(tCONN_CB* p_ccb, uint16_t trans_num,
                             uint16_t error_code, char* p_error_text) {
  uint8_t *p_rsp, *p_rsp_start, *p_rsp_param_len;
  uint16_t rsp_param_len;
  BT_HDR* p_buf = (BT_HDR*)osi_malloc(SDP_DATA_BUF_SIZE);

  SDP_TRACE_WARNING("SDP - sdpu_build_n_send_error  code: 0x%x  CID: 0x%x",
                    error_code, p_ccb->connection_id);

  /* Send the packet to L2CAP */
  p_buf->offset = L2CAP_MIN_OFFSET;
  p_rsp = p_rsp_start = (uint8_t*)(p_buf + 1) + L2CAP_MIN_OFFSET;

  UINT8_TO_BE_STREAM(p_rsp, SDP_PDU_ERROR_RESPONSE);
  UINT16_TO_BE_STREAM(p_rsp, trans_num);

  /* Skip the parameter length, we need to add it at the end */
  p_rsp_param_len = p_rsp;
  p_rsp += 2;

  UINT16_TO_BE_STREAM(p_rsp, error_code);

  /* Unplugfest example traces do not have any error text */
  if (p_error_text)
    ARRAY_TO_BE_STREAM(p_rsp, p_error_text, (int)strlen(p_error_text));

  /* Go back and put the parameter length into the buffer */
  rsp_param_len = p_rsp - p_rsp_param_len - 2;
  UINT16_TO_BE_STREAM(p_rsp_param_len, rsp_param_len);

  /* Set the length of the SDP data in the buffer */
  p_buf->len = p_rsp - p_rsp_start;

  /* Send the buffer through L2CAP */
  L2CA_DataWrite(p_ccb->connection_id, p_buf);
}

/*******************************************************************************
 *
 * Function         sdpu_extract_uid_seq
 *
 * Description      This function extracts a UUID sequence from the passed input
 *                  buffer, and puts it into the passed output list.
 *
 * Returns          Pointer to next byte in the input buffer after the sequence.
 *
 ******************************************************************************/
uint8_t* sdpu_extract_uid_seq(uint8_t* p, uint16_t param_len,
                              tSDP_UUID_SEQ* p_seq) {
  uint8_t* p_seq_end;
  uint8_t descr, type, size;
  uint32_t seq_len, uuid_len;

  /* Assume none found */
  p_seq->num_uids = 0;

  /* A UID sequence is composed of a bunch of UIDs. */
  if (sizeof(descr) > param_len) return (NULL);
  param_len -= sizeof(descr);

  BE_STREAM_TO_UINT8(descr, p);
  type = descr >> 3;
  size = descr & 7;

  if (type != DATA_ELE_SEQ_DESC_TYPE) return (NULL);

  switch (size) {
    case SIZE_TWO_BYTES:
      seq_len = 2;
      break;
    case SIZE_FOUR_BYTES:
      seq_len = 4;
      break;
    case SIZE_SIXTEEN_BYTES:
      seq_len = 16;
      break;
    case SIZE_IN_NEXT_BYTE:
      if (sizeof(uint8_t) > param_len) return (NULL);
      param_len -= sizeof(uint8_t);
      BE_STREAM_TO_UINT8(seq_len, p);
      break;
    case SIZE_IN_NEXT_WORD:
      if (sizeof(uint16_t) > param_len) return (NULL);
      param_len -= sizeof(uint16_t);
      BE_STREAM_TO_UINT16(seq_len, p);
      break;
    case SIZE_IN_NEXT_LONG:
      if (sizeof(uint32_t) > param_len) return (NULL);
      param_len -= sizeof(uint32_t);
      BE_STREAM_TO_UINT32(seq_len, p);
      break;
    default:
      return (NULL);
  }

  if (seq_len > param_len) return (NULL);

  p_seq_end = p + seq_len;

  /* Loop through, extracting the UIDs */
  for (; p < p_seq_end;) {
    BE_STREAM_TO_UINT8(descr, p);
    type = descr >> 3;
    size = descr & 7;

    if (type != UUID_DESC_TYPE) return (NULL);

    switch (size) {
      case SIZE_TWO_BYTES:
        uuid_len = 2;
        break;
      case SIZE_FOUR_BYTES:
        uuid_len = 4;
        break;
      case SIZE_SIXTEEN_BYTES:
        uuid_len = 16;
        break;
      case SIZE_IN_NEXT_BYTE:
        if (p + sizeof(uint8_t) > p_seq_end) return NULL;
        BE_STREAM_TO_UINT8(uuid_len, p);
        break;
      case SIZE_IN_NEXT_WORD:
        if (p + sizeof(uint16_t) > p_seq_end) return NULL;
        BE_STREAM_TO_UINT16(uuid_len, p);
        break;
      case SIZE_IN_NEXT_LONG:
        if (p + sizeof(uint32_t) > p_seq_end) return NULL;
        BE_STREAM_TO_UINT32(uuid_len, p);
        break;
      default:
        return (NULL);
    }

    /* If UUID length is valid, copy it across */
    if (((uuid_len == 2) || (uuid_len == 4) || (uuid_len == 16)) &&
        (p + uuid_len <= p_seq_end)) {
      p_seq->uuid_entry[p_seq->num_uids].len = (uint16_t)uuid_len;
      BE_STREAM_TO_ARRAY(p, p_seq->uuid_entry[p_seq->num_uids].value,
                         (int)uuid_len);
      p_seq->num_uids++;
    } else
      return (NULL);

    /* We can only do so many */
    if (p_seq->num_uids >= MAX_UUIDS_PER_SEQ) return (NULL);
  }

  if (p != p_seq_end) return (NULL);

  return (p);
}

/*******************************************************************************
 *
 * Function         sdpu_extract_attr_seq
 *
 * Description      This function extracts an attribute sequence from the passed
 *                  input buffer, and puts it into the passed output list.
 *
 * Returns          Pointer to next byte in the input buffer after the sequence.
 *
 ******************************************************************************/
uint8_t* sdpu_extract_attr_seq(uint8_t* p, uint16_t param_len,
                               tSDP_ATTR_SEQ* p_seq) {
  uint8_t* p_end_list;
  uint8_t descr, type, size;
  uint32_t list_len, attr_len;

  /* Assume none found */
  p_seq->num_attr = 0;

  /* Get attribute sequence info */
  if (param_len < sizeof(descr)) return NULL;
  param_len -= sizeof(descr);
  BE_STREAM_TO_UINT8(descr, p);
  type = descr >> 3;
  size = descr & 7;

  if (type != DATA_ELE_SEQ_DESC_TYPE) return NULL;

  switch (size) {
    case SIZE_IN_NEXT_BYTE:
      if (param_len < sizeof(uint8_t)) return NULL;
      param_len -= sizeof(uint8_t);
      BE_STREAM_TO_UINT8(list_len, p);
      break;

    case SIZE_IN_NEXT_WORD:
      if (param_len < sizeof(uint16_t)) return NULL;
      param_len -= sizeof(uint16_t);
      BE_STREAM_TO_UINT16(list_len, p);
      break;

    case SIZE_IN_NEXT_LONG:
      if (param_len < sizeof(uint32_t)) return NULL;
      param_len -= sizeof(uint32_t);
      BE_STREAM_TO_UINT32(list_len, p);
      break;

    default:
      return NULL;
  }

  if (list_len > param_len) return NULL;

  p_end_list = p + list_len;

  /* Loop through, extracting the attribute IDs */
  for (; p < p_end_list;) {
    BE_STREAM_TO_UINT8(descr, p);
    type = descr >> 3;
    size = descr & 7;

    if (type != UINT_DESC_TYPE) return NULL;

    switch (size) {
      case SIZE_TWO_BYTES:
        attr_len = 2;
        break;
      case SIZE_FOUR_BYTES:
        attr_len = 4;
        break;
      case SIZE_IN_NEXT_BYTE:
        if (p + sizeof(uint8_t) > p_end_list) return NULL;
        BE_STREAM_TO_UINT8(attr_len, p);
        break;
      case SIZE_IN_NEXT_WORD:
        if (p + sizeof(uint16_t) > p_end_list) return NULL;
        BE_STREAM_TO_UINT16(attr_len, p);
        break;
      case SIZE_IN_NEXT_LONG:
        if (p + sizeof(uint32_t) > p_end_list) return NULL;
        BE_STREAM_TO_UINT32(attr_len, p);
        break;
      default:
        return NULL;
        break;
    }

    /* Attribute length must be 2-bytes or 4-bytes for a paired entry. */
    if (p + attr_len > p_end_list) return NULL;
    if (attr_len == 2) {
      BE_STREAM_TO_UINT16(p_seq->attr_entry[p_seq->num_attr].start, p);
      p_seq->attr_entry[p_seq->num_attr].end =
          p_seq->attr_entry[p_seq->num_attr].start;
    } else if (attr_len == 4) {
      BE_STREAM_TO_UINT16(p_seq->attr_entry[p_seq->num_attr].start, p);
      BE_STREAM_TO_UINT16(p_seq->attr_entry[p_seq->num_attr].end, p);
    } else
      return (NULL);

    /* We can only do so many */
    if (++p_seq->num_attr >= MAX_ATTR_PER_SEQ) return (NULL);
  }

  return (p);
}

/*******************************************************************************
 *
 * Function         sdpu_get_len_from_type
 *
 * Description      This function gets the length
 *
 * Returns          void
 *
 ******************************************************************************/
uint8_t* sdpu_get_len_from_type(uint8_t* p, uint8_t* p_end, uint8_t type,
                                uint32_t* p_len) {
  uint8_t u8;
  uint16_t u16;
  uint32_t u32;

  switch (type & 7) {
    case SIZE_ONE_BYTE:
      *p_len = 1;
      break;
    case SIZE_TWO_BYTES:
      *p_len = 2;
      break;
    case SIZE_FOUR_BYTES:
      *p_len = 4;
      break;
    case SIZE_EIGHT_BYTES:
      *p_len = 8;
      break;
    case SIZE_SIXTEEN_BYTES:
      *p_len = 16;
      break;
    case SIZE_IN_NEXT_BYTE:
      if (p + 1 > p_end) {
        *p_len = 0;
        return NULL;
      }
      BE_STREAM_TO_UINT8(u8, p);
      *p_len = u8;
      break;
    case SIZE_IN_NEXT_WORD:
      if (p + 2 > p_end) {
        *p_len = 0;
        return NULL;
      }
      BE_STREAM_TO_UINT16(u16, p);
      *p_len = u16;
      break;
    case SIZE_IN_NEXT_LONG:
      if (p + 4 > p_end) {
        *p_len = 0;
        return NULL;
      }
      BE_STREAM_TO_UINT32(u32, p);
      *p_len = (uint16_t)u32;
      break;
  }

  return (p);
}

/*******************************************************************************
 *
 * Function         sdpu_is_base_uuid
 *
 * Description      This function checks a 128-bit UUID with the base to see if
 *                  it matches. Only the last 12 bytes are compared.
 *
 * Returns          true if matched, else false
 *
 ******************************************************************************/
bool sdpu_is_base_uuid(uint8_t* p_uuid) {
  uint16_t xx;

  for (xx = 4; xx < Uuid::kNumBytes128; xx++)
    if (p_uuid[xx] != sdp_base_uuid[xx]) return (false);

  /* If here, matched */
  return (true);
}

/*******************************************************************************
 *
 * Function         sdpu_compare_uuid_arrays
 *
 * Description      This function compares 2 BE UUIDs. If needed, they are
 *                  expanded to 128-bit UUIDs, then compared.
 *
 * NOTE             it is assumed that the arrays are in Big Endian format
 *
 * Returns          true if matched, else false
 *
 ******************************************************************************/
bool sdpu_compare_uuid_arrays(uint8_t* p_uuid1, uint32_t len1, uint8_t* p_uuid2,
                              uint16_t len2) {
  uint8_t nu1[Uuid::kNumBytes128];
  uint8_t nu2[Uuid::kNumBytes128];

  if (((len1 != 2) && (len1 != 4) && (len1 != 16)) ||
      ((len2 != 2) && (len2 != 4) && (len2 != 16))) {
    SDP_TRACE_ERROR("%s: invalid length", __func__);
    return false;
  }

  /* If lengths match, do a straight compare */
  if (len1 == len2) {
    if (len1 == 2)
      return ((p_uuid1[0] == p_uuid2[0]) && (p_uuid1[1] == p_uuid2[1]));
    if (len1 == 4)
      return ((p_uuid1[0] == p_uuid2[0]) && (p_uuid1[1] == p_uuid2[1]) &&
              (p_uuid1[2] == p_uuid2[2]) && (p_uuid1[3] == p_uuid2[3]));
    else
      return (memcmp(p_uuid1, p_uuid2, (size_t)len1) == 0);
  } else if (len1 > len2) {
    /* If the len1 was 4-byte, (so len2 is 2-byte), compare on the fly */
    if (len1 == 4) {
      return ((p_uuid1[0] == 0) && (p_uuid1[1] == 0) &&
              (p_uuid1[2] == p_uuid2[0]) && (p_uuid1[3] == p_uuid2[1]));
    } else {
      /* Normalize UUIDs to 16-byte form, then compare. Len1 must be 16 */
      memcpy(nu1, p_uuid1, Uuid::kNumBytes128);
      memcpy(nu2, sdp_base_uuid, Uuid::kNumBytes128);

      if (len2 == 4)
        memcpy(nu2, p_uuid2, len2);
      else if (len2 == 2)
        memcpy(nu2 + 2, p_uuid2, len2);

      return (memcmp(nu1, nu2, Uuid::kNumBytes128) == 0);
    }
  } else {
    /* len2 is greater than len1 */
    /* If the len2 was 4-byte, (so len1 is 2-byte), compare on the fly */
    if (len2 == 4) {
      return ((p_uuid2[0] == 0) && (p_uuid2[1] == 0) &&
              (p_uuid2[2] == p_uuid1[0]) && (p_uuid2[3] == p_uuid1[1]));
    } else {
      /* Normalize UUIDs to 16-byte form, then compare. Len1 must be 16 */
      memcpy(nu2, p_uuid2, Uuid::kNumBytes128);
      memcpy(nu1, sdp_base_uuid, Uuid::kNumBytes128);

      if (len1 == 4)
        memcpy(nu1, p_uuid1, (size_t)len1);
      else if (len1 == 2)
        memcpy(nu1 + 2, p_uuid1, (size_t)len1);

      return (memcmp(nu1, nu2, Uuid::kNumBytes128) == 0);
    }
  }
}

/*******************************************************************************
 *
 * Function         sdpu_compare_uuid_with_attr
 *
 * Description      This function compares a BT UUID structure with the UUID in
 *                  an SDP attribute record. If needed, they are expanded to
 *                  128-bit UUIDs, then compared.
 *
 * NOTE           - it is assumed that BT UUID structures are compressed to the
 *                  smallest possible UUIDs (by removing the base SDP UUID).
 *                - it is also assumed that the discovery atribute is compressed
 *                  to the smallest possible
 *
 * Returns          true if matched, else false
 *
 ******************************************************************************/
bool sdpu_compare_uuid_with_attr(const Uuid& uuid, tSDP_DISC_ATTR* p_attr) {
  int len = uuid.GetShortestRepresentationSize();
  if (len == 2) {
    if (SDP_DISC_ATTR_LEN(p_attr->attr_len_type) == Uuid::kNumBytes16) {
      return uuid.As16Bit() == p_attr->attr_value.v.u16;
    } else {
      LOG(ERROR) << "invalid length for discovery attribute";
      return (false);
    }
  }
  if (len == 4) {
    if (SDP_DISC_ATTR_LEN(p_attr->attr_len_type) == Uuid::kNumBytes32) {
      return uuid.As32Bit() == p_attr->attr_value.v.u32;
    } else {
      LOG(ERROR) << "invalid length for discovery attribute";
      return (false);
    }
  }

  if (SDP_DISC_ATTR_LEN(p_attr->attr_len_type) != Uuid::kNumBytes128) {
    LOG(ERROR) << "invalid length for discovery attribute";
    return (false);
  }

  if (memcmp(uuid.To128BitBE().data(), (void*)p_attr->attr_value.v.array,
             Uuid::kNumBytes128) == 0)
    return (true);

  return (false);
}

/*******************************************************************************
 *
 * Function         sdpu_sort_attr_list
 *
 * Description      sorts a list of attributes in numeric order from lowest to
 *                  highest to conform to SDP specification
 *
 * Returns          void
 *
 ******************************************************************************/
void sdpu_sort_attr_list(uint16_t num_attr, tSDP_DISCOVERY_DB* p_db) {
  uint16_t i;
  uint16_t x;

  /* Done if no attributes to sort */
  if (num_attr <= 1) {
    return;
  } else if (num_attr > SDP_MAX_ATTR_FILTERS) {
    num_attr = SDP_MAX_ATTR_FILTERS;
  }

  num_attr--; /* for the for-loop */
  for (i = 0; i < num_attr;) {
    if (p_db->attr_filters[i] > p_db->attr_filters[i + 1]) {
      /* swap the attribute IDs and start from the beginning */
      x = p_db->attr_filters[i];
      p_db->attr_filters[i] = p_db->attr_filters[i + 1];
      p_db->attr_filters[i + 1] = x;

      i = 0;
    } else
      i++;
  }
}

/*******************************************************************************
 *
 * Function         sdpu_get_list_len
 *
 * Description      gets the total list length in the sdp database for a given
 *                  uid sequence and attr sequence
 *
 * Returns          void
 *
 ******************************************************************************/
uint16_t sdpu_get_list_len(tSDP_UUID_SEQ* uid_seq, tSDP_ATTR_SEQ* attr_seq) {
  tSDP_RECORD* p_rec;
  uint16_t len = 0;
  uint16_t len1;

  for (p_rec = sdp_db_service_search(NULL, uid_seq); p_rec;
       p_rec = sdp_db_service_search(p_rec, uid_seq)) {
    len += 3;

    len1 = sdpu_get_attrib_seq_len(p_rec, attr_seq);

    if (len1 != 0)
      len += len1;
    else
      len -= 3;
  }
  return len;
}

/*******************************************************************************
 *
 * Function         sdpu_get_attrib_seq_len
 *
 * Description      gets the length of the specific attributes in a given
 *                  sdp record
 *
 * Returns          void
 *
 ******************************************************************************/
uint16_t sdpu_get_attrib_seq_len(tSDP_RECORD* p_rec, tSDP_ATTR_SEQ* attr_seq) {
  tSDP_ATTRIBUTE* p_attr;
  uint16_t len1 = 0;
  uint16_t xx;
  bool is_range = false;
  uint16_t start_id = 0, end_id = 0;

  for (xx = 0; xx < attr_seq->num_attr; xx++) {
    if (!is_range) {
      start_id = attr_seq->attr_entry[xx].start;
      end_id = attr_seq->attr_entry[xx].end;
    }
    p_attr = sdp_db_find_attr_in_rec(p_rec, start_id, end_id);
    if (p_attr) {
      len1 += sdpu_get_attrib_entry_len(p_attr);

      /* If doing a range, stick with this one till no more attributes found */
      if (start_id != end_id) {
        /* Update for next time through */
        start_id = p_attr->id + 1;
        xx--;
        is_range = true;
      } else
        is_range = false;
    } else
      is_range = false;
  }
  return len1;
}

/*******************************************************************************
 *
 * Function         sdpu_get_attrib_entry_len
 *
 * Description      gets the length of a specific attribute
 *
 * Returns          void
 *
 ******************************************************************************/
uint16_t sdpu_get_attrib_entry_len(tSDP_ATTRIBUTE* p_attr) {
  uint16_t len = 3;

  /* the attribute is in the db record.
   * assuming the attribute len is less than SDP_MAX_ATTR_LEN */
  switch (p_attr->type) {
    case TEXT_STR_DESC_TYPE:     /* 4 */
    case DATA_ELE_SEQ_DESC_TYPE: /* 6 */
    case DATA_ELE_ALT_DESC_TYPE: /* 7 */
    case URL_DESC_TYPE:          /* 8 */
#if (SDP_MAX_ATTR_LEN > 0xFFFF)
      if (p_attr->len > 0xFFFF) {
        len += 5;
      } else
#endif /* 0xFFFF - 0xFF */
#if (SDP_MAX_ATTR_LEN > 0xFF)
          if (p_attr->len > 0xFF) {
        len += 3;
      } else
#endif /* 0xFF and less*/
      {
        len += 2;
      }
      len += p_attr->len;
      return len;
  }

  /* Now, the attribute value */
  switch (p_attr->len) {
    case 1:
    case 2:
    case 4:
    case 8:
    case 16:
      len += 1;
      break;
    default:
      len += 2;
      break;
  }

  len += p_attr->len;
  return len;
}

/*******************************************************************************
 *
 * Function         sdpu_build_partial_attrib_entry
 *
 * Description      This function fills a buffer with partial attribute. It is
 *                  assumed that the maximum size of any attribute is 256 bytes.
 *
 *                  p_out: output buffer
 *                  p_attr: attribute to be copied partially into p_out
 *                  rem_len: num bytes to copy into p_out
 *                  offset: current start offset within the attr that needs to
 *                          be copied
 *
 * Returns          Pointer to next byte in the output buffer.
 *                  offset is also updated
 *
 ******************************************************************************/
uint8_t* sdpu_build_partial_attrib_entry(uint8_t* p_out, tSDP_ATTRIBUTE* p_attr,
                                         uint16_t len, uint16_t* offset) {
  uint8_t* p_attr_buff =
      (uint8_t*)osi_malloc(sizeof(uint8_t) * SDP_MAX_ATTR_LEN);
  sdpu_build_attrib_entry(p_attr_buff, p_attr);

  uint16_t attr_len = sdpu_get_attrib_entry_len(p_attr);

  if (len > SDP_MAX_ATTR_LEN) {
    SDP_TRACE_ERROR("%s len %d exceeds SDP_MAX_ATTR_LEN", __func__, len);
    len = SDP_MAX_ATTR_LEN;
  }

  size_t len_to_copy =
      ((attr_len - *offset) < len) ? (attr_len - *offset) : len;
  memcpy(p_out, &p_attr_buff[*offset], len_to_copy);

  p_out = &p_out[len_to_copy];
  *offset += len_to_copy;

  osi_free(p_attr_buff);
  return p_out;
}
