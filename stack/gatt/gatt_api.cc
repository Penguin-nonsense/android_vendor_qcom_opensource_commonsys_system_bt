/******************************************************************************
 *
 *  Copyright (C) 1999-2012 Broadcom Corporation
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
 *  this file contains GATT interface functions
 *
 ******************************************************************************/
#include "bt_target.h"

#include <base/strings/string_number_conversions.h>
#include <stdio.h>
#include <string.h>
#include "bt_common.h"
#include "btm_int.h"
#include "device/include/controller.h"
#include "gatt_api.h"
#include "gatt_int.h"
#include "l2c_api.h"
#include "stack/gatt/connection_manager.h"
#include "stack/gatt/eatt_int.h"
#include "btif_storage.h"
#include "stack_config.h"

#define SYSTEM_APP_GATT_IF 3

using bluetooth::Uuid;
using base::StringPrintf;

extern bool BTM_GetLeDisconnectStatus(const RawAddress& address);
extern bool BTM_BackgroundConnectAddressKnown(const RawAddress& address);
/**
 * Add an service handle range to the list in decending order of the start
 * handle. Return reference to the newly added element.
 **/
tGATT_HDL_LIST_ELEM& gatt_add_an_item_to_list(uint16_t s_handle) {
  auto lst_ptr = gatt_cb.hdl_list_info;
  auto it = lst_ptr->begin();
  for (; it != lst_ptr->end(); it++) {
    if (s_handle > it->asgn_range.s_handle) break;
  }

  auto rit = lst_ptr->emplace(it);
  return *rit;
}

/*****************************************************************************
 *
 *                  GATT SERVER API
 *
 *****************************************************************************/
/*******************************************************************************
 *
 * Function         GATTS_AddHandleRange
 *
 * Description      This function add the allocated handles range for the
 *                  specified application UUID, service UUID and service
 *                  instance
 *
 * Parameter        p_hndl_range:   pointer to allocated handles information
 *
 **/

void GATTS_AddHandleRange(tGATTS_HNDL_RANGE* p_hndl_range) {
  gatt_add_an_item_to_list(p_hndl_range->s_handle);
}

/*******************************************************************************
 *
 * Function         GATTS_NVRegister
 *
 * Description      Application manager calls this function to register for
 *                  NV save callback function.  There can be one and only one
 *                  NV save callback function.
 *
 * Parameter        p_cb_info : callback informaiton
 *
 * Returns          true if registered OK, else false
 *
 ******************************************************************************/
bool GATTS_NVRegister(tGATT_APPL_INFO* p_cb_info) {
  bool status = false;
  if (p_cb_info) {
    gatt_cb.cb_info = *p_cb_info;
    status = true;
    gatt_init_srv_chg();
  }

  return status;
}

static uint16_t compute_service_size(btgatt_db_element_t* service, int count) {
  int db_size = 0;
  btgatt_db_element_t* el = service;

  for (int i = 0; i < count; i++, el++)
    if (el->type == BTGATT_DB_PRIMARY_SERVICE ||
        el->type == BTGATT_DB_SECONDARY_SERVICE ||
        el->type == BTGATT_DB_DESCRIPTOR ||
        el->type == BTGATT_DB_INCLUDED_SERVICE) {
      db_size += 1;
    } else if (el->type == BTGATT_DB_CHARACTERISTIC) {
      db_size += 2;

      // if present, Characteristic Extended Properties takes one handle
      if (el->properties & GATT_CHAR_PROP_BIT_EXT_PROP) db_size++;
    } else {
      LOG(ERROR) << __func__ << ": Unknown element type: " << el->type;
      db_size = 0;
      break;
    }

  return db_size;
}

static bool is_gatt_attr_type(const Uuid& uuid) {
  if (uuid == Uuid::From16Bit(GATT_UUID_PRI_SERVICE) ||
      uuid == Uuid::From16Bit(GATT_UUID_SEC_SERVICE) ||
      uuid == Uuid::From16Bit(GATT_UUID_INCLUDE_SERVICE) ||
      uuid == Uuid::From16Bit(GATT_UUID_CHAR_DECLARE)) {
    return true;
  }
  return false;
}

/** Update the the last service info for the service list info */
static void gatt_update_last_srv_info() {
  gatt_cb.last_service_handle = 0;

  for (tGATT_SRV_LIST_ELEM& el : *gatt_cb.srv_list_info) {
    gatt_cb.last_service_handle = el.s_hdl;
  }
}

/** Update database hash and client status */
static void gatt_update_for_database_change() {
  gatt_cb.database_hash = gatts_calculate_database_hash(gatt_cb.srv_list_info);

  uint8_t i = 0;
  for (i = 0; i < GATT_MAX_PHY_CHANNEL; i++) {
    tGATT_TCB& tcb = gatt_cb.tcb[i];
    if (tcb.in_use) gatt_sr_update_cl_status(tcb, /* chg_aware= */ false);
  }
}

static bool is_uuid_le_only_transport(Uuid uuid) {
  uint16_t uuid_val = uuid.As16Bit();
  bool status = false;

  switch (uuid_val) {
    case 0x1849:
      FALLTHROUGH_INTENDED; /* FALLTHROUGH */
    case 0x184C:
      {
        status = true;
        APPL_TRACE_DEBUG("%s: Only LE Transport 0x%X ", __func__, uuid.As16Bit());
      }
      break;
    default:
      APPL_TRACE_DEBUG("%s: 0x%X, 0x%X ", __func__, uuid.As16Bit(), uuid_val);
  }
  return status;
}


/*******************************************************************************
 *
 * Function         GATTS_AddService
 *
 * Description      This function is called to add GATT service.
 *
 * Parameter        gatt_if : application if
 *                  service : pseudo-representation of service and it's content
 *                  count   : size of service
 *
 * Returns          on success GATT_SERVICE_STARTED is returned, and
 *                  attribute_handle field inside service elements are filled.
 *                  on error error status is returned.
 *
 ******************************************************************************/
uint16_t GATTS_AddService(tGATT_IF gatt_if, btgatt_db_element_t* service,
                          int count) {
  uint16_t s_hdl = 0;
  bool save_hdl = false;
  tGATT_REG* p_reg = gatt_get_regcb(gatt_if);

  bool is_pri = (service->type == BTGATT_DB_PRIMARY_SERVICE) ? true : false;
  Uuid svc_uuid = service->uuid;

  LOG(INFO) << __func__;

  if (!p_reg) {
    LOG(ERROR) << "Invalid gatt_if=" << +gatt_if;
    return GATT_INTERNAL_ERROR;
  }

  uint16_t num_handles = compute_service_size(service, count);
  if (num_handles == 0) {
    LOG(ERROR) << "Invalid Gatt Service. Skip adding in db. gatt_if:" << +gatt_if;
    return GATT_INTERNAL_ERROR;
  }

  if (svc_uuid == Uuid::From16Bit(UUID_SERVCLASS_GATT_SERVER)) {
    s_hdl = gatt_cb.hdl_cfg.gatt_start_hdl;
  } else if (svc_uuid == Uuid::From16Bit(UUID_SERVCLASS_GAP_SERVER)) {
    s_hdl = gatt_cb.hdl_cfg.gap_start_hdl;
  } else {
    if (!gatt_cb.hdl_list_info->empty()) {
      s_hdl = gatt_cb.hdl_list_info->front().asgn_range.e_handle + 1;
    }

    if (s_hdl < gatt_cb.hdl_cfg.app_start_hdl)
      s_hdl = gatt_cb.hdl_cfg.app_start_hdl;

    save_hdl = true;
  }

  /* check for space */
  if (num_handles > (0xFFFF - s_hdl + 1)) {
    LOG(ERROR) << __func__ << ": no handles, s_hdl=" << +s_hdl
               << " needed=" << num_handles;
    return GATT_INTERNAL_ERROR;
  }

  tGATT_HDL_LIST_ELEM& list = gatt_add_an_item_to_list(s_hdl);
  list.asgn_range.app_uuid128 = p_reg->app_uuid128;
  list.asgn_range.svc_uuid = svc_uuid;
  list.asgn_range.s_handle = s_hdl;
  list.asgn_range.e_handle = s_hdl + num_handles - 1;
  list.asgn_range.is_primary = is_pri;

  if (save_hdl) {
    if (gatt_cb.cb_info.p_nv_save_callback)
      (*gatt_cb.cb_info.p_nv_save_callback)(true, &list.asgn_range);
  }

  gatts_init_service_db(list.svc_db, svc_uuid, is_pri, s_hdl, num_handles);

  VLOG(1) << __func__ << ": handles needed=" << num_handles
          << ", s_hdl=" << loghex(list.asgn_range.s_handle)
          << ", e_hdl=" << loghex(list.asgn_range.e_handle)
          << ", uuid=" << list.asgn_range.svc_uuid
          << ", is_primary=" << +list.asgn_range.is_primary;

  service->attribute_handle = s_hdl;

  btgatt_db_element_t* el = service + 1;
  for (int i = 0; i < count - 1; i++, el++) {
    const Uuid& uuid = el->uuid;

    if (el->type == BTGATT_DB_CHARACTERISTIC) {
      /* data validity checking */
      if (((el->properties & GATT_CHAR_PROP_BIT_AUTH) &&
           !(el->permissions & GATT_WRITE_SIGNED_PERM)) ||
          ((el->permissions & GATT_WRITE_SIGNED_PERM) &&
           !(el->properties & GATT_CHAR_PROP_BIT_AUTH))) {
        VLOG(1) << "Invalid configuration property=" << loghex(el->properties)
                << ", perm=" << loghex(el->permissions);
        return GATT_INTERNAL_ERROR;
      }

      if (is_gatt_attr_type(uuid)) {
        LOG(ERROR) << __func__
                   << ": attempt to add characteristic with UUID equal to GATT "
                      "Attribute Type "
                   << uuid;
        return GATT_INTERNAL_ERROR;
      }

      el->attribute_handle = gatts_add_characteristic(
          list.svc_db, el->permissions, el->properties, uuid);

      // add characteristic extended properties descriptor if needed
      if (el->properties & GATT_CHAR_PROP_BIT_EXT_PROP) {
        gatts_add_char_ext_prop_descr(list.svc_db, el->extended_properties);
      }

    } else if (el->type == BTGATT_DB_DESCRIPTOR) {
      if (is_gatt_attr_type(uuid)) {
        LOG(ERROR) << __func__
                   << ": attempt to add descriptor with UUID equal to GATT "
                      "Attribute Type "
                   << uuid;
        return GATT_INTERNAL_ERROR;
      }

      el->attribute_handle =
          gatts_add_char_descr(list.svc_db, el->permissions, uuid);
    } else if (el->type == BTGATT_DB_INCLUDED_SERVICE) {
      tGATT_HDL_LIST_ELEM* p_incl_decl;
      p_incl_decl = gatt_find_hdl_buffer_by_handle(el->attribute_handle);
      if (p_incl_decl == nullptr) {
        VLOG(1) << "Included Service not created";
        return GATT_INTERNAL_ERROR;
      }

      el->attribute_handle = gatts_add_included_service(
          list.svc_db, p_incl_decl->asgn_range.s_handle,
          p_incl_decl->asgn_range.e_handle, p_incl_decl->asgn_range.svc_uuid);
    }
  }

  LOG(INFO) << __func__ << ": service parsed correctly, now starting";

  /*this is a new application service start */

  // find a place for this service in the list
  auto lst_ptr = gatt_cb.srv_list_info;
  auto it = lst_ptr->begin();
  for (; it != lst_ptr->end(); it++) {
    if (list.asgn_range.s_handle < it->s_hdl) break;
  }
  auto rit = lst_ptr->emplace(it);

  tGATT_SRV_LIST_ELEM& elem = *rit;
  elem.gatt_if = gatt_if;
  elem.s_hdl = list.asgn_range.s_handle;
  elem.e_hdl = list.asgn_range.e_handle;
  elem.p_db = &list.svc_db;
  elem.is_primary = list.asgn_range.is_primary;

  elem.app_uuid = list.asgn_range.app_uuid128;
  elem.type = list.asgn_range.is_primary ? GATT_UUID_PRI_SERVICE
                                         : GATT_UUID_SEC_SERVICE;

  if (elem.type == GATT_UUID_PRI_SERVICE && gatt_cb.over_br_enabled) {
    Uuid* p_uuid = gatts_get_service_uuid(elem.p_db);
    if (p_uuid && !is_uuid_le_only_transport(*p_uuid)) {
      elem.sdp_handle = gatt_add_sdp_record(*p_uuid, elem.s_hdl, elem.e_hdl);
    } else {
      elem.sdp_handle = 0;
    }
  } else {
    elem.sdp_handle = 0;
  }

  gatt_update_last_srv_info();

  VLOG(1) << __func__ << ": allocated el s_hdl=" << loghex(elem.s_hdl)
          << ", e_hdl=" << loghex(elem.e_hdl) << ", type=" << loghex(elem.type)
          << ", sdp_hdl=" << loghex(elem.sdp_handle);

  gatt_update_for_database_change();
  if (!stack_config_get_interface()->get_pts_service_chg_indication_disable()) {
    gatt_proc_srv_chg();
  }

  return GATT_SERVICE_STARTED;
}

