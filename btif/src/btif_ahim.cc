/******************************************************************************
 * Copyright (c) 2020, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 ******************************************************************************/
/*
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.

    * Redistribution and use in source and binary forms, with or without
      modification, are permitted (subject to the limitations in the
      disclaimer below) provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.

    * Redistributions in binary form must reproduce the above
      copyright notice, this list of conditions and the following
      disclaimer in the documentation and/or other materials provided
      with the distribution.

    * Neither the name of Qualcomm Innovation Center, Inc. nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE
GRANTED BY THIS LICENSE. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT
HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE
*/

/*
 * Changes from Qualcomm Innovation Center are provided under the following license:
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <mutex>

#include "audio_hal_interface/a2dp_encoding.h"
#include "audio_hal_interface/aidl/a2dp_encoding.h"
#include "audio_hal_interface/aidl/le_audio_software.h"
#include <hardware/audio.h>
#include <vector>
#ifdef ADV_AUDIO_FEATURE
#include <hardware/bt_pacs_client.h>
#include <btif_vmcp.h>
#endif
#include <aidl/vendor/qti/hardware/bluetooth/audio/LeAudioVendorConfiguration.h>
#include <aidl/vendor/qti/hardware/bluetooth/audio/VendorCodecType.h>

using bluetooth::audio::aidl::le_audio::LeAudioClientInterface;

using AudioConfigurationAIDL =
    ::aidl::android::hardware::bluetooth::audio::AudioConfiguration;

using ChannelModeAIDL =
	::aidl::android::hardware::bluetooth::audio::ChannelMode;
using ::aidl::android::hardware::bluetooth::audio::CodecType;
using ::aidl::android::hardware::bluetooth::audio::LeAudioBroadcastConfiguration;
using ::aidl::android::hardware::bluetooth::audio::LeAudioConfiguration;
using SessionTypeAIDL = ::aidl::android::hardware::bluetooth::audio::SessionType;
using ::aidl::android::hardware::bluetooth::audio::LeAudioCodecConfiguration;
using ::aidl::android::hardware::bluetooth::audio::Lc3Configuration;
using ::aidl::android::hardware::bluetooth::audio::CodecType;
using ::aidl::vendor::qti::hardware::bluetooth::audio::LeAudioVendorConfiguration;
using VendorCodecType =
    ::aidl::vendor::qti::hardware::bluetooth::audio::VendorCodecType;
using VendorConfiguration =
    ::aidl::android::hardware::bluetooth::audio::LeAudioCodecConfiguration::VendorConfiguration;
using ::aidl::android::hardware::bluetooth::audio::UnicastCapability;
using vendor::qti::hardware::bluetooth_audio::V2_1::LC3ChannelMode;
#ifdef ADV_AUDIO_FEATURE
using bluetooth::bap::pacs::CodecIndex;
#endif
//using ::bluetooth::audio::BitsPerSample;
LeAudioClientInterface* leAudioClientInterface = nullptr;
LeAudioClientInterface::Sink* unicastSinkClientInterface = nullptr;
LeAudioClientInterface::Sink* broadcastSinkClientInterface = nullptr;
LeAudioClientInterface::Source* unicastSourceClientInterface = nullptr;

std::mutex src_metadata_wait_mutex_;
std::condition_variable src_metadata_wait_cv;
bool src_metadata_wait;

std::mutex snk_metadata_wait_mutex_;
std::condition_variable snk_metadata_wait_cv;
bool snk_metadata_wait;
bool is_split_sink_enabled;
bool is_split_sink_hidl_checked;

#if AHIM_ENABLED

uint8_t cur_active_profile = A2DP;
std::mutex active_profile_mtx;
std::mutex session_mtx;

#define BAP        0x01
#define GCP        0x02
#define WMCP       0x04
#define TMAP       0x08
#define BAP_CALL   0x10
#define GCP_RX     0x20
#define GCP_TX     0x40
#define WMCP_TX    0x80
#define WMCP_RX    0x100

btif_ahim_client_callbacks_t* pclient_cbs[MAX_CLIENT] = {NULL};

void btif_ahim_signal_src_metadata_complete() {
  std::unique_lock<std::mutex> guard(src_metadata_wait_mutex_);
  if(!src_metadata_wait) {
    src_metadata_wait = true;
    LOG(INFO) << __func__ << ": Signalling";
    src_metadata_wait_cv.notify_all();
  } else {
    LOG(INFO) << __func__ << ": Already signalled";
  }
}

void btif_ahim_signal_snk_metadata_complete() {
  std::unique_lock<std::mutex> guard(snk_metadata_wait_mutex_);
  if(!snk_metadata_wait) {
    snk_metadata_wait = true;
    LOG(INFO) << __func__ << ": Signalling";
    snk_metadata_wait_cv.notify_all();
  } else {
    LOG(INFO) << __func__ << ": Already signalled";
  }
}

void reg_cb_with_ahim(uint8_t client_id,
                     btif_ahim_client_callbacks_t* pclient_cb)
{
  LOG(INFO) << __func__ << "Registering callback for client id "
            << client_id << " with AHIM";
  // No need to register call back for A2DP.
   if (client_id <= A2DP|| client_id >= MAX_CLIENT)
   {
      // invalid call back registration. return.
      BTIF_TRACE_ERROR("%s, invalid client id %u", __func__, client_id);
      return;
   }
   pclient_cbs[client_id - 1] = pclient_cb;
}

void btif_ahim_update_current_profile(uint8_t profile)
{
  std::lock_guard<std::mutex> lock(active_profile_mtx);

  switch(profile)
  {
     case A2DP:
       FALLTHROUGH;
     case AUDIO_GROUP_MGR:
       FALLTHROUGH;
     case BROADCAST:
       cur_active_profile = profile;
       break;
     default:
       LOG(INFO) << __func__ << ": Unsupported active profile, resetting to A2DP";
       cur_active_profile = A2DP;
       break;
  }

  LOG(INFO) << __func__ << ": Current active profile: "<< cur_active_profile;
}
void btif_ahim_process_request(tA2DP_CTRL_CMD cmd, uint8_t profile, 
                               uint8_t direction) {
  std::lock_guard<std::mutex> lock(active_profile_mtx);
  if (btif_ahim_is_aosp_aidl_hal_enabled()) {
    cur_active_profile = profile;
  }
  switch(cur_active_profile)  {
    case A2DP:
      BTIF_TRACE_IMP("%s: sending AIDL request to AV", __func__);
      btif_dispatch_sm_event(BTIF_AV_PROCESS_HIDL_REQ_EVT,
                             (char*)&cmd, sizeof(cmd));
      break;
    case AUDIO_GROUP_MGR:
      BTIF_TRACE_IMP("%s: sending AIDL request to Audio Group Manager",
                     __func__);
      if (pclient_cbs[cur_active_profile - 1] &&
          pclient_cbs[cur_active_profile - 1]->client_cb) {
        BTIF_TRACE_IMP("%s: calling call back for Audio Group Manager",
                       __func__);
        pclient_cbs[cur_active_profile - 1]->client_cb(cmd, direction);
      }
      else
        BTIF_TRACE_ERROR("%s, Audio Group Manager is not registered with AHIM",
                          __func__);
      break;
    case BROADCAST:
      BTIF_TRACE_IMP("%s: sending AIDL request to BROADCAST", __func__);
      if (pclient_cbs[cur_active_profile - 1] &&
          pclient_cbs[cur_active_profile - 1]->client_cb) {
        BTIF_TRACE_IMP("%s: calling call back for BROADCAST", __func__);
        pclient_cbs[cur_active_profile - 1]->client_cb(cmd, direction);
      }
      else
        BTIF_TRACE_ERROR("%s, BROADCAST is not registered with AHIM", __func__);
      break;
  }
}

void btif_ahim_update_src_metadata (const source_metadata_t& source_metadata) {
  auto track_count = source_metadata.track_count;
  auto usage = source_metadata.tracks->usage;

  LOG(INFO) << __func__ << ", track_count: " << track_count
                        << ", usage: " << usage;

  // pass on the callbacks to ACM only for new vendor
  if(btif_ahim_is_aosp_aidl_hal_enabled()) {
    BTIF_TRACE_IMP("%s: sending AIDL request to Audio Group Manager", __func__);
    if (pclient_cbs[AUDIO_GROUP_MGR - 1] &&
        pclient_cbs[AUDIO_GROUP_MGR - 1]->src_meta_update) {
      BTIF_TRACE_IMP("%s: calling call back for Audio Group Manager", __func__);
      std::unique_lock<std::mutex> guard(src_metadata_wait_mutex_);
      src_metadata_wait = false;
      pclient_cbs[AUDIO_GROUP_MGR - 1]->src_meta_update(source_metadata);
      src_metadata_wait_cv.wait_for(guard, std::chrono::milliseconds(3200),
                        []{return src_metadata_wait;});
      BTIF_TRACE_IMP("%s: src waiting completed", __func__);
    }
  }
}

void btif_ahim_update_params (uint16_t delay, uint8_t mode) {
  // pass on the callbacks to ACM only for new vendor
  if(btif_ahim_is_aosp_aidl_hal_enabled()) {
    if (pclient_cbs[AUDIO_GROUP_MGR - 1] &&
        pclient_cbs[AUDIO_GROUP_MGR - 1]->params_update) {
      BTIF_TRACE_IMP("%s: calling updateParams for Audio Group Manager", __func__);
      pclient_cbs[AUDIO_GROUP_MGR - 1]->params_update(delay, mode);
    }
  }
}

void btif_ahim_update_sink_metadata (const sink_metadata_t& sink_metadata) {
  auto track_count = sink_metadata.track_count;
  auto source = sink_metadata.tracks->source;

  LOG(INFO) << __func__ << ", track_count: " << track_count
                        << ", source: " << source;

  // pass on the callbacks to ACM only for new vendor
  if(btif_ahim_is_aosp_aidl_hal_enabled()) {
    BTIF_TRACE_IMP("%s: sending AIDL request to Audio Group Manager", __func__);
    if(pclient_cbs[AUDIO_GROUP_MGR - 1] &&
       pclient_cbs[AUDIO_GROUP_MGR - 1]->snk_meta_update) {
      BTIF_TRACE_IMP("%s: calling call back for Audio Group Manager", __func__);
      std::unique_lock<std::mutex> guard(snk_metadata_wait_mutex_);
      snk_metadata_wait = false;
      pclient_cbs[AUDIO_GROUP_MGR - 1]->snk_meta_update(sink_metadata);
      snk_metadata_wait_cv.wait_for(guard, std::chrono::milliseconds(3200),
                        []{return snk_metadata_wait;});
      BTIF_TRACE_IMP("%s: snk waiting completed", __func__);
    }
  }
}

bool btif_ahim_init_hal(thread_t *t, uint8_t profile) {
  if(btif_ahim_is_aosp_aidl_hal_enabled()) {
    std::lock_guard<std::mutex>lock(session_mtx);
    BTIF_TRACE_IMP("%s: AIDL", __func__);
    if (profile == A2DP) {
        if (unicastSinkClientInterface != nullptr) {
          leAudioClientInterface->ReleaseSink(unicastSinkClientInterface);
          unicastSinkClientInterface = nullptr;
        }
        if (unicastSourceClientInterface != nullptr) {
          leAudioClientInterface->ReleaseSource(unicastSourceClientInterface);
          unicastSourceClientInterface = nullptr;
        }
        if (broadcastSinkClientInterface != nullptr) {
          leAudioClientInterface->ReleaseSink(broadcastSinkClientInterface);
          broadcastSinkClientInterface = nullptr;
        }
      return bluetooth::audio::aidl::a2dp::init(t);
    } else {
      if(leAudioClientInterface == nullptr) {
        leAudioClientInterface = LeAudioClientInterface::Get();
        if (leAudioClientInterface == nullptr) {
          LOG(ERROR) << __func__ << ", can't get LE audio client interface";
          return false;
        }
      }
      bluetooth::audio::aidl::le_audio::StreamCallbacks stream_cb;
      if (profile == AUDIO_GROUP_MGR) {
        if (unicastSinkClientInterface == nullptr)
          unicastSinkClientInterface = leAudioClientInterface->GetSink(stream_cb, t, false);
        if (unicastSourceClientInterface == nullptr)
          unicastSourceClientInterface = leAudioClientInterface->GetSource(stream_cb, t);
      } else if (profile == BROADCAST) {
        BTIF_TRACE_IMP("%s: Init AIDL interface for broadcast session", __func__);
        if (broadcastSinkClientInterface == nullptr)
          broadcastSinkClientInterface = leAudioClientInterface->GetSink(stream_cb, t, true);
        btif_ahim_setup_codec(BROADCAST);
      }
    }
  } else if(btif_ahim_is_qc_hal_enabled()) {
    BTIF_TRACE_IMP("%s: QC", __func__);
    return bluetooth::audio::a2dp::init(t, profile);
  }
  return true;
}

void btif_ahim_cleanup_hal(uint8_t profile) {
  if (btif_ahim_is_aosp_aidl_hal_enabled()) {
    if (profile == A2DP) {
      bluetooth::audio::aidl::a2dp::cleanup();
    } else {
      if(leAudioClientInterface == nullptr) {
        leAudioClientInterface = LeAudioClientInterface::Get();
        if (leAudioClientInterface == nullptr) {
          LOG(ERROR) << __func__ << ", can't get LE audio client interface";
          return;
        }
      }
      if (profile == AUDIO_GROUP_MGR) {
        bluetooth::audio::aidl::a2dp::cleanup(); //leaudio session path needs to be added
      } else if (profile == BROADCAST) {
        leAudioClientInterface->ReleaseSink(broadcastSinkClientInterface);
        broadcastSinkClientInterface = nullptr;
      }
    }
  } else if (btif_ahim_is_qc_hal_enabled()) {
    bluetooth::audio::a2dp::cleanup();
  }
}

bool btif_ahim_is_hal_2_0_supported() {
  if (btif_ahim_is_aosp_aidl_hal_enabled()) {
    return true;
  }
  return bluetooth::audio::a2dp::is_hal_2_0_supported();
}

bool btif_ahim_is_qc_hal_enabled() {
  return bluetooth::audio::a2dp::is_qc_hal_enabled();
}

bool btif_ahim_is_qc_lea_enabled() {
  return bluetooth::audio::a2dp::is_qc_lea_enabled();
}

bool btif_ahim_is_aosp_aidl_hal_enabled() {
  if (!is_split_sink_hidl_checked) {
    char a2dp_role[255] = "false";
    osi_property_get("persist.vendor.service.bt.a2dp.sink", a2dp_role, "false");
    if (!strncmp("true", a2dp_role, 4)) {
      is_split_sink_enabled = true;
    }
    is_split_sink_hidl_checked = true;
  }
  if(is_split_sink_enabled) {
    BTIF_TRACE_ERROR("Disabling aidl as splitsink is enabled");
    return false;
  } else {
    return bluetooth::audio::aidl::a2dp::is_aidl_hal_available();
  }
}

bool btif_ahim_is_hal_2_0_enabled() {
  // this API is to differentiate the communication between Audio HAL to BT stack
  // is over HIDL/AIDL or legacy way.
  // this API will return true in case of QC HIDL/AOSP AIDL communication
  if ((btif_ahim_is_aosp_aidl_hal_enabled()) ||
      (btif_ahim_is_qc_hal_enabled())) {
    return true;
  } else {
    return false;
  }
}

bool btif_ahim_update_codec_offloading_capabilities(
    const std::vector<btav_a2dp_codec_config_t>& framework_preference) {
  if (btif_ahim_is_aosp_aidl_hal_enabled()) {
    return bluetooth::audio::aidl::a2dp::
    update_codec_offloading_capabilities(framework_preference);
  }
  return false;
}

bool btif_ahim_is_restart_session_needed(uint8_t profile) {
  // currently this function is used only by the a2dp so for other profiles
  // it will return false
  if (btif_ahim_is_aosp_aidl_hal_enabled()) {
    if (profile == A2DP) {
      return bluetooth::audio::aidl::a2dp::is_restart_session_needed();
    } else if ((profile == AUDIO_GROUP_MGR)  || (profile == BROADCAST)) {
      return true;
    }
  } else if (btif_ahim_is_qc_hal_enabled()) { //hal 2_2 or aidl
    return bluetooth::audio::a2dp::is_restart_session_needed();
  }
  return false;
}

void btif_ahim_update_session_params(SessionParamType param_type) {

  if ((btif_ahim_is_aosp_aidl_hal_enabled()) && (cur_active_profile == A2DP)) {
    bluetooth::audio::aidl::a2dp::update_session_params((uint8_t)param_type);
  } else if (btif_ahim_is_qc_hal_enabled() && cur_active_profile == A2DP) {
    bluetooth::audio::a2dp::update_session_params(param_type);
  }
}

int btif_lc3_sample_rate(uint16_t rate) {
  LOG(ERROR) << __func__
               << ": sample_rate: " << rate;
  switch (rate) {
    case ADV_AUD_CODEC_SAMPLE_RATE_44100:
      return 44100;
    case ADV_AUD_CODEC_SAMPLE_RATE_48000:
      return 48000;
    case ADV_AUD_CODEC_SAMPLE_RATE_32000:
      return 32000;
    case ADV_AUD_CODEC_SAMPLE_RATE_24000:
      return 24000;
    case ADV_AUD_CODEC_SAMPLE_RATE_16000:
      return 16000;
    case ADV_AUD_CODEC_SAMPLE_RATE_8000:
      return 8000;
    case ADV_AUD_CODEC_SAMPLE_RATE_88200:
      return 88200;
    case ADV_AUD_CODEC_SAMPLE_RATE_96000:
      return 96000;
    case ADV_AUD_CODEC_SAMPLE_RATE_176400:
      return 176400;
    case ADV_AUD_CODEC_SAMPLE_RATE_192000:
      return 192000;
    default:
      return 8000;
  }
}

ChannelModeAIDL btif_lc3_aidl_channel_mode(uint8_t mode) {
BTIF_TRACE_IMP("%s:", __func__);
  switch (mode) {
    case BTAV_A2DP_CODEC_CHANNEL_MODE_MONO:
      return ChannelModeAIDL::MONO;
    case BTAV_A2DP_CODEC_CHANNEL_MODE_STEREO:
      return ChannelModeAIDL::STEREO;
    case 4://to be defined common to bap
      return ChannelModeAIDL::UNKNOWN;
    case 8://to be defined common to bap
      return ChannelModeAIDL::UNKNOWN;
    default:
      return ChannelModeAIDL::UNKNOWN;
  }
}

LC3ChannelMode btif_lc3_channel_mode(uint8_t mode) {
BTIF_TRACE_IMP("%s: mode: %d", __func__, mode);
  switch (mode) {
    case BTAV_A2DP_CODEC_CHANNEL_MODE_MONO:
      return LC3ChannelMode::MONO;
    case BTAV_A2DP_CODEC_CHANNEL_MODE_STEREO:
      return LC3ChannelMode::STEREO;
    case 4://to be defined common to bap
      return LC3ChannelMode::JOINT_STEREO;
    case 8://to be defined common to bap
      return LC3ChannelMode::CH_5_1;
    default:
      return LC3ChannelMode::UNKNOWN;
  }
}

LeAudioBroadcastConfiguration fetch_offload_broadcast_audio_config() {
  BTIF_TRACE_IMP("%s: ", __func__);

  int numBises = pclient_cbs[BROADCAST - 1]->get_ch_count_cb();
  uint32_t bit_rate = pclient_cbs[BROADCAST - 1]->get_bitrate_cb(TX_ONLY_CONFIG);
  uint16_t sample_rate = pclient_cbs[BROADCAST - 1]->get_sample_rate_cb(TX_ONLY_CONFIG);
  uint16_t frame_duration = pclient_cbs[BROADCAST - 1]->get_frame_length_cb(TX_ONLY_CONFIG);
  uint32_t mtu = pclient_cbs[BROADCAST - 1]->get_mtu_cb(bit_rate, TX_ONLY_CONFIG);
  uint8_t channel_mode = pclient_cbs[BROADCAST - 1]->get_channel_mode_cb(TX_ONLY_CONFIG);

  LeAudioBroadcastConfiguration bcast_config = {
      .codecType = CodecType::LC3};

  for (int i = 0; i < numBises; i++) {
    Lc3Configuration lc3_config = {
        .pcmBitDepth = 24,
        .samplingFrequencyHz = btif_lc3_sample_rate(sample_rate),
        .frameDurationUs = static_cast<int32_t>(frame_duration),
        .octetsPerFrame = static_cast<int32_t>(mtu),
        .blocksPerSdu = 1,
        .channelMode = btif_lc3_aidl_channel_mode(channel_mode)
    };

    int channel_allocation;
    if (lc3_config.channelMode == ChannelModeAIDL::STEREO) {
      channel_allocation = (CHANNEL_FL + (i % 2));
    } else {
      channel_allocation = CHANNEL_MONO;
    }

    bcast_config.streamMap.push_back({
        .streamHandle = static_cast<char16_t>(i),
        .audioChannelAllocation = channel_allocation,
        .leAudioCodecConfig = LeAudioCodecConfiguration(lc3_config)
    });
  }
  return bcast_config;
}

LeAudioConfiguration fetch_offload_audio_config(int profile, int direction) {
  BTIF_TRACE_IMP("%s: profile: %d, direction: %d", __func__, profile, direction);
  uint8_t cis_count = 2;
  LC3ChannelMode ch_mode = btif_lc3_channel_mode(
      pclient_cbs[profile - 1]->get_channel_mode_cb(direction));

  BTIF_TRACE_IMP("%s: ch_mode: %d", __func__, static_cast<uint16_t>(ch_mode));

  if (ch_mode == LC3ChannelMode::JOINT_STEREO ||
      ch_mode == LC3ChannelMode::MONO) {
    cis_count = 1;
  }

  uint16_t type = pclient_cbs[profile - 1]->get_profile_status_cb();
  uint16_t frame_duration = pclient_cbs[profile - 1]->get_frame_length_cb(direction);

#ifdef ADV_AUDIO_FEATURE
  bool is_lc3q_supported = false;
  CodecIndex codec_type = (CodecIndex) pclient_cbs[profile - 1]->get_codec_type(direction);
  if (codec_type == CodecIndex::CODEC_INDEX_SOURCE_APTX_ADAPTIVE_LE ||
      codec_type == CodecIndex::CODEC_INDEX_SOURCE_APTX_ADAPTIVE_R4) {
    frame_duration = pclient_cbs[profile - 1]->get_frame_duration(direction);
    LOG(ERROR) << __func__ << ": fetch frame duration: "
               << frame_duration << ", from leaudio_configs.xml";
  }
  uint8_t encoder_version = 0;
/*if (codec_type == CodecIndex::CODEC_INDEX_SOURCE_APTX_ADAPTIVE_LE ||
      pclient_cbs[profile - 1]->get_is_codec_type_lc3q(direction)) {*/
    VendorConfiguration vendor_config;
    LeAudioVendorConfiguration le_vendor_config;

    le_vendor_config.pcmBitDepth = 24;
    le_vendor_config.samplingFrequencyHz = btif_lc3_sample_rate(
                    pclient_cbs[profile - 1]->get_sample_rate_cb(direction));
    le_vendor_config.frameDurationUs = (int) frame_duration;
    LOG(ERROR) << __func__ << ": le_vendor_config.frameDurationUs: "
                           << loghex(le_vendor_config.frameDurationUs);
    le_vendor_config.octetsPerFrame =
                    (int) pclient_cbs[profile - 1]->get_mtu_cb(0, direction);
    le_vendor_config.blocksPerSdu = 1;
    le_vendor_config.codecSpecificData =
                    { 0x0F, //Vendor Metadata Length
                      0xFF, //Vendor META data type
                      0x0A, //Qtil ID
                      0x00,
                      0x0B, //Vendor Metadata length
                      0x10, //LC3Q Type
                      pclient_cbs[profile - 1]->get_codec_encoder_version(direction),
                      pclient_cbs[profile - 1]->get_codec_decoder_version(direction),
                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 // RFU
                    };
    encoder_version = pclient_cbs[profile - 1]->get_codec_encoder_version(direction);
    LOG(ERROR) << __func__ << ": codec negotiated encoder version: "
                           << loghex(encoder_version);
    if (codec_type == CodecIndex::CODEC_INDEX_SOURCE_APTX_ADAPTIVE_LE ||
        codec_type == CodecIndex::CODEC_INDEX_SOURCE_APTX_ADAPTIVE_R4) {
      le_vendor_config.codecSpecificData[5] = 0x11; // Aptx Adaptive Type
      le_vendor_config.codecSpecificData[8] = frame_duration;
      le_vendor_config.codecSpecificData[9] = pclient_cbs[profile - 1]->get_feature_map(direction);
      if(codec_type == CodecIndex::CODEC_INDEX_SOURCE_APTX_ADAPTIVE_LE) {
        le_vendor_config.codecSpecificData[6] = 0;
        le_vendor_config.codecSpecificData[7] = pclient_cbs[profile - 1]->get_codec_version_aptx(direction);
      }
      if (codec_type == CodecIndex::CODEC_INDEX_SOURCE_APTX_ADAPTIVE_R4) {
        le_vendor_config.vendorCodecType = VendorCodecType::APTX_ADAPTIVE_R4;
        LOG(ERROR) << __func__ << ": AptX R4 metadata params are updated";
        for (int i = 0; i < 4; i++) {
          le_vendor_config.codecSpecificData.push_back((pclient_cbs[profile - 1]->get_mode_cb() &
          (0xff <<((3 - i)*8))) >> ((3 - i)*8));
        }
      } else {
        le_vendor_config.vendorCodecType = VendorCodecType::APTX_ADAPTIVE_R3;
          LOG(ERROR) << __func__ << ": AptX LE metadata params are updated";
      }
    } else {
      is_lc3q_supported = true;
      le_vendor_config.vendorCodecType = VendorCodecType::LC3Q;
      LOG(ERROR) << __func__ << ": Lc3q params are updated";
    }

    for (int i = 0; i < 16; i++) {
      BTIF_TRACE_IMP("%s: AIDL, extension metadata [%d]: %d", __func__, i,
                      le_vendor_config.codecSpecificData[i]);
    }

    vendor_config.extension.setParcelable(le_vendor_config);

    int32_t latency = 0;
    if (direction == RX_ONLY_CONFIG) {
      if (unicastSourceClientInterface != nullptr) {
        latency = unicastSourceClientInterface->GetRemoteDelay();
      }
    } else {
      if (unicastSinkClientInterface != nullptr) {
        latency = unicastSinkClientInterface->GetRemoteDelay();
      }
    }
    // TODO to fill the right PD
    LeAudioConfiguration ucast_config = {
       .codecType = CodecType::VENDOR,
       .peerDelayUs = latency,
       .leAudioCodecConfig = LeAudioCodecConfiguration(vendor_config)
    };

    LOG(ERROR) << __func__ << ": type :" << type << ", direction :" << direction;
    bool is_mono_mic_channel_config = false;
    if ((direction == TX_RX_BOTH_CONFIG) && (type == GCP_RX) && is_lc3q_supported) {
      encoder_version = pclient_cbs[profile - 1]->get_codec_encoder_version(TX_ONLY_CONFIG);
      LOG(ERROR) << __func__ << ": Encoder Version: " << loghex(encoder_version);
      if (encoder_version == LC3Q_CODEC_FT_CHANGE_SUPPORTED_VERSION) {
        is_mono_mic_channel_config = true;
        LOG(ERROR) << __func__ << ": Mono config channel set to true";
      }
    }

    for (int i = 0; i < cis_count; i++) {
      int channel = CHANNEL_MONO;
      if (ch_mode == LC3ChannelMode::STEREO) {
        channel = pclient_cbs[profile - 1]->get_audio_location(i%2, direction);
        LOG(ERROR) << __func__ << ": channel: " << channel
                   << ", is_mono_mic_channel_config: " << is_mono_mic_channel_config;
        if (is_mono_mic_channel_config) {
          channel = CHANNEL_MONO;
          LOG(ERROR) << __func__ << ": Set Mono config channel";
        }
      } else if (ch_mode == LC3ChannelMode::JOINT_STEREO){
          LOG(ERROR) << __func__ << ": Set Stereo config channel";
          channel = (CHANNEL_FL | CHANNEL_FR);
      } else if (ch_mode == LC3ChannelMode::MONO) {
          LOG(ERROR) << __func__ << ": Set Mono config Channel";
          channel = CHANNEL_MONO;
      }
      LOG(ERROR) << __func__ << ": channel location: " << channel;
      ucast_config.streamMap.push_back({
          .streamHandle = static_cast<char16_t>(i),
          .audioChannelAllocation = channel,
      });
      if (is_mono_mic_channel_config) {
        LOG(ERROR) << __func__ << ": Set Mono config channel and break";
        break;
      }
    }
    return ucast_config;
