/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/* WMI wire definitions from Linux ath11k/wmi.h.
 * Copyright (c) 2018-2019 The Linux Foundation. All rights reserved.
 * Copyright (c) 2021-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#ifndef Q8B_WCN_WMI_PROTO_H
#define Q8B_WCN_WMI_PROTO_H
#include <stdint.h>
struct wmi_resource_config {
	uint32_t tlv_header;
	uint32_t num_vdevs;
	uint32_t num_peers;
	uint32_t num_offload_peers;
	uint32_t num_offload_reorder_buffs;
	uint32_t num_peer_keys;
	uint32_t num_tids;
	uint32_t ast_skid_limit;
	uint32_t tx_chain_mask;
	uint32_t rx_chain_mask;
	uint32_t rx_timeout_pri[4];
	uint32_t rx_decap_mode;
	uint32_t scan_max_pending_req;
	uint32_t bmiss_offload_max_vdev;
	uint32_t roam_offload_max_vdev;
	uint32_t roam_offload_max_ap_profiles;
	uint32_t num_mcast_groups;
	uint32_t num_mcast_table_elems;
	uint32_t mcast2ucast_mode;
	uint32_t tx_dbg_log_size;
	uint32_t num_wds_entries;
	uint32_t dma_burst_size;
	uint32_t mac_aggr_delim;
	uint32_t rx_skip_defrag_timeout_dup_detection_check;
	uint32_t vow_config;
	uint32_t gtk_offload_max_vdev;
	uint32_t num_msdu_desc;
	uint32_t max_frag_entries;
	uint32_t num_tdls_vdevs;
	uint32_t num_tdls_conn_table_entries;
	uint32_t beacon_tx_offload_max_vdev;
	uint32_t num_multicast_filter_entries;
	uint32_t num_wow_filters;
	uint32_t num_keep_alive_pattern;
	uint32_t keep_alive_pattern_size;
	uint32_t max_tdls_concurrent_sleep_sta;
	uint32_t max_tdls_concurrent_buffer_sta;
	uint32_t wmi_send_separate;
	uint32_t num_ocb_vdevs;
	uint32_t num_ocb_channels;
	uint32_t num_ocb_schedules;
	uint32_t flag1;
	uint32_t smart_ant_cap;
	uint32_t bk_minfree;
	uint32_t be_minfree;
	uint32_t vi_minfree;
	uint32_t vo_minfree;
	uint32_t alloc_frag_desc_for_data_pkt;
	uint32_t num_ns_ext_tuples_cfg;
	uint32_t bpf_instruction_size;
	uint32_t max_bssid_rx_filters;
	uint32_t use_pdev_id;
	uint32_t max_num_dbs_scan_duty_cycle;
	uint32_t max_num_group_keys;
	uint32_t peer_map_unmap_v2_support;
	uint32_t sched_params;
	uint32_t twt_ap_pdev_count;
	uint32_t twt_ap_sta_count;
	uint32_t max_nlo_ssids;
	uint32_t num_pkt_filters;
	uint32_t num_max_sta_vdevs;
	uint32_t max_bssid_indicator;
	uint32_t ul_resp_config;
	uint32_t msdu_flow_override_config0;
	uint32_t msdu_flow_override_config1;
	uint32_t flags2;
	uint32_t host_service_flags;
	uint32_t max_rnr_neighbours;
	uint32_t ema_max_vap_cnt;
	uint32_t ema_max_profile_period;
};

struct wmi_mac_addr { uint8_t addr[8]; };

struct  wmi_start_scan_cmd {
	uint32_t tlv_header;
	uint32_t scan_id;
	uint32_t scan_req_id;
	uint32_t vdev_id;
	uint32_t scan_priority;
	uint32_t notify_scan_events;
	uint32_t dwell_time_active;
	uint32_t dwell_time_passive;
	uint32_t min_rest_time;
	uint32_t max_rest_time;
	uint32_t repeat_probe_time;
	uint32_t probe_spacing_time;
	uint32_t idle_time;
	uint32_t max_scan_time;
	uint32_t probe_delay;
	uint32_t scan_ctrl_flags;
	uint32_t burst_duration;
	uint32_t num_chan;
	uint32_t num_bssid;
	uint32_t num_ssids;
	uint32_t ie_len;
	uint32_t n_probes;
	struct wmi_mac_addr mac_addr;
	struct wmi_mac_addr mac_mask;
	uint32_t ie_bitmap[8];
	uint32_t num_vendor_oui;
	uint32_t scan_ctrl_flags_ext;
	uint32_t dwell_time_active_2g;
	uint32_t dwell_time_active_6g;
	uint32_t dwell_time_passive_6g;
	uint32_t scan_start_offset;
};

struct wmi_vdev_create_cmd {
	uint32_t tlv_header;
	uint32_t vdev_id;
	uint32_t vdev_type;
	uint32_t vdev_subtype;
	struct wmi_mac_addr vdev_macaddr;
	uint32_t num_cfg_txrx_streams;
	uint32_t pdev_id;
	uint32_t mbssid_flags;
	uint32_t mbssid_tx_vdev_id;
};

struct wmi_vdev_txrx_streams {
	uint32_t tlv_header;
	uint32_t band;
	uint32_t supported_tx_streams;
	uint32_t supported_rx_streams;
};

struct wmi_scan_chan_list_cmd {
	uint32_t tlv_header;
	uint32_t num_scan_chans;
	uint32_t flags;
	uint32_t pdev_id;
};

struct wmi_channel {
	uint32_t tlv_header;
	uint32_t mhz;
	uint32_t band_center_freq1;
	uint32_t band_center_freq2;
	uint32_t info;
	uint32_t reg_info_1;
	uint32_t reg_info_2;
};

struct wmi_ssid {
	uint32_t ssid_len;
	uint32_t ssid[8];
};

struct wmi_vdev_start_request_cmd {
	uint32_t tlv_header;
	uint32_t vdev_id;
	uint32_t requestor_id;
	uint32_t beacon_interval;
	uint32_t dtim_period;
	uint32_t flags;
	struct wmi_ssid ssid;
	uint32_t bcn_tx_rate;
	uint32_t bcn_txpower;
	uint32_t num_noa_descriptors;
	uint32_t disable_hw_ack;
	uint32_t preferred_tx_streams;
	uint32_t preferred_rx_streams;
	uint32_t he_ops;
	uint32_t cac_duration_ms;
	uint32_t regdomain;
	uint32_t min_data_rate;
	uint32_t mbssid_flags;
	uint32_t mbssid_tx_vdev_id;
};

struct wmi_peer_create_cmd {
	uint32_t tlv_header;
	uint32_t vdev_id;
	struct wmi_mac_addr peer_macaddr;
	uint32_t peer_type;
};

struct wmi_mgmt_send_cmd {
	uint32_t tlv_header;
	uint32_t vdev_id;
	uint32_t desc_id;
	uint32_t chanfreq;
	uint32_t paddr_lo;
	uint32_t paddr_hi;
	uint32_t frame_len;
	uint32_t buf_len;
	uint32_t tx_params_valid;

	/* This TLV is followed by struct wmi_mgmt_frame */

	/* Followed by struct wmi_mgmt_send_params */
};