bool is_active_service(const Uuid& app_uuid128, Uuid* p_svc_uuid,
                       uint16_t start_handle) {
  for (auto& info : *gatt_cb.srv_list_info) {
    Uuid* p_this_uuid = gatts_get_service_uuid(info.p_db);

    if (p_this_uuid && app_uuid128 == info.app_uuid &&
        *p_svc_uuid == *p_this_uuid && (start_handle == info.s_hdl)) {
      LOG(ERROR) << "Active Service Found: " << *p_svc_uuid;
      return true;
    }
  }
  return false;
}

/*******************************************************************************
 *
 * Function         GATTS_DeleteService
 *
 * Description      This function is called to delete a service.
 *
 * Parameter        gatt_if       : application interface
 *                  p_svc_uuid    : service UUID
 *                  start_handle  : start handle of the service
 *
 * Returns          true if the operation succeeded, false if the handle block
 *                  was not found.
 *
 ******************************************************************************/
bool GATTS_DeleteService(tGATT_IF gatt_if, Uuid* p_svc_uuid,
                         uint16_t svc_inst) {
  VLOG(1) << __func__;

  tGATT_REG* p_reg = gatt_get_regcb(gatt_if);
  if (p_reg == NULL) {
    LOG(ERROR) << "Applicaiton not foud";
    return false;
  }

  auto it =
      gatt_find_hdl_buffer_by_app_id(p_reg->app_uuid128, p_svc_uuid, svc_inst);
  if (it == gatt_cb.hdl_list_info->end()) {
    LOG(ERROR) << "No Service found";
    return false;
  }

  if (is_active_service(p_reg->app_uuid128, p_svc_uuid, svc_inst)) {
    GATTS_StopService(it->asgn_range.s_handle);
  }

  gatt_update_for_database_change();
  if (!stack_config_get_interface()->get_pts_service_chg_indication_disable()) {
    gatt_proc_srv_chg();
  }

  VLOG(1) << "released handles s_hdl=" << loghex(it->asgn_range.s_handle)
          << ", e_hdl=" << loghex(it->asgn_range.e_handle);

  if ((it->asgn_range.s_handle >= gatt_cb.hdl_cfg.app_start_hdl) &&
      gatt_cb.cb_info.p_nv_save_callback)
    (*gatt_cb.cb_info.p_nv_save_callback)(false, &it->asgn_range);

  gatt_cb.hdl_list_info->erase(it);
  return true;
}

/*******************************************************************************
 *
 * Function         GATTS_StopService
 *
 * Description      This function is called to stop a service
 *
 * Parameter         service_handle : this is the start handle of a service
 *
 * Returns          None.
 *
 ******************************************************************************/
void GATTS_StopService(uint16_t service_handle) {
  LOG(INFO) << __func__ << ": " << loghex(service_handle);

  auto it = gatt_sr_find_i_rcb_by_handle(service_handle);
  if (it == gatt_cb.srv_list_info->end()) {
    LOG(ERROR) << __func__ << ": service_handle=" << loghex(service_handle)
               << " is not in use";
  }

  if (it->sdp_handle) {
    SDP_DeleteRecord(it->sdp_handle);
  }

  gatt_cb.srv_list_info->erase(it);
  gatt_update_last_srv_info();
}
/*******************************************************************************
 *
 * Function         GATTs_HandleValueIndication
 *
 * Description      This function sends a handle value indication to a client.
 *
 * Parameter        conn_id: connection identifier.
 *                  attr_handle: Attribute handle of this handle value
 *                               indication.
 *                  val_len: Length of the indicated attribute value.
 *                  p_val: Pointer to the indicated attribute value data.
 *
 * Returns          GATT_SUCCESS if sucessfully sent or queued; otherwise error
 *                  code.
 *
 ******************************************************************************/
tGATT_STATUS GATTS_HandleValueIndication(uint16_t conn_id, uint16_t attr_handle,
                                         uint16_t val_len, uint8_t* p_val) {
  tGATT_IF gatt_if = GATT_GET_GATT_IF(conn_id);
  uint8_t tcb_idx = GATT_GET_TCB_IDX(conn_id);
  tGATT_REG* p_reg = gatt_get_regcb(gatt_if);
  tGATT_TCB* p_tcb = gatt_get_tcb_by_idx(tcb_idx);
  uint16_t lcid = 0;
  tGATT_EBCB* p_eatt_bcb = NULL;
  uint16_t indicate_handle = 0;

  VLOG(1) << __func__;
  if ((p_reg == NULL) || (p_tcb == NULL)) {
    LOG(ERROR) << __func__ << ": Unknown  conn_id=" << loghex(conn_id);
    return (tGATT_STATUS)GATT_INVALID_CONN_ID;
  }

  if (!GATT_HANDLE_IS_VALID(attr_handle)) return GATT_ILLEGAL_PARAMETER;

  tGATT_VALUE indication;
  indication.conn_id = conn_id;
  indication.handle = attr_handle;
  indication.len = val_len;
  memcpy(indication.value, p_val, val_len);
  indication.auth_req = GATT_AUTH_REQ_NONE;
  indication.read_sub_type = 0;
  indication.offset = 0;

  lcid = p_tcb->att_lcid;
  indicate_handle = p_tcb->indicate_handle;
  if (p_tcb->is_eatt_supported && p_reg->eatt_support) {
    if (is_gatt_conn_id_found(conn_id)) {
      lcid = gatt_get_cid_by_conn_id(conn_id);
      p_eatt_bcb = gatt_find_eatt_bcb_by_cid(p_tcb, lcid);
      if (p_eatt_bcb)
        indicate_handle = p_eatt_bcb->indicate_handle;
    }
    else {
      //Find least burdened channel
      p_eatt_bcb = gatt_find_best_eatt_bcb(p_tcb, gatt_if, 0, false);
      if (p_eatt_bcb) {
        lcid = p_eatt_bcb->cid;
        indicate_handle = p_eatt_bcb->indicate_handle;
      }
    }
  }
  else if (p_tcb->is_eatt_supported && !p_reg->eatt_support) {
    p_eatt_bcb = gatt_find_eatt_bcb_by_cid(p_tcb, L2CAP_ATT_CID);
    if (p_eatt_bcb)
      indicate_handle = p_eatt_bcb->indicate_handle;
  }

  if (GATT_HANDLE_IS_VALID(indicate_handle)) {
    VLOG(1) << "Add a pending indication";
    gatt_add_pending_ind(p_tcb, lcid, &indication);
    return GATT_SUCCESS;
  }

  tGATT_SR_MSG gatt_sr_msg;
  gatt_sr_msg.attr_value = indication;
  BT_HDR* p_msg =
      attp_build_sr_msg(*p_tcb, lcid, GATT_HANDLE_VALUE_IND, &gatt_sr_msg);
  if (!p_msg) return GATT_NO_RESOURCES;

  tGATT_STATUS cmd_status = attp_send_sr_msg(*p_tcb, lcid, p_msg);
  if (cmd_status == GATT_SUCCESS || cmd_status == GATT_CONGESTED) {
    if (p_tcb->is_eatt_supported && p_eatt_bcb)
      p_eatt_bcb->indicate_handle = indication.handle;
    else
      p_tcb->indicate_handle = indication.handle;
    gatt_start_conf_timer(p_tcb, lcid);
  }
  else if (cmd_status == GATT_NO_CREDITS) {
    VLOG(1) << "Add a pending indication";
    if (p_tcb->is_eatt_supported && p_eatt_bcb &&
       (p_eatt_bcb->ind_no_credits_apps.empty() ||
        (std::find(p_eatt_bcb->ind_no_credits_apps.begin(), p_eatt_bcb->ind_no_credits_apps.end(),
        conn_id) == p_eatt_bcb->ind_no_credits_apps.end()))) {
      p_eatt_bcb->indicate_handle = indication.handle;
      gatt_add_pending_ind(p_tcb, lcid, &indication);
      p_eatt_bcb->ind_no_credits_apps.push_back(conn_id);
      cmd_status = GATT_CONGESTED;
    }
  }
  return cmd_status;
}