#else /* ADV_AUDIO_FEATURE */
    // TODO to fill the right PD
    Lc3Configuration lc3_config = {
                  .pcmBitDepth = 24,
                  .samplingFrequencyHz = btif_lc3_sample_rate(
                   pclient_cbs[profile - 1]->get_sample_rate_cb(direction)),
                  .frameDurationUs = (int) frame_duration,
                  .octetsPerFrame =
                  (int) pclient_cbs[profile - 1]->get_mtu_cb(0, direction),
                  .blocksPerSdu = 1
                };

    int32_t latency = 0;
    if (direction == RX_ONLY_CONFIG) {
      if (unicastSourceClientInterface != nullptr) {
        latency = unicastSourceClientInterface->GetRemoteDelay();
      }
    } else {
      if (unicastSinkClientInterface != nullptr) {
        latency = unicastSinkClientInterface->GetRemoteDelay();
      }
    }
    // TODO to fill the right PD
    LeAudioConfiguration ucast_config = {
       .codecType = CodecType::LC3,
       .peerDelayUs = latency,
       .leAudioCodecConfig = LeAudioCodecConfiguration(lc3_config)
    };

    for (int i = 0; i < cis_count; i++) {
      ucast_config.streamMap.push_back({
          .streamHandle = static_cast<char16_t>(i),
          .audioChannelAllocation = (CHANNEL_FL + (i%2)),
      });
    }
    return ucast_config;
