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
#endif