/*******************************************************************************
 *
 * Function         GATTS_HandleValueNotification
 *
 * Description      This function sends a handle value notification to a client.
 *
 * Parameter        conn_id: connection identifier.
 *                  attr_handle: Attribute handle of this handle value
 *                               indication.
 *                  val_len: Length of the indicated attribute value.
 *                  p_val: Pointer to the indicated attribute value data.
 *
 * Returns          GATT_SUCCESS if sucessfully sent; otherwise error code.
 *
 ******************************************************************************/
tGATT_STATUS GATTS_HandleValueNotification(uint16_t conn_id,
                                           uint16_t attr_handle,
                                           uint16_t val_len, uint8_t* p_val) {
  tGATT_VALUE notif;
  tGATT_IF gatt_if = GATT_GET_GATT_IF(conn_id);
  uint8_t tcb_idx = GATT_GET_TCB_IDX(conn_id);
  tGATT_REG* p_reg = gatt_get_regcb(gatt_if);
  tGATT_TCB* p_tcb = gatt_get_tcb_by_idx(tcb_idx);
  uint16_t lcid = 0;
  tGATT_EBCB* p_eatt_bcb = NULL;

  VLOG(1) << __func__;

  if ((p_reg == NULL) || (p_tcb == NULL)) {
    LOG(ERROR) << __func__ << "Unknown  conn_id: " << conn_id;
    return (tGATT_STATUS)GATT_INVALID_CONN_ID;
  }

  if (!GATT_HANDLE_IS_VALID(attr_handle)) {
    return GATT_ILLEGAL_PARAMETER;
  }

  notif.handle = attr_handle;
  notif.len = val_len;
  memcpy(notif.value, p_val, val_len);
  notif.auth_req = GATT_AUTH_REQ_NONE;
  notif.conn_id = conn_id;

  lcid = p_tcb->att_lcid;
  if (p_tcb->is_eatt_supported && p_reg->eatt_support) {
    if (is_gatt_conn_id_found(conn_id)) {
      lcid = gatt_get_cid_by_conn_id(conn_id);
      p_eatt_bcb = gatt_find_eatt_bcb_by_cid(p_tcb, lcid);
    }
    else {
      p_eatt_bcb = gatt_find_best_eatt_bcb(p_tcb, gatt_if, 0, false);
      if (p_eatt_bcb)
        lcid = p_eatt_bcb->cid;
    }
  }

  tGATT_STATUS cmd_sent;
  tGATT_SR_MSG gatt_sr_msg;
  gatt_sr_msg.attr_value = notif;
  BT_HDR* p_buf =
      attp_build_sr_msg(*p_tcb, lcid, GATT_HANDLE_VALUE_NOTIF, &gatt_sr_msg);

  if (p_buf != NULL) {
    cmd_sent = attp_send_sr_msg(*p_tcb, lcid, p_buf);
  } else
    cmd_sent = GATT_NO_RESOURCES;

  if (cmd_sent == GATT_NO_CREDITS) {
    if (p_tcb->is_eatt_supported && p_eatt_bcb &&
       (p_eatt_bcb->notif_no_credits_apps.empty()
        || (std::find(p_eatt_bcb->notif_no_credits_apps.begin(),
            p_eatt_bcb->notif_no_credits_apps.end(),
            conn_id) == p_eatt_bcb->notif_no_credits_apps.end()))) {
      gatt_notif_enq(p_tcb, lcid, &notif);
      p_eatt_bcb->notif_no_credits_apps.push_back(conn_id);
      cmd_sent = GATT_CONGESTED;
    }
  }

  return cmd_sent;
}

/*******************************************************************************
 *
 * Function         GATTS_MultiHandleValueNotifications
 *
 * Description      This function sends multiple handle value notifications to a client.
 *
 * Parameter        conn_id: connection identifier.
 *                  num_attr: number of attributes which are notified
 *                  handles: array of attribute handles notified.
 *                  lens: array of lengths of characteristic value notified.
 *                  values: vector of characteristic values notified.
 *
 * Returns          GATT_SUCCESS if sucessfully sent; otherwise error code.
 *
 ******************************************************************************/
tGATT_STATUS GATTS_MultiHandleValueNotifications(uint16_t conn_id,
                                                 uint8_t num_attr,
                                                 uint16_t handles[],
                                                 uint16_t lens[],
                                                 std::vector<std::vector<uint8_t>> values) {
  tGATT_IF gatt_if = GATT_GET_GATT_IF(conn_id);
  uint8_t tcb_idx = GATT_GET_TCB_IDX(conn_id);
  tGATT_REG* p_reg = gatt_get_regcb(gatt_if);
  tGATT_TCB* p_tcb = gatt_get_tcb_by_idx(tcb_idx);
  uint16_t lcid = 0;
  tGATT_EBCB* p_eatt_bcb;
  tGATT_MULTI_NOTIF multi_ntf;
  uint16_t payload_size = 0;
  uint8_t cl_supp_feat = 0;

  VLOG(1) << __func__ << " gatt_if:" << +gatt_if;

  if ((p_reg == NULL) || (p_tcb == NULL)) {
    LOG(ERROR) << __func__ << "Unknown  conn_id: " << conn_id;
    return (tGATT_STATUS)GATT_INVALID_CONN_ID;
  }

  cl_supp_feat = btif_storage_get_cl_supp_feat(p_tcb->peer_bda);
  if ((cl_supp_feat & CL_MULTI_NOTIF_SUPPORTED) != CL_MULTI_NOTIF_SUPPORTED) {
    LOG(ERROR) << __func__ << " Unsupported by remote client";
    return GATT_REQ_NOT_SUPPORTED;
  }

  multi_ntf.auth_req = GATT_AUTH_REQ_NONE;
  multi_ntf.conn_id = conn_id;
  multi_ntf.num_attr = num_attr;

  for (uint8_t i=0; i<num_attr; i++) {
    multi_ntf.handles[i] = handles[i];
    multi_ntf.lens[i] = lens[i];
    if (!GATT_HANDLE_IS_VALID(multi_ntf.handles[i])) {
      return GATT_ILLEGAL_PARAMETER;
    }
  }
  multi_ntf.values = values;

  lcid = p_tcb->att_lcid;
  payload_size = p_tcb->payload_size;
  if (p_tcb->is_eatt_supported && p_reg->eatt_support) {
    if (is_gatt_conn_id_found(conn_id)) {
      lcid = gatt_get_cid_by_conn_id(conn_id);
    }
    else {
      //Find least burdened channel
      p_eatt_bcb = gatt_find_best_eatt_bcb(p_tcb, gatt_if, 0, false);
      if (p_eatt_bcb)
        lcid = p_eatt_bcb->cid;
    }
  }
  payload_size = gatt_get_payload_size(p_tcb, lcid);

  tGATT_STATUS cmd_sent;

  BT_HDR* p_buf = attp_build_multi_ntf_cmd(payload_size, multi_ntf);

  if (p_buf != NULL) {
    cmd_sent = attp_send_sr_msg(*p_tcb, lcid, p_buf);
  } else
    cmd_sent = GATT_NO_RESOURCES;
  return cmd_sent;
}

/*******************************************************************************
 *
 * Function         GATTS_SendRsp
 *
 * Description      This function sends the server response to client.
 *
 * Parameter        conn_id: connection identifier.
 *                  trans_id: transaction id
 *                  status: response status
 *                  p_msg: pointer to message parameters structure.
 *
 * Returns          GATT_SUCCESS if sucessfully sent; otherwise error code.
 *
 ******************************************************************************/