#endif /* ADV_AUDIO_FEATURE */
}

bool leAudio_get_selected_hal_codec_config(AudioConfigurationAIDL *lea_config,
                                           int profile, int direction) {
  LOG(INFO) << __func__ << ": profile: " << profile
                        << ": direction: " << direction;
  int cis_count = 2;
  if (profile == BROADCAST) {
    lea_config->set<AudioConfigurationAIDL::leAudioBroadcastConfig>
                       (fetch_offload_broadcast_audio_config());
  } else { //unicast
    lea_config->set<AudioConfigurationAIDL::leAudioConfig>
                       (fetch_offload_audio_config(profile, direction));
      //LOG(ERROR) << __func__
      //       << ": lea_config: " << lea_config->getTag();
  }
  //LOG(INFO) << __func__ << ": lea_config=" << toString(*lea_config);
  return true;
}

uint16_t btif_ahim_get_lea_active_profile(int profile) {
  return pclient_cbs[profile - 1]->get_profile_status_cb();
}

bool btif_ahim_setup_codec(uint8_t profile) {
  BTIF_TRACE_IMP("%s: setup", __func__);
  if (btif_ahim_is_aosp_aidl_hal_enabled()) {
    BTIF_TRACE_IMP("%s: AIDL, profile: %d", __func__, profile);
    if (profile == A2DP) {
      return bluetooth::audio::aidl::a2dp::setup_codec();
    } else if (profile == AUDIO_GROUP_MGR) {
      AudioConfigurationAIDL lea_tx_config;
      AudioConfigurationAIDL lea_rx_config;
      uint16_t profile_type = btif_ahim_get_lea_active_profile(profile);
      BTIF_TRACE_IMP("%s: AIDL, profile_type: %d", __func__, profile_type);
      if(profile_type == BAP || profile_type == GCP) {  // ToAIr only
#ifdef ADV_AUDIO_FEATURE
        CodecIndex codec_type = (CodecIndex) pclient_cbs[profile - 1]->get_codec_type(TX_ONLY_CONFIG);
        if (codec_type == CodecIndex::CODEC_INDEX_SOURCE_APTX_ADAPTIVE_R4) {
          if (!leAudio_get_selected_hal_codec_config(&lea_tx_config, profile,
                                                      TX_ONLY_CONFIG)) {
            LOG(ERROR) << __func__ << ": Failed to get CodecConfiguration";
            return false;
          }
          if(unicastSinkClientInterface)
            unicastSinkClientInterface->UpdateAudioConfigToHal(lea_tx_config);

          if (!leAudio_get_selected_hal_codec_config(&lea_rx_config, profile,
                                                      TX_RX_BOTH_CONFIG)) {
            LOG(ERROR) << __func__ << ": Failed to get CodecConfiguration";
            return false;
          }
          if(unicastSourceClientInterface)
            unicastSourceClientInterface->UpdateAudioConfigToHal(lea_rx_config);
        } else {
#endif
            if (!leAudio_get_selected_hal_codec_config(&lea_tx_config, profile,
                                                        TX_ONLY_CONFIG)) {
              LOG(ERROR) << __func__ << ": Failed to get CodecConfiguration";
              return false;
            }

            //LOG(ERROR) << __func__
            //     << ": audio_config_tag: " << lea_tx_config.getTag();
            // TODO to fill both session/single session configs based on profile
            if(unicastSinkClientInterface)
              unicastSinkClientInterface->UpdateAudioConfigToHal(lea_tx_config);
#ifdef ADV_AUDIO_FEATURE
        }
#endif
      } else if(profile_type == BAP_CALL ||
                profile_type == GCP_RX ||
                profile_type == WMCP_TX) { // Toair and FromAir
        if (!leAudio_get_selected_hal_codec_config(&lea_tx_config, profile,
                                                    TX_ONLY_CONFIG)) {
          LOG(ERROR) << __func__ << ": Failed to get CodecConfiguration";
          return false;
        }
        if(unicastSinkClientInterface)
          unicastSinkClientInterface->UpdateAudioConfigToHal(lea_tx_config);

        if (!leAudio_get_selected_hal_codec_config(&lea_rx_config, profile,
                                                    TX_RX_BOTH_CONFIG)) {
          LOG(ERROR) << __func__ << ": Failed to get CodecConfiguration";
          return false;
        }
        if(unicastSourceClientInterface)
          unicastSourceClientInterface->UpdateAudioConfigToHal(lea_rx_config);

      } else if(profile_type == WMCP) { // FromAir only
        if (!leAudio_get_selected_hal_codec_config(&lea_rx_config, profile,
                                                    RX_ONLY_CONFIG)) {
          LOG(ERROR) << __func__ << ": Failed to get CodecConfiguration";
          return false;
        }
        if(unicastSourceClientInterface)
          unicastSourceClientInterface->UpdateAudioConfigToHal(lea_rx_config);
      }
    } else if (profile == BROADCAST) {
        AudioConfigurationAIDL lea_tx_config;
        if (!leAudio_get_selected_hal_codec_config(&lea_tx_config, profile,
                                                    TX_ONLY_CONFIG)) {
          LOG(ERROR) << __func__ << ": Failed to get CodecConfiguration for Broadcast";
          return false;
        }
        if (broadcastSinkClientInterface)
          broadcastSinkClientInterface->UpdateAudioConfigToHal(lea_tx_config);
    }
  }else if (btif_ahim_is_qc_hal_enabled()) {
    BTIF_TRACE_IMP("%s: QC", __func__);
    return bluetooth::audio::a2dp::setup_codec(profile);
  }
  return true;
}