struct wmi_vdev_up_cmd {
	uint32_t tlv_header;
	uint32_t vdev_id;
	uint32_t vdev_assoc_id;
	struct wmi_mac_addr vdev_bssid;
	struct wmi_mac_addr tx_vdev_bssid;
	uint32_t nontx_profile_idx;
	uint32_t nontx_profile_cnt;
};

struct wmi_peer_reorder_queue_setup_cmd {
	uint32_t tlv_header;
	uint32_t vdev_id;
	struct wmi_mac_addr peer_macaddr;
	uint32_t tid;
	uint32_t queue_ptr_lo;
	uint32_t queue_ptr_hi;
	uint32_t queue_no;
	uint32_t ba_window_size_valid;
	uint32_t ba_window_size;
};

struct wmi_peer_set_param_cmd {
	uint32_t tlv_header;
	uint32_t vdev_id;
	struct wmi_mac_addr peer_macaddr;
	uint32_t param_id;
	uint32_t param_value;
};

struct wmi_ppe_threshold { uint32_t numss_m1, ru_mask, ppet16_ppet8_ru3_ru0[8]; };

struct  wmi_peer_assoc_complete_cmd {
	uint32_t tlv_header;
	struct wmi_mac_addr peer_macaddr;
	uint32_t vdev_id;
	uint32_t peer_new_assoc;
	uint32_t peer_associd;
	uint32_t peer_flags;
	uint32_t peer_caps;
	uint32_t peer_listen_intval;
	uint32_t peer_ht_caps;
	uint32_t peer_max_mpdu;
	uint32_t peer_mpdu_density;
	uint32_t peer_rate_caps;
	uint32_t peer_nss;
	uint32_t peer_vht_caps;
	uint32_t peer_phymode;
	uint32_t peer_ht_info[2];
	uint32_t num_peer_legacy_rates;
	uint32_t num_peer_ht_rates;
	uint32_t peer_bw_rxnss_override;
	struct  wmi_ppe_threshold peer_ppet;
	uint32_t peer_he_cap_info;
	uint32_t peer_he_ops;
	uint32_t peer_he_cap_phy[3];
	uint32_t peer_he_mcs;
	uint32_t peer_he_cap_info_ext;
	uint32_t peer_he_cap_info_internal;
	uint32_t min_data_rate;
	uint32_t peer_he_caps_6ghz;
};

struct wmi_vht_rate_set {
	uint32_t tlv_header;
	uint32_t rx_max_rate;
	/* MCS at which the peer can transmit */
	uint32_t rx_mcs_set;
	uint32_t tx_max_rate;
	/* MCS at which the peer can receive */
	uint32_t tx_mcs_set;
	uint32_t tx_max_mcs_nss;
};

struct wmi_key_seq_counter {
	uint32_t key_seq_counter_l;
	uint32_t key_seq_counter_h;
};

struct wmi_vdev_install_key_cmd {
	uint32_t tlv_header;
	uint32_t vdev_id;
	struct wmi_mac_addr peer_macaddr;
	uint32_t key_idx;
	uint32_t key_flags;
	uint32_t key_cipher;
	struct wmi_key_seq_counter key_rsc_counter;
	struct wmi_key_seq_counter key_global_rsc_counter;
	struct wmi_key_seq_counter key_tsc_counter;
	uint8_t wpi_key_rsc_counter[16];
	uint8_t wpi_key_tsc_counter[16];
	uint32_t key_len;
	uint32_t key_txmic_len;
	uint32_t key_rxmic_len;
	uint32_t is_group_key_id_valid;
	uint32_t group_key_id;

	/* Followed by key_data containing key followed by
	 * tx mic and then rx mic
	 */
};
#endif