tGATT_STATUS GATTS_SendRsp(uint16_t conn_id, uint32_t trans_id,
                           tGATT_STATUS status, tGATTS_RSP* p_msg) {
  tGATT_STATUS cmd_sent = GATT_ILLEGAL_PARAMETER;
  tGATT_IF gatt_if = GATT_GET_GATT_IF(conn_id);
  uint8_t tcb_idx = GATT_GET_TCB_IDX(conn_id);
  tGATT_REG* p_reg = gatt_get_regcb(gatt_if);
  tGATT_TCB* p_tcb = gatt_get_tcb_by_idx(tcb_idx);
  uint32_t sr_cmd_trans_id = 0;
  uint8_t op_code = 0;
  tGATT_EBCB* p_eatt_bcb = NULL;

  VLOG(1) << __func__ << ": conn_id=" << loghex(conn_id)
          << ", trans_id=" << loghex(trans_id) << ", status=" << loghex(status);

  if ((p_reg == NULL) || (p_tcb == NULL)) {
    LOG(ERROR) << "Unknown  conn_id=" << loghex(conn_id);
    return (tGATT_STATUS)GATT_INVALID_CONN_ID;
  }

  if (p_tcb->is_eatt_supported) {
    p_eatt_bcb =
            gatt_find_eatt_bcb_by_srv_trans_id(trans_id, p_tcb->peer_bda);
    if (p_eatt_bcb) {
      sr_cmd_trans_id = p_eatt_bcb->sr_cmd.trans_id;
      op_code = p_eatt_bcb->sr_cmd.op_code;
    }
    else {
      return (GATT_NO_RESOURCES);
    }
  }
  else {
    sr_cmd_trans_id = p_tcb->sr_cmd.trans_id;
    op_code = p_tcb->sr_cmd.op_code;
  }

  if (sr_cmd_trans_id != trans_id) {
    LOG(ERROR) << "conn_id=" << loghex(conn_id)
               << " waiting for op_code=" << loghex(sr_cmd_trans_id);
    return (GATT_WRONG_STATE);
  }
  /* Process App response */
  cmd_sent = gatt_sr_process_app_rsp(*p_tcb, gatt_if, trans_id,
                                     op_code, status, p_msg);

  if (cmd_sent == GATT_NO_CREDITS) {
    if (p_tcb->is_eatt_supported && p_eatt_bcb && p_eatt_bcb->gatt_rsp_q.empty()) {
      tGATT_PEND_RSP gatt_rsp;
      gatt_rsp.conn_id = conn_id;
      gatt_rsp.trans_id = trans_id;
      gatt_rsp.status = status;
      gatt_rsp.p_msg = p_msg;

      gatt_rsp_enq(p_tcb, p_eatt_bcb->cid, &gatt_rsp);
      cmd_sent = GATT_CONGESTED;
    }
  }

  return cmd_sent;
}

/*******************************************************************************
 *
 * Function         GATTS_ConfigureMTU
 *
 * Description      This function add the allocated handles range for the
 *                  specified application UUID, service UUID and service
 *                  instance
 *
 * Parameter        p_hndl_range:   pointer to allocated handles information
 *
******************************************************************************/
tGATT_STATUS GATTS_ConfigureMTU(uint16_t conn_id, uint16_t mtu) {
  tGATT_IF gatt_if = GATT_GET_GATT_IF(conn_id);
  uint8_t tcb_idx = GATT_GET_TCB_IDX(conn_id);
  tGATT_TCB* p_tcb = gatt_get_tcb_by_idx(tcb_idx);
  tGATT_REG* p_reg = gatt_get_regcb(gatt_if);

  if ((p_tcb == NULL) || (p_reg == NULL) || (mtu < GATT_DEF_BLE_MTU_SIZE) ||
      (mtu > GATT_MAX_MTU_SIZE)) {
    return GATT_ILLEGAL_PARAMETER;
  }

  /* Validate that the link is BLE, not BR/EDR */
  if (p_tcb->transport != BT_TRANSPORT_LE) {
    return GATT_ERROR;
  }

  tGATTS_DATA gatts_data;
  uint16_t lcid = gatt_get_cid_by_conn_id(conn_id);
  gatts_data.mtu = gatt_get_payload_size(p_tcb, lcid);

  VLOG(1) << __func__ << " mtu:" << +gatts_data.mtu;
  gatt_sr_send_req_callback(conn_id, 0, GATTS_REQ_TYPE_MTU, &gatts_data);

  return GATT_SUCCESS;
}

/******************************************************************************/
/* GATT Profile Srvr Functions */
/******************************************************************************/

/******************************************************************************/
/*                                                                            */
/*                  GATT CLIENT APIs                                          */
/*                                                                            */
/******************************************************************************/

/*******************************************************************************
 *
 * Function         GATTC_ConfigureMTU
 *
 * Description      This function is called to configure the ATT MTU size.
 *
 * Parameters       conn_id: connection identifier.
 *                  mtu    - attribute MTU size..
 *
 * Returns          GATT_SUCCESS if command started successfully.
 *
 ******************************************************************************/
tGATT_STATUS GATTC_ConfigureMTU(uint16_t conn_id, uint16_t mtu) {
  tGATT_IF gatt_if = GATT_GET_GATT_IF(conn_id);
  uint8_t tcb_idx = GATT_GET_TCB_IDX(conn_id);
  tGATT_TCB* p_tcb = gatt_get_tcb_by_idx(tcb_idx);
  tGATT_REG* p_reg = gatt_get_regcb(gatt_if);
  uint16_t lcid = 0;
  tGATT_EBCB* p_eatt_bcb = NULL;
  tGATT_CMPL_CBACK* p_cmpl_cb =
      (p_reg) ? p_reg->app_cb.p_cmpl_cb : NULL;
  tGATT_CL_COMPLETE cb_data;

  VLOG(1) << __func__ << ": conn_id=" << loghex(conn_id) << ", mtu=" << +mtu;

  if ((p_tcb == NULL) || (p_reg == NULL) || (mtu < GATT_DEF_BLE_MTU_SIZE) ||
      (mtu > GATT_MAX_MTU_SIZE)) {
    return GATT_ILLEGAL_PARAMETER;
  }

  /* Validate that the link is BLE, not BR/EDR */
  if (p_tcb->transport != BT_TRANSPORT_LE) {
    return GATT_ERROR;
  }

  if (gatt_is_clcb_allocated(conn_id)) {
    LOG(ERROR) << "GATT_BUSY conn_id = " << +conn_id;
    return GATT_BUSY;
  }

  tGATT_CLCB* p_clcb = gatt_clcb_alloc(conn_id);
  if (!p_clcb) return GATT_NO_RESOURCES;

  VLOG(1) << __func__ << " Not assigning ptcb payload size here: conn_id=" << loghex(conn_id) << ", mtu=" << +mtu;
  /*if (p_clcb->p_tcb)
    p_clcb->p_tcb->payload_size = GATT_MAX_MTU_SIZE;*/
  p_clcb->operation = GATTC_OPTYPE_CONFIG;

  lcid = p_tcb->att_lcid;

  tGATT_CL_MSG gatt_cl_msg;

  /* Since GATT MTU Exchange can be done only once, and it is impossible to
   * predict what MTU will be requested by other applications, let's use
   * default MTU in the request. */
  gatt_cl_msg.mtu = GATT_MAX_MTU_SIZE;

  LOG(INFO) << __func__ << StringPrintf("Configuring ATT mtu size conn_id:%hu mtu:%hu user mtu %hu",
            conn_id, gatt_cl_msg.mtu, mtu);

  if (p_clcb->p_tcb) {
    auto result = attp_send_cl_msg(*p_clcb->p_tcb, p_clcb, lcid, GATT_REQ_MTU, &gatt_cl_msg);
    if (result == GATT_SUCCESS || result == GATT_CMD_STARTED) {
      p_clcb->p_tcb->pending_user_mtu_exchange_value = mtu;
    }
    return result;
  }
  return GATT_ERROR;
}

/******************************************************************************
 *
 * Function         GATTC_TryMtuRequest
 *
 * Description      This function shall be called before calling
 *                  GATTC_ConfgureMTU in order to check if operation is
 *                  available to do.
 *
 * Parameters        remote_bda : peer device address. (input)
 *                   transport  : physical transport of the GATT connection
 *                                 (BR/EDR or LE) (input)
 *                   conn_id    : connection id  (input)
 *                   current_mtu: current mtu on the link (output)
 *
 * Returns          tGATTC_TryMtuRequestResult:
 *                  - MTU_EXCHANGE_NOT_DONE_YET: There was no MTU Exchange
 *                      procedure on the link. User can call GATTC_ConfigureMTU
 *                      now.
 *                  - MTU_EXCHANGE_NOT_ALLOWED : Not allowed for BR/EDR or if
 *                      link does not exist
 *                  - MTU_EXCHANGE_ALREADY_DONE: MTU Exchange is done. MTU
 *                      should be taken from current_mtu
 *                  - MTU_EXCHANGE_IN_PROGRESS : Other use is doing MTU
 *                      Exchange. Conn_id is stored for result.
 *
 ******************************************************************************/
tGATTC_TryMtuRequestResult GATTC_TryMtuRequest(const RawAddress& remote_bda,
                                               tBT_TRANSPORT transport,
                                               uint16_t conn_id,
                                               uint16_t* current_mtu) {
  LOG(INFO) << __func__ << StringPrintf("%s conn_id=0x%04x",
                                         remote_bda.ToString().c_str(), conn_id);
  *current_mtu = GATT_DEF_BLE_MTU_SIZE;

  if (transport == BT_TRANSPORT_BR_EDR) {
    LOG(ERROR)<< StringPrintf("Device %s connected over BR/EDR",
                 remote_bda.ToString().c_str());
    return MTU_EXCHANGE_NOT_ALLOWED;
  }

  tGATT_TCB* p_tcb = gatt_find_tcb_by_addr(remote_bda, transport);
  if (!p_tcb) {
    LOG(ERROR)<< __func__ << StringPrintf("Device %s is not connected ",
                     remote_bda.ToString().c_str());
    return MTU_EXCHANGE_DEVICE_DISCONNECTED;
  }

  if (gatt_is_pending_mtu_exchange(p_tcb)) {
    LOG(INFO) << __func__ << StringPrintf("Continue MTU pending for other client.");
    /* MTU Exchange is in progress, started by other GATT Client.
     * Wait until it is completed.
     */
    gatt_set_conn_id_waiting_for_mtu_exchange(p_tcb, conn_id);
    return MTU_EXCHANGE_IN_PROGRESS;
  }

  uint16_t mtu = gatt_get_mtu(remote_bda, transport);
  if (mtu == GATT_DEF_BLE_MTU_SIZE || mtu == 0) {
    LOG(INFO) << __func__ << StringPrintf("MTU not yet updated for %s",
                  remote_bda.ToString().c_str());
    return MTU_EXCHANGE_NOT_DONE_YET;
  }

  *current_mtu = mtu;
  return MTU_EXCHANGE_ALREADY_DONE;
}