void btif_ahim_start_session(uint8_t profile) {
  BTIF_TRACE_IMP("%s: start_session for profile: %d", __func__, profile);
  if (btif_ahim_is_aosp_aidl_hal_enabled()) {
    BTIF_TRACE_IMP("%s: AIDL, cur_active_profile: %d",
                             __func__, cur_active_profile);
    if (profile == A2DP) {
      return bluetooth::audio::aidl::a2dp::start_session();
    } else if (profile == AUDIO_GROUP_MGR) {
      uint16_t profile_type =
               btif_ahim_get_lea_active_profile(profile);
      BTIF_TRACE_IMP("%s: AIDL, profile_type: %d", __func__, profile_type);
      if(profile_type == BAP || profile_type == GCP) {  // ToAIr only
#ifdef ADV_AUDIO_FEATURE
        CodecIndex codec_type = (CodecIndex) pclient_cbs[profile - 1]->get_codec_type(TX_ONLY_CONFIG);
        if (codec_type == CodecIndex::CODEC_INDEX_SOURCE_APTX_ADAPTIVE_R4) {
          if(unicastSinkClientInterface)
            unicastSinkClientInterface->StartSession();
          if(unicastSourceClientInterface)
            unicastSourceClientInterface->StartSession();
        } else {
#endif
        if(unicastSinkClientInterface)
          unicastSinkClientInterface->StartSession();
#ifdef ADV_AUDIO_FEATURE
        }
#endif
      } else if(profile_type == BAP_CALL ||
                profile_type == GCP_RX ||
                profile_type == WMCP_TX) { // Toair and FromAir
        if(unicastSinkClientInterface)
          unicastSinkClientInterface->StartSession();
        if(unicastSourceClientInterface)
          unicastSourceClientInterface->StartSession();
      } else if(profile_type == WMCP) { // FromAir only
        if(unicastSourceClientInterface)
          unicastSourceClientInterface->StartSession();
      }
    } else if (profile == BROADCAST) {
        if (broadcastSinkClientInterface)
          broadcastSinkClientInterface->StartSession();
    }
  } else if (btif_ahim_is_qc_hal_enabled()) {
    BTIF_TRACE_IMP("%s: QC Hal", __func__);
    bluetooth::audio::a2dp::start_session();
  }
}