/*******************************************************************************
 * Function         GATTC_UpdateUserAttMtuIfNeeded
 *
 * Description      This function to be called when user requested MTU after
 *                  MTU Exchange has been already done. This will update data
 *                  length in the controller.
 *
 * Parameters        remote_bda : peer device address. (input)
 *                   transport  : physical transport of the GATT connection
 *                                 (BR/EDR or LE) (input)
 *                   user_mtu: user request mtu
 *
 ******************************************************************************/
void GATTC_UpdateUserAttMtuIfNeeded(const RawAddress& remote_bda,
                                    tBT_TRANSPORT transport,
                                    uint16_t user_mtu) {
  LOG(INFO) << __func__ << StringPrintf("%s, mtu=%hu",
            remote_bda.ToString().c_str(), user_mtu);
  tGATT_TCB* p_tcb = gatt_find_tcb_by_addr(remote_bda, transport);
  if (!p_tcb) {
    LOG(ERROR) << __func__ << "Transport control block not found";
    return;
  }

  LOG(INFO) << __func__ << StringPrintf("%s, current mtu: %d, max_user_mtu:%d, user_mtu: %d",
               remote_bda.ToString().c_str(), p_tcb->payload_size,
               p_tcb->max_user_mtu, user_mtu);

  if (p_tcb->payload_size < user_mtu) {
    LOG(INFO) << __func__ << "User requested more than what GATT can handle. Trim it.";
    user_mtu = p_tcb->payload_size;
  }

  if (p_tcb->max_user_mtu >= user_mtu) {
    return;
  }

  p_tcb->max_user_mtu = user_mtu;
  BTM_SetBleDataLength(remote_bda, user_mtu);
}

std::list<uint16_t> GATTC_GetAndRemoveListOfConnIdsWaitingForMtuRequest(
    const RawAddress& remote_bda) {
  std::list result = std::list<uint16_t>();

  tGATT_TCB* p_tcb = gatt_find_tcb_by_addr(remote_bda, BT_TRANSPORT_LE);
  if (!p_tcb || p_tcb->conn_ids_waiting_for_mtu_exchange.empty()) {
    return result;
  }

  result.swap(p_tcb->conn_ids_waiting_for_mtu_exchange);
  return result;
}

/*******************************************************************************
 *
 * Function         GATTC_Discover
 *
 * Description      This function is called to do a discovery procedure on ATT
 *                  server.
 *
 * Parameters       conn_id: connection identifier.
 *                  disc_type:discovery type.
 *                  start_handle and end_handle: range of handles for discovery
 *                  uuid: uuid to discovery. set to Uuid::kEmpty for requests
 *                        that don't need it
 *
 * Returns          GATT_SUCCESS if command received/sent successfully.
 *
 ******************************************************************************/
tGATT_STATUS GATTC_Discover(uint16_t conn_id, tGATT_DISC_TYPE disc_type,
                            uint16_t start_handle, uint16_t end_handle,
                            const Uuid& uuid) {
  tGATT_IF gatt_if = GATT_GET_GATT_IF(conn_id);
  uint8_t tcb_idx = GATT_GET_TCB_IDX(conn_id);
  tGATT_TCB* p_tcb = gatt_get_tcb_by_idx(tcb_idx);
  tGATT_REG* p_reg = gatt_get_regcb(gatt_if);

  if ((p_tcb == NULL) || (p_reg == NULL) || (disc_type >= GATT_DISC_MAX)) {
    LOG(ERROR) << __func__ << " Illegal param: disc_type=" << +disc_type
               << " conn_id=" << loghex(conn_id);
    return GATT_ILLEGAL_PARAMETER;
  }

  LOG(INFO) << __func__ << " conn_id=" << loghex(conn_id)
            << ", disc_type=" << +disc_type
            << ", s_handle=" << loghex(start_handle)
            << ", e_handle=" << loghex(end_handle);

  if (!GATT_HANDLE_IS_VALID(start_handle) ||
      !GATT_HANDLE_IS_VALID(end_handle) ||
      /* search by type does not have a valid UUID param */
      (disc_type == GATT_DISC_SRVC_BY_UUID && uuid.IsEmpty())) {
    return GATT_ILLEGAL_PARAMETER;
  }

  if (gatt_is_clcb_allocated(conn_id)) {
    LOG(ERROR) << __func__ << "GATT_BUSY conn_id = " << +conn_id;
    return GATT_BUSY;
  }

  tGATT_CLCB* p_clcb = gatt_clcb_alloc(conn_id);
  if (!p_clcb) return GATT_NO_RESOURCES;

  p_clcb->operation = GATTC_OPTYPE_DISCOVERY;
  p_clcb->op_subtype = disc_type;
  p_clcb->s_handle = start_handle;
  p_clcb->e_handle = end_handle;
  p_clcb->uuid = uuid;

  gatt_act_discovery(p_clcb);
  return GATT_SUCCESS;
}

tGATT_STATUS GATTC_Discover(uint16_t conn_id, tGATT_DISC_TYPE disc_type,
                            uint16_t start_handle, uint16_t end_handle) {
  return GATTC_Discover(conn_id, disc_type, start_handle, end_handle,
                        Uuid::kEmpty);
}

/*******************************************************************************
 *
 * Function         GATTC_Read
 *
 * Description      This function is called to read the value of an attribute
 *                  from the server.
 *
 * Parameters       conn_id: connection identifier.
 *                  type    - attribute read type.
 *                  p_read  - read operation parameters.
 *
 * Returns          GATT_SUCCESS if command started successfully.
 *
 ******************************************************************************/
tGATT_STATUS GATTC_Read(uint16_t conn_id, tGATT_READ_TYPE type,
                        tGATT_READ_PARAM* p_read) {
  tGATT_IF gatt_if = GATT_GET_GATT_IF(conn_id);
  uint8_t tcb_idx = GATT_GET_TCB_IDX(conn_id);
  tGATT_TCB* p_tcb = gatt_get_tcb_by_idx(tcb_idx);
  tGATT_REG* p_reg = gatt_get_regcb(gatt_if);
  uint16_t payload_size = 0;

  VLOG(1) << __func__ << ": conn_id=" << loghex(conn_id)
          << ", type=" << loghex(type);

  if ((p_tcb == NULL) || (p_reg == NULL) || (p_read == NULL) ||
      ((type >= GATT_READ_MAX) || (type == 0))) {
    LOG(ERROR) << ": illegal param: conn_id=" << loghex(conn_id)
               << "type=" << loghex(type);
    return GATT_ILLEGAL_PARAMETER;
  }

  if (gatt_is_clcb_allocated(conn_id)) {
    LOG(ERROR) << "GATT_BUSY conn_id=" << loghex(conn_id);
    return GATT_BUSY;
  }

  tGATT_CLCB* p_clcb = gatt_clcb_alloc(conn_id);
  if (!p_clcb) return GATT_NO_RESOURCES;

  uint16_t lcid = gatt_get_cid_by_conn_id(conn_id);
  payload_size = gatt_get_payload_size(p_tcb, lcid);
  p_clcb->operation = GATTC_OPTYPE_READ;
  p_clcb->op_subtype = type;
  p_clcb->auth_req = p_read->by_handle.auth_req;
  p_clcb->counter = 0;
  p_clcb->read_req_current_mtu = payload_size;

  switch (type) {
    case GATT_READ_BY_TYPE:
    case GATT_READ_CHAR_VALUE:
      p_clcb->s_handle = p_read->service.s_handle;
      p_clcb->e_handle = p_read->service.e_handle;
      p_clcb->uuid = p_read->service.uuid;
      break;
    case GATT_READ_MULTIPLE:
    case GATT_READ_MULTIPLE_VARIABLE: {
      p_clcb->s_handle = 0;
      /* copy multiple handles in CB */
      tGATT_READ_MULTI* p_read_multi =
          (tGATT_READ_MULTI*)osi_malloc(sizeof(tGATT_READ_MULTI));
      p_clcb->p_attr_buf = (uint8_t*)p_read_multi;
      memcpy(p_read_multi, &p_read->read_multiple, sizeof(tGATT_READ_MULTI));
      break;
    }
    case GATT_READ_BY_HANDLE:
    case GATT_READ_PARTIAL:
      p_clcb->uuid = Uuid::kEmpty;
      p_clcb->s_handle = p_read->by_handle.handle;

      if (type == GATT_READ_PARTIAL) {
        p_clcb->counter = p_read->partial.offset;
      }

      break;
    default:
      break;
  }

  /* start security check */
  if (gatt_security_check_start(p_clcb)) p_tcb->pending_enc_clcb.push(p_clcb);
  return GATT_SUCCESS;
}