void btif_ahim_end_session(uint8_t profile) {
  if (btif_ahim_is_aosp_aidl_hal_enabled()) {
    std::lock_guard<std::mutex>lock(session_mtx);
    BTIF_TRACE_IMP("%s: AIDL, profile: %d", __func__, profile);
    if (profile == A2DP) {
       return bluetooth::audio::aidl::a2dp::end_session();
    } else if (profile == AUDIO_GROUP_MGR) {
      uint16_t profile_type =
               btif_ahim_get_lea_active_profile(profile);
      BTIF_TRACE_IMP("%s: AIDL, profile_type: %d", __func__, profile_type);
      if(profile_type == BAP || profile_type == GCP) {  // ToAIr only
        if(unicastSinkClientInterface)
          unicastSinkClientInterface->StopSession();
      } else if(profile_type == BAP_CALL ||
                profile_type == GCP_RX ||
                profile_type == WMCP_TX) { // Toair and FromAir
        if(unicastSinkClientInterface)
          unicastSinkClientInterface->StopSession();
        if(unicastSourceClientInterface)
          unicastSourceClientInterface->StopSession();
      } else if(profile_type == WMCP) { // FromAir only
        if(unicastSourceClientInterface)
          unicastSourceClientInterface->StopSession();
      }
    } else if (profile == BROADCAST) {
        if (broadcastSinkClientInterface)
          broadcastSinkClientInterface->StopSession();
    }
  } else if (btif_ahim_is_qc_hal_enabled()) {
    BTIF_TRACE_IMP("%s: QC", __func__);
    bluetooth::audio::a2dp::end_session();
  }
}