/*******************************************************************************
 *
 * Function         GATTC_Write
 *
 * Description      This function is called to write the value of an attribute
 *                  to the server.
 *
 * Parameters       conn_id: connection identifier.
 *                  type    - attribute write type.
 *                  p_write  - write operation parameters.
 *
 * Returns          GATT_SUCCESS if command started successfully.
 *
 ******************************************************************************/
tGATT_STATUS GATTC_Write(uint16_t conn_id, tGATT_WRITE_TYPE type,
                         tGATT_VALUE* p_write) {
  tGATT_IF gatt_if = GATT_GET_GATT_IF(conn_id);
  uint8_t tcb_idx = GATT_GET_TCB_IDX(conn_id);
  tGATT_TCB* p_tcb = gatt_get_tcb_by_idx(tcb_idx);
  tGATT_REG* p_reg = gatt_get_regcb(gatt_if);

  if ((p_tcb == NULL) || (p_reg == NULL) || (p_write == NULL) ||
      ((type != GATT_WRITE) && (type != GATT_WRITE_PREPARE) &&
       (type != GATT_WRITE_NO_RSP))) {
    LOG(ERROR) << __func__ << " Illegal param: conn_id=" << loghex(conn_id)
               << ", type=" << loghex(type);
    return GATT_ILLEGAL_PARAMETER;
  }

  if (gatt_is_clcb_allocated(conn_id)) {
    LOG(ERROR) << "GATT_BUSY conn_id=" << loghex(conn_id);
    return GATT_BUSY;
  }

  tGATT_CLCB* p_clcb = gatt_clcb_alloc(conn_id);
  if (!p_clcb) return GATT_NO_RESOURCES;

  p_clcb->operation = GATTC_OPTYPE_WRITE;
  p_clcb->op_subtype = type;
  p_clcb->auth_req = p_write->auth_req;

  p_clcb->p_attr_buf = (uint8_t*)osi_malloc(sizeof(tGATT_VALUE));
  memcpy(p_clcb->p_attr_buf, (void*)p_write, sizeof(tGATT_VALUE));

  tGATT_VALUE* p = (tGATT_VALUE*)p_clcb->p_attr_buf;
  if (type == GATT_WRITE_PREPARE) {
    p_clcb->start_offset = p_write->offset;
    p->offset = 0;
  }

  if (gatt_security_check_start(p_clcb)) p_tcb->pending_enc_clcb.push(p_clcb);
  return GATT_SUCCESS;
}

/*******************************************************************************
 *
 * Function         GATTC_ExecuteWrite
 *
 * Description      This function is called to send an Execute write request to
 *                  the server.
 *
 * Parameters       conn_id: connection identifier.
 *                  is_execute - to execute or cancel the prepared write
 *                               request(s)
 *
 * Returns          GATT_SUCCESS if command started successfully.
 *
 ******************************************************************************/
tGATT_STATUS GATTC_ExecuteWrite(uint16_t conn_id, bool is_execute) {
  tGATT_IF gatt_if = GATT_GET_GATT_IF(conn_id);
  uint8_t tcb_idx = GATT_GET_TCB_IDX(conn_id);
  tGATT_TCB* p_tcb = gatt_get_tcb_by_idx(tcb_idx);
  tGATT_REG* p_reg = gatt_get_regcb(gatt_if);

  VLOG(1) << __func__ << ": conn_id=" << loghex(conn_id)
          << ", is_execute=" << +is_execute;

  if ((p_tcb == NULL) || (p_reg == NULL)) {
    LOG(ERROR) << " Illegal param: conn_id=" << loghex(conn_id);
    return GATT_ILLEGAL_PARAMETER;
  }

  if (gatt_is_clcb_allocated(conn_id)) {
    LOG(ERROR) << " GATT_BUSY conn_id=" << loghex(conn_id);
    return GATT_BUSY;
  }

  tGATT_CLCB* p_clcb = gatt_clcb_alloc(conn_id);
  if (!p_clcb) return GATT_NO_RESOURCES;

  p_clcb->operation = GATTC_OPTYPE_EXE_WRITE;
  tGATT_EXEC_FLAG flag =
      is_execute ? GATT_PREP_WRITE_EXEC : GATT_PREP_WRITE_CANCEL;
  if (p_clcb->p_tcb)
    gatt_send_queue_write_cancel(*p_clcb->p_tcb, p_clcb, flag);
  return GATT_SUCCESS;
}

/*******************************************************************************
 *
 * Function         GATTC_SendHandleValueConfirm
 *
 * Description      This function is called to send a handle value confirmation
 *                  as response to a handle value notification from server.
 *
 * Parameters       conn_id: connection identifier.
 *                  handle: the handle of the attribute confirmation.
 *
 * Returns          GATT_SUCCESS if command started successfully.
 *
 ******************************************************************************/
tGATT_STATUS GATTC_SendHandleValueConfirm(uint16_t conn_id, uint16_t handle,
                                          uint32_t trans_id) {
  VLOG(1) << __func__ << " conn_id=" << loghex(conn_id)
          << ", handle=" << loghex(handle);
  tGATT_EBCB* p_eatt_bcb;
  uint16_t lcid;

  tGATT_TCB* p_tcb = gatt_get_tcb_by_idx(GATT_GET_TCB_IDX(conn_id));
  if (!p_tcb) {
    LOG(ERROR) << "Unknown conn_id=" << loghex(conn_id);
    return GATT_ILLEGAL_PARAMETER;
  }

  if (p_tcb->ind_count == 0) {
    VLOG(1) << " conn_id: " << loghex(conn_id)
            << " ignored not waiting for indicaiton ack";
    return GATT_SUCCESS;
  }

  alarm_cancel(p_tcb->ind_ack_timer);

  VLOG(1) << "notif_count= " << p_tcb->ind_count;

  lcid = p_tcb->att_lcid;

  if (p_tcb->is_eatt_supported) {
    p_eatt_bcb = gatt_find_eatt_bcb_by_cl_trans_id(trans_id, p_tcb->peer_bda);
    if (p_eatt_bcb)
      lcid = p_eatt_bcb->cid;
    else
      return GATT_ILLEGAL_PARAMETER;
  }
  /* send confirmation now */
  tGATT_CL_MSG gatt_cl_msg;
  gatt_cl_msg.handle = handle;
  tGATT_STATUS ret =
      attp_send_cl_msg(*p_tcb, nullptr, lcid, GATT_HANDLE_VALUE_CONF, &gatt_cl_msg);

  p_tcb->ind_count = 0;

  return ret;
}

/******************************************************************************/
/*                                                                            */
/*                  GATT  APIs                                                */
/*                                                                            */
/******************************************************************************/
/*******************************************************************************
 *
 * Function         GATT_SetIdleTimeout
 *
 * Description      This function (common to both client and server) sets the
 *                  idle timeout for a tansport connection
 *
 * Parameter        bd_addr:   target device bd address.
 *                  idle_tout: timeout value in seconds.
 *
 * Returns          void
 *
 ******************************************************************************/
void GATT_SetIdleTimeout(const RawAddress& bd_addr, uint16_t idle_tout,
                         tBT_TRANSPORT transport, uint16_t lcid) {
  bool status = false;

  tGATT_TCB* p_tcb = gatt_find_tcb_by_addr(bd_addr, transport);
  if (p_tcb != NULL) {
    if(!p_tcb->is_eatt_supported) {
      lcid = p_tcb->att_lcid;
    }
    if (lcid == L2CAP_ATT_CID) {
      status = L2CA_SetFixedChannelTout(bd_addr, L2CAP_ATT_CID, idle_tout);

      if (idle_tout == GATT_LINK_IDLE_TIMEOUT_WHEN_NO_APP)
        L2CA_SetIdleTimeoutByBdAddr(p_tcb->peer_bda,
                                    GATT_LINK_IDLE_TIMEOUT_WHEN_NO_APP,
                                    BT_TRANSPORT_LE);
    } else {
      status = L2CA_SetIdleTimeout(lcid, idle_tout, false);
    }
  }

  VLOG(1) << __func__ << " idle_tout=" << idle_tout << ", status=" << +status
          << " (1-OK 0-not performed)";
}

/*******************************************************************************
 *
 * Function         GATT_Register
 *
 * Description      This function is called to register an  application
 *                  with GATT
 *
 * Parameter        p_app_uuid128: Application UUID
 *                  p_cb_info: callback functions.
 *
 * Returns          0 for error, otherwise the index of the client registered
 *                  with GATT
 *
 ******************************************************************************/
tGATT_IF GATT_Register(const Uuid& app_uuid128, tGATT_CBACK* p_cb_info, bool eatt_support) {
  tGATT_REG* p_reg;
  uint8_t i_gatt_if = 0;
  tGATT_IF gatt_if = 0;

  LOG(INFO) << __func__ << " " << app_uuid128;

  for (i_gatt_if = 0, p_reg = gatt_cb.cl_rcb; i_gatt_if < GATT_MAX_APPS;
       i_gatt_if++, p_reg++) {
    if (p_reg->in_use && p_reg->app_uuid128 == app_uuid128) {
      LOG(ERROR) << "application already registered.";
      return 0;
    }
  }

  for (i_gatt_if = 0, p_reg = gatt_cb.cl_rcb; i_gatt_if < GATT_MAX_APPS;
       i_gatt_if++, p_reg++) {
    if (!p_reg->in_use) {
      memset(p_reg, 0, sizeof(tGATT_REG));
      i_gatt_if++; /* one based number */
      p_reg->app_uuid128 = app_uuid128;
      gatt_if = p_reg->gatt_if = (tGATT_IF)i_gatt_if;
      p_reg->app_cb = *p_cb_info;
      p_reg->in_use = true;
      p_reg->eatt_support = eatt_support;

      LOG(INFO) << "allocated gatt_if=" << +gatt_if;
      return gatt_if;
    }
  }

  LOG(ERROR) << "can't Register GATT client, MAX client reached: "
             << GATT_MAX_APPS;
  return 0;
}