tA2DP_CTRL_CMD btif_ahim_get_pending_command(uint8_t profile) {
  BTIF_TRACE_IMP("%s: AIDL, profile: %d", __func__, profile);
  if (btif_ahim_is_aosp_aidl_hal_enabled()) {
    if (profile == A2DP) {
       return bluetooth::audio::aidl::a2dp::GetPendingCmd();
    } else if (profile == AUDIO_GROUP_MGR) {
      uint16_t profile_type =
                 btif_ahim_get_lea_active_profile(profile);
      BTIF_TRACE_IMP("%s: AIDL, profile_type: %d", __func__, profile_type);
      if(profile_type == BAP || profile_type == GCP) {  // ToAIr only
        if(unicastSinkClientInterface)
          return unicastSinkClientInterface->GetPendingCmd();
      } else if(profile_type == BAP_CALL ||
                profile_type == GCP_RX ||
                profile_type == WMCP_TX) { // Toair and FromAir
        // TODO to return both or single one
        if(unicastSinkClientInterface) {
          tA2DP_CTRL_CMD cmd = unicastSinkClientInterface->GetPendingCmd();
          if(cmd != A2DP_CTRL_CMD_NONE) {
            return cmd;
          }
        }
        if(unicastSourceClientInterface) {
          tA2DP_CTRL_CMD cmd = unicastSourceClientInterface->GetPendingCmd();
          if(cmd != A2DP_CTRL_CMD_NONE) {
            return cmd;
          }
        }
        return A2DP_CTRL_CMD_NONE;
      } else if(profile_type == WMCP) { // FromAir only
        if(unicastSourceClientInterface)
          return unicastSourceClientInterface->GetPendingCmd();
      }
    } else if (profile == BROADCAST) {
        if (broadcastSinkClientInterface)
          return broadcastSinkClientInterface->GetPendingCmd();
    }
  } else if (btif_ahim_is_qc_hal_enabled()) {
    return bluetooth::audio::a2dp::get_pending_command();
  }
  return A2DP_CTRL_CMD_NONE;
}

tA2DP_CTRL_CMD btif_ahim_get_pending_command(uint8_t profile,
                                             uint8_t direction) {
  BTIF_TRACE_IMP("%s: AIDL, profile: %d, direction: %d",
                                     __func__, profile, direction);
  if (btif_ahim_is_aosp_aidl_hal_enabled()) {
    if (profile == A2DP) {
       return bluetooth::audio::aidl::a2dp::GetPendingCmd();
    } else if (profile == AUDIO_GROUP_MGR) {
      if(direction == TO_AIR) {
        if(unicastSinkClientInterface)
          return unicastSinkClientInterface->GetPendingCmd();
      } else {
        if(unicastSourceClientInterface)
          return unicastSourceClientInterface->GetPendingCmd();
      }
    } else if (profile == BROADCAST) {
        if (broadcastSinkClientInterface)
          return broadcastSinkClientInterface->GetPendingCmd();
    }
  } else if (btif_ahim_is_qc_hal_enabled()) {
    return bluetooth::audio::a2dp::get_pending_command();
  }
  return A2DP_CTRL_CMD_NONE;
}

uint16_t btif_ahim_get_sink_latency() {
  return bluetooth::audio::a2dp::get_sink_latency();
}

void btif_ahim_reset_pending_command(uint8_t profile) {
  BTIF_TRACE_IMP("%s: AIDL, profile: %d", __func__, profile);
  if (btif_ahim_is_aosp_aidl_hal_enabled()) {
    if (profile == A2DP) {
       return bluetooth::audio::aidl::a2dp::ResetPendingCmd();
    } else if (profile == AUDIO_GROUP_MGR) {
      uint16_t profile_type =
                     btif_ahim_get_lea_active_profile(profile);
      BTIF_TRACE_IMP("%s: AIDL, profile_type: %d", __func__, profile_type);
      if(profile_type == BAP || profile_type == GCP) {  // ToAIr only
        if(unicastSinkClientInterface)
          unicastSinkClientInterface->ResetPendingCmd();
      } else if(profile_type == BAP_CALL ||
                profile_type == GCP_RX ||
                profile_type == WMCP_TX) { // Toair and FromAir
        // TODO to return both or single one
        if(unicastSinkClientInterface)
          unicastSinkClientInterface->ResetPendingCmd();
        if(unicastSourceClientInterface)
          unicastSourceClientInterface->ResetPendingCmd();
      } else if(profile_type == WMCP) { // FromAir only
        if(unicastSourceClientInterface)
          unicastSourceClientInterface->ResetPendingCmd();
      }
    } else if (profile == BROADCAST) {
      if (broadcastSinkClientInterface)
        broadcastSinkClientInterface->ResetPendingCmd();
    }
  } else if (btif_ahim_is_qc_hal_enabled()) {
    if (cur_active_profile == profile) {
      bluetooth::audio::a2dp::reset_pending_command();
    } else {
      BTIF_TRACE_WARNING("%s, reset pending cmd ignored from #\
                          inactive profile", __func__);
    }
  }
}

void btif_ahim_reset_pending_command(uint8_t profile, uint8_t direction) {
  BTIF_TRACE_IMP("%s: AIDL, profile: %d, direction: %d",
                                       __func__, profile, direction);
  if (btif_ahim_is_aosp_aidl_hal_enabled()) {
    if (profile == A2DP) {
       return bluetooth::audio::aidl::a2dp::ResetPendingCmd();
    } else if (profile == AUDIO_GROUP_MGR) {
      if(direction == TO_AIR) {  // ToAIr only
        if(unicastSinkClientInterface)
          unicastSinkClientInterface->ResetPendingCmd();
      } else {
        if(unicastSourceClientInterface)
          unicastSourceClientInterface->ResetPendingCmd();
      }
    } else if (profile == BROADCAST) {
      if (broadcastSinkClientInterface)
        broadcastSinkClientInterface->ResetPendingCmd();
    }
  } else if (btif_ahim_is_qc_hal_enabled()) {
    if (cur_active_profile == profile) {
      bluetooth::audio::a2dp::reset_pending_command();
    } else {
      BTIF_TRACE_WARNING("%s, reset pending cmd ignored from #\
                          inactive profile", __func__);
    }
  }
}

void btif_ahim_update_pending_command(tA2DP_CTRL_CMD cmd, uint8_t profile) {
  BTIF_TRACE_IMP("%s: AIDL, profile: %d, cmd: %d",
                                       __func__, profile, cmd);
  if (btif_ahim_is_qc_hal_enabled()) {
    BTIF_TRACE_IMP("%s: QC, cur_active_profile: %d", __func__, cur_active_profile);
    if (cur_active_profile == profile) {
      bluetooth::audio::a2dp::update_pending_command(cmd);
    } else {
      BTIF_TRACE_WARNING("%s, update pending cmd ignored from "
                              "inactive profile, rsp failed ack", __func__);
      tA2DP_CTRL_ACK status = A2DP_CTRL_ACK_FAILURE;
      switch (cmd) {
        case A2DP_CTRL_CMD_START:
          btif_ahim_ack_stream_started(status, profile);
          break;
        case A2DP_CTRL_CMD_SUSPEND:
        case A2DP_CTRL_CMD_STOP:
          btif_ahim_ack_stream_suspended(status, profile);
          break;
        default:
          break;
      }
    }
  }
}