/*******************************************************************************
 *
 * Function         GATT_Deregister
 *
 * Description      This function deregistered the application from GATT.
 *
 * Parameters       gatt_if: applicaiton interface.
 *
 * Returns          None.
 *
 ******************************************************************************/
void GATT_Deregister(tGATT_IF gatt_if) {
  bool is_gatt_connected = false;
  tGATT_EBCB* p_eatt_bcb;
  uint16_t lcid;

  VLOG(1) << __func__ << " gatt_if=" << +gatt_if;
  
  tGATT_REG* p_reg = gatt_get_regcb(gatt_if);
  /* Index 0 is GAP and is never deregistered */
  if ((gatt_if == 0) || (p_reg == NULL)) {
    LOG(ERROR) << "invalid gatt_if=" << +gatt_if;
    return;
  }

  if(gatt_cb.srv_list_info->empty()) {
    VLOG(1) << __func__ << "srv list info is empty";
    return;
  }

  /* stop all services  */
  /* todo an applcaiton can not be deregistered if its services is also used by
    other application
    deregisteration need to bed performed in an orderly fashion
    no check for now */
  for (auto it = gatt_cb.srv_list_info->begin();
       it != gatt_cb.srv_list_info->end();) {
    if (it->gatt_if == gatt_if) {
      GATTS_StopService(it++->s_hdl);
    } else {
      ++it;
    }
  }

  /* free all services db buffers if owned by this application */
  gatt_free_srvc_db_buffer_app_id(p_reg->app_uuid128);

  /* When an application deregisters, check remove the link associated with the
   * app */
  tGATT_TCB* p_tcb;
  int i, j;
  for (i = 0, p_tcb = gatt_cb.tcb; i < GATT_MAX_PHY_CHANNEL; i++, p_tcb++) {
    if (!p_tcb->in_use) continue;
    if (p_tcb->in_use) {
      if (gatt_get_ch_state(p_tcb) != GATT_CH_CLOSE) {
        lcid = p_tcb->att_lcid;
        if(p_tcb->is_eatt_supported) {
          p_eatt_bcb = gatt_find_eatt_bcb_by_gatt_if(gatt_if, p_tcb->peer_bda);
          if (p_eatt_bcb)
            lcid = p_eatt_bcb->cid;
          else
            LOG(ERROR) << " EATT bearer not found";
        }

        is_gatt_connected = gatt_is_app_holding_link(gatt_if, p_tcb);
        gatt_update_app_use_link_flag(gatt_if, p_tcb, false, true);
        if (is_gatt_connected && (gatt_if > SYSTEM_APP_GATT_IF) && p_tcb->app_hold_link.empty())
        {
          /* this will disconnect the link or cancel the pending connect request at lower layer*/
          gatt_disconnect(p_tcb, lcid);
        }

        uint16_t conn_id = GATT_CREATE_CONN_ID(p_tcb->tcb_idx, gatt_if);
        gatt_remove_conn(conn_id, lcid);
      }
    }


    tGATT_CLCB* p_clcb;
    for (j = 0, p_clcb = &gatt_cb.clcb[j]; j < GATT_CL_MAX_LCB; j++, p_clcb++) {
      if (p_clcb->in_use && (p_clcb->p_reg->gatt_if == gatt_if) &&
          (p_clcb->p_tcb->tcb_idx == p_tcb->tcb_idx)) {
        alarm_cancel(p_clcb->gatt_rsp_timer_ent);
        gatt_clcb_dealloc(p_clcb);
        break;
      }
    }
  }

  connection_manager::on_app_deregistered(gatt_if);

  memset(p_reg, 0, sizeof(tGATT_REG));
}

/*******************************************************************************
 *
 * Function         GATT_StartIf
 *
 * Description      This function is called after registration to start
 *                  receiving callbacks for registered interface.  Function may
 *                  call back with connection status and queued notifications
 *
 * Parameter        gatt_if: applicaiton interface.
 *
 * Returns          None.
 *
 ******************************************************************************/
void GATT_StartIf(tGATT_IF gatt_if) {
  tGATT_REG* p_reg;
  tGATT_TCB* p_tcb;
  RawAddress bda;
  uint8_t start_idx, found_idx;
  uint16_t conn_id;
  tGATT_TRANSPORT transport;

  VLOG(1) << __func__ << " gatt_if=" << +gatt_if;
  p_reg = gatt_get_regcb(gatt_if);
  if (p_reg != NULL) {
    start_idx = 0;
    while (
        gatt_find_the_connected_bda(start_idx, bda, &found_idx, &transport)) {
      p_tcb = gatt_find_tcb_by_addr(bda, transport);
      if (p_reg->app_cb.p_conn_cb && p_tcb) {
        conn_id = GATT_CREATE_CONN_ID(p_tcb->tcb_idx, gatt_if);
        (*p_reg->app_cb.p_conn_cb)(gatt_if, bda, conn_id, true, 0, transport);
      }
      start_idx = ++found_idx;
    }
  }
}

/*******************************************************************************
 *
 * Function         GATT_Connect
 *
 * Description      This function initiate a connecttion to a remote device on
 *                  GATT channel.
 *
 * Parameters       gatt_if: applicaiton interface
 *                  bd_addr: peer device address.
 *                  is_direct: is a direct conenection or a background auto
 *                             connection
 *
 * Returns          true if connection started; false if connection start
 *                  failure.
 *
 ******************************************************************************/
bool GATT_Connect(tGATT_IF gatt_if, const RawAddress& bd_addr, bool is_direct,
                  tBT_TRANSPORT transport, bool opportunistic) {
  uint8_t phy = controller_get_interface()->get_le_all_initiating_phys();
  return GATT_Connect(gatt_if, bd_addr, is_direct, transport, opportunistic,
                      phy);
}

bool GATT_Connect(tGATT_IF gatt_if, const RawAddress& bd_addr, bool is_direct,
                  tBT_TRANSPORT transport, bool opportunistic,
                  uint8_t initiating_phys) {
  tGATT_TCB* p_tcb;
  tBTM_SEC_DEV_REC* p_dev_rec = btm_find_dev(bd_addr);
  LOG(INFO) << __func__ << " gatt_if=" << +gatt_if << ", address=" << bd_addr
    << " is_direct " << is_direct;

  /* Make sure app is registered */
  tGATT_REG* p_reg = gatt_get_regcb(gatt_if);
  if (!p_reg) {
    LOG(ERROR) << "gatt_if = " << +gatt_if << " is not registered";
    return false;
  }

  if (!is_direct && transport != BT_TRANSPORT_LE) {
    LOG(ERROR) << "Unsupported transport for background connection";
    return false;
  }

  if (BTM_GetLeDisconnectStatus(bd_addr)) {
    LOG(ERROR) << "Link Level Disconnection is progress. Wait for it";
    return false;
  }

  if (opportunistic) {
    LOG(INFO) << __func__ << " opportunistic connection";

    p_tcb = gatt_find_tcb_by_addr(bd_addr, transport);
    if (p_tcb && p_tcb->is_eatt_supported && p_reg->eatt_support) {
      gatt_find_best_eatt_bcb(p_tcb, gatt_if, 0, true);
    }

    return true;
  }

  bool ret;
  if (is_direct) {
    ret = gatt_act_connect(p_reg, bd_addr, transport, initiating_phys);
  } else {
    if (!BTM_BackgroundConnectAddressKnown(bd_addr)) {
      //  RPA can rotate, causing address to "expire" in the background
      //  connection list. RPA is allowed for direct connect, as such request
      //  times out after 30 seconds
      LOG(INFO) << "Can't add RPA to background connection.";
      ret = true;
    } else {
      ret = connection_manager::background_connect_add(gatt_if, bd_addr);
    }
  }

  p_tcb = gatt_find_tcb_by_addr(bd_addr, transport);
  // background connections don't necessarily create tcb
  if (p_tcb && ret)
    gatt_update_app_use_link_flag(p_reg->gatt_if, p_tcb, true, !is_direct);

  if (p_tcb && p_tcb->is_eatt_supported && p_reg->eatt_support
      && !is_direct) {
    std::deque<tGATT_IF>::iterator it =
        std::find(p_tcb->apps_needing_eatt.begin(), p_tcb->apps_needing_eatt.end(), gatt_if);
    if (it == p_tcb->apps_needing_eatt.end()) {
      p_tcb->apps_needing_eatt.push_back(gatt_if);
    }
    if (p_dev_rec && (p_dev_rec->sec_flags & BTM_SEC_LE_ENCRYPTED))
      gatt_establish_eatt_connect(p_tcb, 1);
  }

  return ret;
}

/*******************************************************************************
 *
 * Function         GATT_CancelConnect
 *
 * Description      This function terminate the connection initaition to a
 *                  remote device on GATT channel.
 *
 * Parameters       gatt_if: client interface. If 0 used as unconditionally
 *                           disconnect, typically used for direct connection
 *                           cancellation.
 *                  bd_addr: peer device address.
 *
 * Returns          true if the connection started; false otherwise.
 *
 ******************************************************************************/
bool GATT_CancelConnect(tGATT_IF gatt_if, const RawAddress& bd_addr,
                        bool is_direct) {
  LOG(INFO) << __func__ << ": gatt_if:" << +gatt_if << ", address: " << bd_addr
            << ", direct:" << is_direct;

  tGATT_REG* p_reg;
  if (gatt_if) {
    p_reg = gatt_get_regcb(gatt_if);
    if (!p_reg) {
      LOG(ERROR) << "gatt_if=" << +gatt_if << " is not registered";
      return false;
    }

    if (is_direct)
      return gatt_cancel_open(gatt_if, bd_addr);
    else
      return gatt_auto_connect_dev_remove(p_reg->gatt_if, bd_addr);
  }

  VLOG(1) << " unconditional";

  /* only LE connection can be cancelled */
  tGATT_TCB* p_tcb = gatt_find_tcb_by_addr(bd_addr, BT_TRANSPORT_LE);
  if (p_tcb && !p_tcb->app_hold_link.empty()) {
    for (auto it = p_tcb->app_hold_link.begin();
         it != p_tcb->app_hold_link.end();) {
      auto next = std::next(it);
      // gatt_cancel_open modifies the app_hold_link.
      gatt_cancel_open(*it, bd_addr);

      it = next;
    }
  }

  if (!connection_manager::remove_unconditional(bd_addr)) {
    LOG(ERROR)
        << __func__
        << ": no app associated with the bg device for unconditional removal";
    return false;
  }

  return true;
}

/*******************************************************************************
 *
 * Function         GATT_Disconnect
 *
 * Description      This function disconnects the GATT channel for this
 *                  registered application.
 *
 * Parameters       conn_id: connection identifier.
 *
 * Returns          GATT_SUCCESS if disconnected.
 *
 ******************************************************************************/
tGATT_STATUS GATT_Disconnect(uint16_t conn_id) {
  LOG(INFO) << __func__ << " conn_id=" << loghex(conn_id);

  uint8_t tcb_idx = GATT_GET_TCB_IDX(conn_id);
  tGATT_TCB* p_tcb = gatt_get_tcb_by_idx(tcb_idx);
  if (!p_tcb) return GATT_ILLEGAL_PARAMETER;

  tGATT_IF gatt_if = GATT_GET_GATT_IF(conn_id);
  gatt_update_app_use_link_flag(gatt_if, p_tcb, false, true);

  uint16_t lcid = gatt_get_cid_by_conn_id(conn_id);
  gatt_remove_conn(conn_id, lcid);

  return GATT_SUCCESS;
}

/*******************************************************************************
 *
 * Function         GATT_GetConnectionInfor
 *
 * Description      This function uses conn_id to find its associated BD address
 *                  and application interface
 *
 * Parameters        conn_id: connection id  (input)
 *                   p_gatt_if: applicaiton interface (output)
 *                   bd_addr: peer device address. (output)
 *
 * Returns          true the ligical link information is found for conn_id
 *
 ******************************************************************************/
bool GATT_GetConnectionInfor(uint16_t conn_id, tGATT_IF* p_gatt_if,
                             RawAddress& bd_addr, tBT_TRANSPORT* p_transport) {
  tGATT_IF gatt_if = GATT_GET_GATT_IF(conn_id);
  tGATT_REG* p_reg = gatt_get_regcb(gatt_if);
  uint8_t tcb_idx = GATT_GET_TCB_IDX(conn_id);
  tGATT_TCB* p_tcb = gatt_get_tcb_by_idx(tcb_idx);

  VLOG(1) << __func__ << " conn_id=" << loghex(conn_id);

  if (!p_tcb || !p_reg) return false;

  bd_addr = p_tcb->peer_bda;
  *p_gatt_if = gatt_if;
  *p_transport = p_tcb->transport;
  return true;
}

/*******************************************************************************
 *
 * Function         GATT_GetConnIdIfConnected
 *
 * Description      This function find the conn_id if the logical link for BD
 *                  address and applciation interface is connected
 *
 * Parameters        gatt_if: applicaiton interface (input)
 *                   bd_addr: peer device address. (input)
 *                   p_conn_id: connection id  (output)
 *                   transport: transport option
 *
 * Returns          true the logical link is connected
 *
 ******************************************************************************/
bool GATT_GetConnIdIfConnected(tGATT_IF gatt_if, const RawAddress& bd_addr,
                               uint16_t* p_conn_id, tBT_TRANSPORT transport) {
  tGATT_REG* p_reg = gatt_get_regcb(gatt_if);
  tGATT_TCB* p_tcb = gatt_find_tcb_by_addr(bd_addr, transport);
  bool status = false;

  if (p_reg && p_tcb && (gatt_get_ch_state(p_tcb) == GATT_CH_OPEN)) {
    *p_conn_id = GATT_CREATE_CONN_ID(p_tcb->tcb_idx, gatt_if);
    status = true;
  }

  VLOG(1) << __func__ << " status= " << +status;
  return status;
}

/*******************************************************************************
 *
 * Function         GATT_GetEattSupportIfConnected
 *
 * Description      This function checks if an app is using EATT or not.
 *
 * Parameters        gatt_if: applicaiton interface (input)
 *                   bd_addr: peer device address. (input)
 *                   p_conn_id: connection id  (output)
 *                   transport: transport option
 *
 * Returns          true, if local and remote support EATT and gatt_if also
 *                  requested for EATT.
 *                  false, otherwise.
 *
 ******************************************************************************/
bool GATT_GetEattSupportIfConnected(tGATT_IF gatt_if, const RawAddress& bd_addr,
                                    tBT_TRANSPORT transport) {
  tGATT_REG* p_reg = gatt_get_regcb(gatt_if);
  tGATT_TCB* p_tcb = gatt_find_tcb_by_addr(bd_addr, transport);
  bool status = false;

  if (transport != BT_TRANSPORT_LE)
    return false;

  if (p_reg && p_tcb && (gatt_get_ch_state(p_tcb) == GATT_CH_OPEN)) {
    if (p_tcb->is_eatt_supported && p_reg->eatt_support) {
      status = true;
    }
  }

  VLOG(1) << __func__ << " status= " << +status;
  return status;
}

/*******************************************************************************
 *
 * Function         GATT_GetMtuSize
 *
 * Description      This function find the conn_id if the logical link for BD
 *                  address and applciation interface is connected
 *
 * Parameters        gatt_if: applicaiton interface (input)
 *                   bd_addr: peer device address. (input)
 *                   p_conn_id: connection id  (output)
 *                   transport: transport option
 *
 * Returns          true the logical link is connected
 *
 ******************************************************************************/
uint16_t GATT_GetMtuSize(uint16_t conn_id, const RawAddress& bd_addr,
                         tBT_TRANSPORT transport) {
  tGATT_IF gatt_if = GATT_GET_GATT_IF(conn_id);
  tGATT_REG* p_reg = gatt_get_regcb(gatt_if);
  tGATT_TCB* p_tcb = gatt_find_tcb_by_addr(bd_addr, transport);
  uint16_t mtu = 0;
  uint16_t lcid = L2CAP_ATT_CID;

  if (p_reg && p_tcb && (gatt_get_ch_state(p_tcb) == GATT_CH_OPEN)) {
    if (p_tcb->is_eatt_supported && p_reg->eatt_support) {
      lcid = gatt_get_cid_by_conn_id(conn_id);
    }
    mtu = gatt_get_payload_size(p_tcb, lcid);
  }

  VLOG(1) << __func__ << " mtu: " << +mtu;
  return mtu;
}

/*******************************************************************************
 *
 * Function         GATT_CheckStatusForApp
 *
 * Description      This function checks if an app has an indication or
 *                  notification already queued because of no credits.
 *                  If yes, GATT_BUSY is returned
 *                  else, this notification/indication is sent to lower layer.
 *
 * Parameters       conn_id: conn_id
 *                  confirm: confirm true for indication, false for notification
 *
 * Returns          tGATT_STATUS.
 *
 ******************************************************************************/
tGATT_STATUS GATTS_CheckStatusForApp(uint16_t conn_id, bool confirm) {
  tGATT_IF gatt_if = GATT_GET_GATT_IF(conn_id);
  uint8_t tcb_idx = GATT_GET_TCB_IDX(conn_id);
  tGATT_REG* p_reg = gatt_get_regcb(gatt_if);
  tGATT_TCB* p_tcb = gatt_get_tcb_by_idx(tcb_idx);
  uint16_t lcid = 0;
  tGATT_EBCB* p_eatt_bcb = NULL;
  tGATT_STATUS status = GATT_SUCCESS;

  VLOG(1) << __func__;

  if ((p_reg == NULL) || (p_tcb == NULL)) {
    LOG(ERROR) << __func__ << "Unknown  conn_id: " << conn_id;
    return (tGATT_STATUS)GATT_INVALID_CONN_ID;
  }
  if (!p_tcb->is_eatt_supported || !p_reg->eatt_support) {
    return status;
  }

  lcid = p_tcb->att_lcid;
  if (p_tcb->is_eatt_supported && p_reg->eatt_support) {
    if (is_gatt_conn_id_found(conn_id)) {
      lcid = gatt_get_cid_by_conn_id(conn_id);
      p_eatt_bcb = gatt_find_eatt_bcb_by_cid(p_tcb, lcid);
    }
  }

  if (confirm) {
    if (p_tcb->is_eatt_supported && p_eatt_bcb && p_eatt_bcb->no_credits &&
      std::find(p_eatt_bcb->ind_no_credits_apps.begin(),
      p_eatt_bcb->ind_no_credits_apps.end(),
      conn_id) != p_eatt_bcb->ind_no_credits_apps.end()) {
      LOG(ERROR) << __func__ << "Multiple indications from same conn_id when congested:" << conn_id;
      return GATT_BUSY;
    }
  }
  else {
    if (p_tcb->is_eatt_supported && p_eatt_bcb && p_eatt_bcb->no_credits &&
        std::find(p_eatt_bcb->notif_no_credits_apps.begin(),
        p_eatt_bcb->notif_no_credits_apps.end(),
        conn_id) != p_eatt_bcb->notif_no_credits_apps.end()) {
      LOG(ERROR) << __func__ << "Multiple notifications from same conn_id when congested:" << conn_id;
      return GATT_BUSY;
    }
  }

  VLOG(1) << __func__ << " status= " << +status;
  return status;
}