void btif_ahim_ack_stream_started(const tA2DP_CTRL_ACK& ack, uint8_t profile) {
  if (btif_ahim_is_aosp_aidl_hal_enabled()) {
    BTIF_TRACE_IMP("%s: AIDL, profile: %d, ack: %d", __func__, profile, ack);
    if (profile == A2DP) {
      return bluetooth::audio::aidl::a2dp::ack_stream_started(ack);
    } else if (profile == AUDIO_GROUP_MGR) {
      uint16_t profile_type =
               btif_ahim_get_lea_active_profile(profile);
      BTIF_TRACE_IMP("%s: AIDL, profile_type: %d", __func__, profile_type);

      if (ack == A2DP_CTRL_ACK_DISCONNECT_IN_PROGRESS ||
          ack == A2DP_CTRL_ACK_FAILURE) {
        if (profile_type == BAP || profile_type == GCP ||
            profile_type == BAP_CALL || profile_type == GCP_RX ||
            profile_type == WMCP_TX) {
          if(unicastSinkClientInterface)
            unicastSinkClientInterface->CancelStreamingRequest();
        }
        if (profile_type == BAP_CALL || profile_type == GCP_RX ||
            profile_type == WMCP || profile_type == WMCP_TX) {
          if(unicastSourceClientInterface)
            unicastSourceClientInterface->CancelStreamingRequest();
        }
        return;
      }

      if (ack != A2DP_CTRL_ACK_SUCCESS) {
        BTIF_TRACE_IMP("%s: Ack is not success yet, return", __func__);
        return;
      }

      if(profile_type == BAP || profile_type == GCP) {  // ToAIr only
        if(unicastSinkClientInterface)
          unicastSinkClientInterface->ConfirmStreamingRequest();
      } else if(profile_type == BAP_CALL ||
                profile_type == GCP_RX ||
                profile_type == WMCP_TX) { // Toair and FromAir
        if(unicastSinkClientInterface)
          unicastSinkClientInterface->ConfirmStreamingRequest();
        if(unicastSourceClientInterface)
          unicastSourceClientInterface->ConfirmStreamingRequest();
      } else if(profile_type == WMCP) { // FromAir only
        if(unicastSourceClientInterface)
          unicastSourceClientInterface->ConfirmStreamingRequest();
      }
    } else if (profile == BROADCAST) {
      if (ack == A2DP_CTRL_ACK_SUCCESS) {
        if (broadcastSinkClientInterface)
          broadcastSinkClientInterface->ConfirmStreamingRequest();
      } else if (ack == A2DP_CTRL_ACK_PENDING) {
        BTIF_TRACE_IMP("%s: Ignore Pending ACK", __func__);
        return;
      } else {
        if (broadcastSinkClientInterface)
          broadcastSinkClientInterface->CancelStreamingRequest();
      }
    }
  } else if (btif_ahim_is_qc_hal_enabled()) {
    BTIF_TRACE_IMP("%s: QC", __func__);
    if (cur_active_profile == profile) {
      bluetooth::audio::a2dp::ack_stream_started(ack);
    } else {
      BTIF_TRACE_WARNING("%s, ACK ignored from inactive profile", __func__);
    }
  }
}

void btif_ahim_ack_stream_suspended(const tA2DP_CTRL_ACK& ack, uint8_t profile) {
  btif_ahim_ack_stream_profile_suspended(ack, profile, 0);
}

void btif_ahim_ack_stream_direction_suspended(const tA2DP_CTRL_ACK& ack,
                                              uint8_t profile, uint8_t direction) {
  BTIF_TRACE_IMP("%s: profile: %d, ack: %d, direction: %d",
                                        __func__,  profile, ack, direction);
  if(direction == FROM_AIR) {
    // Fake WMCP for FROM_AIR
    btif_ahim_ack_stream_profile_suspended(ack, profile, WMCP);
  } else {
    // Fake BAP for TO_AIR
    btif_ahim_ack_stream_profile_suspended(ack, profile, BAP);
  }
}

void btif_ahim_ack_stream_profile_suspended(const tA2DP_CTRL_ACK& ack, uint8_t profile,
                                    uint16_t sub_profile) {
  BTIF_TRACE_IMP("%s: AIDL, profile: %d, ack: %d", __func__, profile, ack);
  if (btif_ahim_is_aosp_aidl_hal_enabled()) {
    BTIF_TRACE_IMP("%s: AIDL, sub_profile: %d", __func__, sub_profile);
    if (profile == A2DP) {
      return bluetooth::audio::aidl::a2dp::ack_stream_suspended(ack);
    } else if (profile == AUDIO_GROUP_MGR) {
     uint16_t profile_type;
      if(sub_profile) {
        profile_type = sub_profile;
      } else {
        profile_type = btif_ahim_get_lea_active_profile(profile);
      }

      BTIF_TRACE_IMP("%s: AIDL, profile_type: %d", __func__, profile_type);
      if(ack == A2DP_CTRL_ACK_STREAM_SUSPENDED) {
        if(profile_type == BAP || profile_type == GCP) {  // ToAIr only
          if(unicastSinkClientInterface)
            unicastSinkClientInterface->CancelSuspendRequestWithReconfig();
        } else if(profile_type == BAP_CALL ||
                  profile_type == GCP_RX ||
                  profile_type == WMCP_TX) { // Toair and FromAir
          if(unicastSinkClientInterface)
            unicastSinkClientInterface->CancelSuspendRequestWithReconfig();
          if(unicastSourceClientInterface)
            unicastSourceClientInterface->CancelSuspendRequestWithReconfig();
        } else if(profile_type == WMCP) { // FromAir only
          if(unicastSourceClientInterface)
            unicastSourceClientInterface->CancelSuspendRequestWithReconfig();
        }
        return;
      } else if (ack == A2DP_CTRL_ACK_DISCONNECT_IN_PROGRESS ||
                 ack == A2DP_CTRL_ACK_FAILURE) {
        if(profile_type == BAP || profile_type == GCP ||
           profile_type == BAP_CALL || profile_type == GCP_RX ||
           profile_type == WMCP_TX) {
          if(unicastSinkClientInterface)
            unicastSinkClientInterface->CancelSuspendRequest();
        }
        if (profile_type == BAP_CALL || profile_type == GCP_RX ||
            profile_type == WMCP || profile_type == WMCP_TX) {
          if(unicastSourceClientInterface)
            unicastSourceClientInterface->CancelSuspendRequest();
        }
        return;
      } else if (ack != A2DP_CTRL_ACK_SUCCESS) {
        BTIF_TRACE_IMP("%s: Ack is not success yet, return", __func__);
        return;
      }

      if (profile_type == BAP || profile_type == GCP) {  // ToAIr only
        if(unicastSinkClientInterface)
          unicastSinkClientInterface->ConfirmSuspendRequest();
      } else if(profile_type == BAP_CALL ||
                profile_type == GCP_RX ||
                profile_type == WMCP_TX) { // Toair and FromAir
        if(unicastSinkClientInterface)
          unicastSinkClientInterface->ConfirmSuspendRequest();
        if(unicastSourceClientInterface)
          unicastSourceClientInterface->ConfirmSuspendRequest();
      } else if(profile_type == WMCP) { // FromAir only
        if(unicastSourceClientInterface)
          unicastSourceClientInterface->ConfirmSuspendRequest();
      }
    } else if (profile == BROADCAST) {
      if (ack == A2DP_CTRL_ACK_STREAM_SUSPENDED) {
        if (broadcastSinkClientInterface)
          broadcastSinkClientInterface->CancelSuspendRequestWithReconfig();
      } else if (ack == A2DP_CTRL_ACK_SUCCESS) {
        if (broadcastSinkClientInterface)
          broadcastSinkClientInterface->ConfirmSuspendRequest();
      } else if (ack == A2DP_CTRL_ACK_PENDING) {
        BTIF_TRACE_IMP("%s: Ignore Pending ACK", __func__);
        return;
      } else {
        if (broadcastSinkClientInterface)
          broadcastSinkClientInterface->CancelSuspendRequest();
      }
    }
  } else if (btif_ahim_is_qc_hal_enabled()) {
    BTIF_TRACE_IMP("%s: QC", __func__);
    if (cur_active_profile == profile) {
      bluetooth::audio::a2dp::ack_stream_suspended(ack);
    } else {
      BTIF_TRACE_WARNING("%s, ACK ignored from inactive profile", __func__);
    }
  }
}

size_t btif_ahim_read(uint8_t* p_buf, uint32_t len) {
  if (btif_ahim_is_aosp_aidl_hal_enabled()) {
    return bluetooth::audio::aidl::a2dp::read(p_buf, len);
  } else if (btif_ahim_is_qc_hal_enabled()) {
    return bluetooth::audio::a2dp::read(p_buf, len);
  }
  return bluetooth::audio::aidl::a2dp::read(p_buf, len);
}

#ifdef ADV_AUDIO_FEATURE
void btif_ahim_update_audio_config() {
  AudioConfigurationAIDL lea_tx_config, lea_rx_config;
  uint16_t profile_type = btif_ahim_get_lea_active_profile(AUDIO_GROUP_MGR);
  BTIF_TRACE_IMP("%s: AIDL, profile_type: %d", __func__, profile_type);
  if(profile_type == BAP || profile_type == GCP) {  // ToAIr only
    CodecIndex codec_type = (CodecIndex) pclient_cbs[AUDIO_GROUP_MGR - 1]->get_codec_type(TX_ONLY_CONFIG);
    if (codec_type == CodecIndex::CODEC_INDEX_SOURCE_APTX_ADAPTIVE_R4) {
      if (!leAudio_get_selected_hal_codec_config(&lea_tx_config, AUDIO_GROUP_MGR,
                                                  TX_ONLY_CONFIG)) {
        LOG(ERROR) << __func__ << ": Failed to get CodecConfiguration";
        return;
      }
      if(unicastSinkClientInterface)
        unicastSinkClientInterface->UpdateAudioConfigToHal(lea_tx_config);

      if (!leAudio_get_selected_hal_codec_config(&lea_rx_config, AUDIO_GROUP_MGR,
                                                  TX_RX_BOTH_CONFIG)) {
        LOG(ERROR) << __func__ << ": Failed to get CodecConfiguration";
        return;
      }
      if(unicastSourceClientInterface)
        unicastSourceClientInterface->UpdateAudioConfigToHal(lea_rx_config);
    } else {
        if (!leAudio_get_selected_hal_codec_config(&lea_tx_config, AUDIO_GROUP_MGR,
                                                    TX_ONLY_CONFIG)) {
          LOG(ERROR) << __func__ << ": Failed to get CodecConfiguration";
          return;
        }

        if(unicastSinkClientInterface)
          unicastSinkClientInterface->UpdateAudioConfigToHal(lea_tx_config);
    }
  } else if(profile_type == BAP_CALL ||
            profile_type == GCP_RX ||
            profile_type == WMCP_TX) { // Toair and FromAir
    if (!leAudio_get_selected_hal_codec_config(&lea_tx_config, AUDIO_GROUP_MGR,
                                                TX_ONLY_CONFIG)) {
      LOG(ERROR) << __func__ << ": Failed to get CodecConfiguration";
      return;
    }
    if(unicastSinkClientInterface)
      unicastSinkClientInterface->UpdateAudioConfigToHal(lea_tx_config);

    if (!leAudio_get_selected_hal_codec_config(&lea_rx_config, AUDIO_GROUP_MGR,
                                                TX_RX_BOTH_CONFIG)) {
      LOG(ERROR) << __func__ << ": Failed to get CodecConfiguration";
      return;
    }
    if(unicastSourceClientInterface)
      unicastSourceClientInterface->UpdateAudioConfigToHal(lea_rx_config);

  } else if(profile_type == WMCP) { // FromAir only
    if (!leAudio_get_selected_hal_codec_config(&lea_rx_config, AUDIO_GROUP_MGR,
                                                RX_ONLY_CONFIG)) {
      LOG(ERROR) << __func__ << ": Failed to get CodecConfiguration";
      return;
    }
    if(unicastSourceClientInterface)
      unicastSourceClientInterface->UpdateAudioConfigToHal(lea_rx_config);
  }
}

uint16_t btif_ahim_get_remote_delay() {
if(unicastSinkClientInterface)
  return unicastSinkClientInterface->GetRemoteDelay();
else
  return 0xFFFF;
}
#endif

void btif_ahim_set_remote_delay(uint16_t delay_report, uint8_t profile) {
  BTIF_TRACE_IMP("%s: AIDL, profile: %d, delay_report: %d",
                                        __func__, profile, delay_report);
  if (btif_ahim_is_aosp_aidl_hal_enabled()) {
    if (profile == A2DP) {
      bluetooth::audio::aidl::a2dp::set_remote_delay(delay_report);
    } else if (profile == AUDIO_GROUP_MGR) {
      uint16_t profile_type =
                btif_ahim_get_lea_active_profile(profile);
      BTIF_TRACE_IMP("%s: AIDL, profile_type: %d", __func__, profile_type);
      if(profile_type == BAP || profile_type == GCP) {  // ToAIr only
        if(unicastSinkClientInterface)
          unicastSinkClientInterface->SetRemoteDelay(delay_report);
      } else if(profile_type == BAP_CALL ||
                profile_type == GCP_RX ||
                profile_type == WMCP_TX) { // Toair and FromAir
        if(unicastSinkClientInterface)
          unicastSinkClientInterface->SetRemoteDelay(delay_report);
        if(unicastSourceClientInterface)
          unicastSourceClientInterface->SetRemoteDelay(delay_report);
      } else if(profile_type == WMCP) { // FromAir only
        if(unicastSourceClientInterface)
          unicastSourceClientInterface->SetRemoteDelay(delay_report);
      }
    } else if (profile == BROADCAST) {
      if (broadcastSinkClientInterface) {
        broadcastSinkClientInterface->SetRemoteDelay(delay_report);
      } else {
        if (broadcastSinkClientInterface)
          broadcastSinkClientInterface->SetRemoteDelay(delay_report);
      }
    }
  } else if (btif_ahim_is_qc_hal_enabled()) {
    BTIF_TRACE_IMP("%s: QC", __func__);
    if (cur_active_profile == profile) {
      bluetooth::audio::a2dp::set_remote_delay(delay_report);
    } else {
      BTIF_TRACE_WARNING("%s, ACK ignored from inactive profile", __func__);
    }
  }
}

//there is no implementation for this function
//in hidl. so, ignoring it for aidl.
bool btif_ahim_is_streaming() {
  return bluetooth::audio::a2dp::is_streaming();
}

SessionType btif_ahim_get_session_type(uint8_t profile) {
  BTIF_TRACE_IMP("%s: AIDL, profile: %d", __func__, profile);
  if (btif_ahim_is_aosp_aidl_hal_enabled()) {
    if (profile == A2DP) {
      SessionTypeAIDL session_type =
         bluetooth::audio::aidl::a2dp::get_session_type();
      SessionType qc_hidl_session_type;
      switch (session_type) {
        case SessionTypeAIDL::A2DP_SOFTWARE_ENCODING_DATAPATH:
          qc_hidl_session_type = SessionType::A2DP_SOFTWARE_ENCODING_DATAPATH;
          break;
        case SessionTypeAIDL::A2DP_HARDWARE_OFFLOAD_ENCODING_DATAPATH:
          qc_hidl_session_type = SessionType::A2DP_HARDWARE_OFFLOAD_DATAPATH;
          break;
        default:
          qc_hidl_session_type = SessionType::A2DP_HARDWARE_OFFLOAD_DATAPATH;
          break;
      }
      return qc_hidl_session_type;
    } else if (profile == AUDIO_GROUP_MGR) {
      uint16_t profile_type =
              btif_ahim_get_lea_active_profile(profile);
     BTIF_TRACE_IMP("%s: AIDL, profile_type: %d", __func__, profile_type);
     if(profile_type == BAP || profile_type == GCP) {  // ToAIr only
       //unicastSinkClientInterface->ConfirmSuspendRequest();
     } else if(profile_type == BAP_CALL ||
               profile_type == GCP_RX ||
               profile_type == WMCP_TX) { // Toair and FromAir
       //unicastSinkClientInterface->ConfirmSuspendRequest();
       //unicastSourceClientInterface->ConfirmSuspendRequest();
      } else if(profile_type == WMCP) { // FromAir only
        //unicastSourceClientInterface->ConfirmSuspendRequest();
      }
      return SessionType::A2DP_HARDWARE_OFFLOAD_DATAPATH;
    } else if (profile == BROADCAST) {
      return SessionType::A2DP_HARDWARE_OFFLOAD_DATAPATH;
      /* TODO
      return SessionTypeAIDL::LE_AUDIO_BROADCAST_HARDWARE_OFFLOAD_ENCODING_DATAPATH;
  */
    }
  } else if (btif_ahim_is_qc_hal_enabled()) {
    return bluetooth::audio::a2dp::get_session_type();
  }
  return SessionType::A2DP_HARDWARE_OFFLOAD_DATAPATH;
}
#endif // #if AHIM_ENABLED
