/*
 * Copyright (c) 2010 Broadcom Corporation
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define __UNDEF_NO_VERSION__

#include <linux/kernel.h>
#include <linux/etherdevice.h>
#include <linux/string.h>
#include <linux/pci_ids.h>
#include <bcmdefs.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/sched.h>
#include <osl.h>
#define WLC_MAXBSSCFG		1	/* single BSS configs */

#include <wlc_cfg.h>
#include <net/mac80211.h>
#include <phy_version.h>
#include <bcmutils.h>
#include <pcicfg.h>
#include <wlioctl.h>
#include <wlc_key.h>
#include <sbhndpio.h>
#include <sbhnddma.h>
#include <wlc_channel.h>
#include <wlc_pub.h>
#include <wlc_scb.h>
#include <wl_dbg.h>
#include <wl_export.h>

#include <wl_mac80211.h>
#include <linux/firmware.h>
#include <wl_ucode.h>
#include <d11ucode_ext.h>


static void wl_timer(unsigned long data);
static void _wl_timer(wl_timer_t *t);


static int ieee_hw_init(struct ieee80211_hw *hw);
static int ieee_hw_rate_init(struct ieee80211_hw *hw);

static int wl_linux_watchdog(void *ctx);

/* Flags we support */
#define MAC_FILTERS (FIF_PROMISC_IN_BSS | \
	FIF_ALLMULTI | \
	FIF_FCSFAIL | \
	FIF_PLCPFAIL | \
	FIF_CONTROL | \
	FIF_OTHER_BSS | \
	FIF_BCN_PRBRESP_PROMISC)

static int wl_found;

#define WL_DEV_IF(dev)		((struct wl_if *)netdev_priv(dev))
#define	WL_INFO(dev)		((struct wl_info *)(WL_DEV_IF(dev)->wl))
static int wl_request_fw(struct wl_info *wl, struct pci_dev *pdev);
static void wl_release_fw(struct wl_info *wl);

/* local prototypes */
static int wl_start(struct sk_buff *skb, struct wl_info *wl);
static int wl_start_int(struct wl_info *wl, struct ieee80211_hw *hw,
			struct sk_buff *skb);
static void wl_dpc(unsigned long data);

MODULE_AUTHOR("Broadcom Corporation");
MODULE_DESCRIPTION("Broadcom 802.11n wireless LAN driver.");
MODULE_SUPPORTED_DEVICE("Broadcom 802.11n WLAN cards");
MODULE_LICENSE("Dual BSD/GPL");

/* recognized PCI IDs */
static struct pci_device_id wl_id_table[] = {
	{PCI_VENDOR_ID_BROADCOM, 0x4357, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0},	/* 43225 2G */
	{PCI_VENDOR_ID_BROADCOM, 0x4353, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0},	/* 43224 DUAL */
	{PCI_VENDOR_ID_BROADCOM, 0x4727, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0},	/* 4313 DUAL */
	{0}
};

MODULE_DEVICE_TABLE(pci, wl_id_table);
static void wl_remove(struct pci_dev *pdev);


#ifdef BCMDBG
static int msglevel = 0xdeadbeef;
module_param(msglevel, int, 0);
static int phymsglevel = 0xdeadbeef;
module_param(phymsglevel, int, 0);
#endif				/* BCMDBG */

#define HW_TO_WL(hw)	 (hw->priv)
#define WL_TO_HW(wl)	  (wl->pub->ieee_hw)
static int wl_ops_tx(struct ieee80211_hw *hw, struct sk_buff *skb);
static int wl_ops_start(struct ieee80211_hw *hw);
static void wl_ops_stop(struct ieee80211_hw *hw);
static int wl_ops_add_interface(struct ieee80211_hw *hw,
				struct ieee80211_vif *vif);
static void wl_ops_remove_interface(struct ieee80211_hw *hw,
				    struct ieee80211_vif *vif);
static int wl_ops_config(struct ieee80211_hw *hw, u32 changed);
static void wl_ops_bss_info_changed(struct ieee80211_hw *hw,
				    struct ieee80211_vif *vif,
				    struct ieee80211_bss_conf *info,
				    u32 changed);
static void wl_ops_configure_filter(struct ieee80211_hw *hw,
				    unsigned int changed_flags,
				    unsigned int *total_flags, u64 multicast);
static int wl_ops_set_tim(struct ieee80211_hw *hw, struct ieee80211_sta *sta,
			  bool set);
static void wl_ops_sw_scan_start(struct ieee80211_hw *hw);
static void wl_ops_sw_scan_complete(struct ieee80211_hw *hw);
static void wl_ops_set_tsf(struct ieee80211_hw *hw, u64 tsf);
static int wl_ops_get_stats(struct ieee80211_hw *hw,
			    struct ieee80211_low_level_stats *stats);
static int wl_ops_set_rts_threshold(struct ieee80211_hw *hw, u32 value);
static void wl_ops_sta_notify(struct ieee80211_hw *hw,
			      struct ieee80211_vif *vif,
			      enum sta_notify_cmd cmd,
			      struct ieee80211_sta *sta);
static int wl_ops_conf_tx(struct ieee80211_hw *hw, u16 queue,
			  const struct ieee80211_tx_queue_params *params);
static u64 wl_ops_get_tsf(struct ieee80211_hw *hw);
static int wl_ops_sta_add(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		      struct ieee80211_sta *sta);
static int wl_ops_sta_remove(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			 struct ieee80211_sta *sta);
static int wl_ops_ampdu_action(struct ieee80211_hw *hw,
			       struct ieee80211_vif *vif,
			       enum ieee80211_ampdu_mlme_action action,
			       struct ieee80211_sta *sta, u16 tid, u16 *ssn);
static void wl_ops_rfkill_poll(struct ieee80211_hw *hw);

static int wl_ops_tx(struct ieee80211_hw *hw, struct sk_buff *skb)
{
	int status;
	struct wl_info *wl = hw->priv;

	WL_LOCK(wl);
	if (!wl->pub->up) {
		WL_ERROR("ops->tx called while down\n");
		status = -ENETDOWN;
		goto done;
	}
	status = wl_start(skb, wl);
 done:
	WL_UNLOCK(wl);
	return status;
}

static int wl_ops_start(struct ieee80211_hw *hw)
{
	struct wl_info *wl = hw->priv;
	bool blocked;
	/*
	  struct ieee80211_channel *curchan = hw->conf.channel;
	  WL_NONE("%s : Initial channel: %d\n", __func__, curchan->hw_value);
	*/

	ieee80211_wake_queues(hw);
	WL_LOCK(wl);
	blocked = wl_rfkill_set_hw_state(wl);
	WL_UNLOCK(wl);
	if (!blocked)
		wiphy_rfkill_stop_polling(wl->pub->ieee_hw->wiphy);

	return 0;
}

static void wl_ops_stop(struct ieee80211_hw *hw)
{
	struct wl_info *wl = hw->priv;
	ASSERT(wl);
	ieee80211_stop_queues(hw);
	return;
}

static int
wl_ops_add_interface(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
	struct wl_info *wl;
	int err;

	/* Just STA for now */
	if (vif->type != NL80211_IFTYPE_AP &&
	    vif->type != NL80211_IFTYPE_MESH_POINT &&
	    vif->type != NL80211_IFTYPE_STATION &&
	    vif->type != NL80211_IFTYPE_WDS &&
	    vif->type != NL80211_IFTYPE_ADHOC) {
		WL_ERROR("%s: Attempt to add type %d, only STA for now\n",
			 __func__, vif->type);
		return -EOPNOTSUPP;
	}

	wl = HW_TO_WL(hw);
	WL_LOCK(wl);
	err = wl_up(wl);
	WL_UNLOCK(wl);

	if (err != 0) {
		WL_ERROR("%s: wl_up() returned %d\n", __func__, err);
	}
	return err;
}

static void
wl_ops_remove_interface(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
	struct wl_info *wl;

	wl = HW_TO_WL(hw);

	/* put driver in down state */
	WL_LOCK(wl);
	wl_down(wl);
	WL_UNLOCK(wl);
}

/*
 * precondition: perimeter lock has been acquired
 */
static int
ieee_set_channel(struct ieee80211_hw *hw, struct ieee80211_channel *chan,
		 enum nl80211_channel_type type)
{
	struct wl_info *wl = HW_TO_WL(hw);
	int err = 0;

	switch (type) {
	case NL80211_CHAN_HT20:
	case NL80211_CHAN_NO_HT:
		err = wlc_set(wl->wlc, WLC_SET_CHANNEL, chan->hw_value);
		break;
	case NL80211_CHAN_HT40MINUS:
	case NL80211_CHAN_HT40PLUS:
		WL_ERROR("%s: Need to implement 40 Mhz Channels!\n", __func__);
		err = 1;
		break;
	}

	if (err)
		return -EIO;
	return err;
}

static int wl_ops_config(struct ieee80211_hw *hw, u32 changed)
{
	struct ieee80211_conf *conf = &hw->conf;
	struct wl_info *wl = HW_TO_WL(hw);
	int err = 0;
	int new_int;

	WL_LOCK(wl);
	if (changed & IEEE80211_CONF_CHANGE_LISTEN_INTERVAL) {
		if (wlc_iovar_setint
		    (wl->wlc, "bcn_li_bcn", conf->listen_interval)) {
			WL_ERROR("%s: Error setting listen_interval\n",
				 __func__);
			err = -EIO;
			goto config_out;
		}
		wlc_iovar_getint(wl->wlc, "bcn_li_bcn", &new_int);
		ASSERT(new_int == conf->listen_interval);
	}
	if (changed & IEEE80211_CONF_CHANGE_MONITOR)
		WL_ERROR("%s: change monitor mode: %s (implement)\n", __func__,
			 conf->flags & IEEE80211_CONF_MONITOR ?
				"true" : "false");
	if (changed & IEEE80211_CONF_CHANGE_PS)
		WL_ERROR("%s: change power-save mode: %s (implement)\n",
			 __func__, conf->flags & IEEE80211_CONF_PS ?
				"true" : "false");

	if (changed & IEEE80211_CONF_CHANGE_POWER) {
		if (wlc_iovar_setint
		    (wl->wlc, "qtxpower", conf->power_level * 4)) {
			WL_ERROR("%s: Error setting power_level\n", __func__);
			err = -EIO;
			goto config_out;
		}
		wlc_iovar_getint(wl->wlc, "qtxpower", &new_int);
		if (new_int != (conf->power_level * 4))
			WL_ERROR("%s: Power level req != actual, %d %d\n",
				 __func__, conf->power_level * 4, new_int);
	}
	if (changed & IEEE80211_CONF_CHANGE_CHANNEL) {
		err = ieee_set_channel(hw, conf->channel, conf->channel_type);
	}
	if (changed & IEEE80211_CONF_CHANGE_RETRY_LIMITS) {
		if (wlc_set
		    (wl->wlc, WLC_SET_SRL,
		     conf->short_frame_max_tx_count) < 0) {
			WL_ERROR("%s: Error setting srl\n", __func__);
			err = -EIO;
			goto config_out;
		}
		if (wlc_set(wl->wlc, WLC_SET_LRL, conf->long_frame_max_tx_count)
		    < 0) {
			WL_ERROR("%s: Error setting lrl\n", __func__);
			err = -EIO;
			goto config_out;
		}
	}

 config_out:
	WL_UNLOCK(wl);
	return err;
}

static void
wl_ops_bss_info_changed(struct ieee80211_hw *hw,
			struct ieee80211_vif *vif,
			struct ieee80211_bss_conf *info, u32 changed)
{
	struct wl_info *wl = HW_TO_WL(hw);
	int val;

	if (changed & BSS_CHANGED_ASSOC) {
		/* association status changed (associated/disassociated)
		 * also implies a change in the AID.
		 */
		WL_ERROR("%s: %s: %sassociated\n", KBUILD_MODNAME, __func__,
			 info->assoc ? "" : "dis");
		wlc_associate_upd(wl->wlc, info->assoc);
	}
	if (changed & BSS_CHANGED_ERP_CTS_PROT) {
		/* CTS protection changed */
		WL_ERROR("%s: use_cts_prot: %s (implement)\n", __func__,
			info->use_cts_prot ? "true" : "false");
	}
	if (changed & BSS_CHANGED_ERP_PREAMBLE) {
		/* preamble changed */
		WL_ERROR("%s: short preamble: %s (implement)\n", __func__,
			info->use_short_preamble ? "true" : "false");
	}
	if (changed & BSS_CHANGED_ERP_SLOT) {
		/* slot timing changed */
		if (info->use_short_slot)
			val = 1;
		else
			val = 0;
		WL_LOCK(wl);
		wlc_set(wl->wlc, WLC_SET_SHORTSLOT_OVERRIDE, val);
		WL_UNLOCK(wl);
	}

	if (changed & BSS_CHANGED_HT) {
		/* 802.11n parameters changed */
		u16 mode = info->ht_operation_mode;
		WL_NONE("%s: HT mode: 0x%04X\n", __func__, mode);
		wlc_protection_upd(wl->wlc, WLC_PROT_N_CFG,
			mode & IEEE80211_HT_OP_MODE_PROTECTION);
		wlc_protection_upd(wl->wlc, WLC_PROT_N_NONGF,
			mode & IEEE80211_HT_OP_MODE_NON_GF_STA_PRSNT);
		wlc_protection_upd(wl->wlc, WLC_PROT_N_OBSS,
			mode & IEEE80211_HT_OP_MODE_NON_HT_STA_PRSNT);
	}
	if (changed & BSS_CHANGED_BASIC_RATES) {
		/* Basic rateset changed */
		WL_ERROR("%s: Need to change Basic Rates: 0x%x (implement)\n",
			 __func__, (u32) info->basic_rates);
	}
	if (changed & BSS_CHANGED_BEACON_INT) {
		/* Beacon interval changed */
		WL_NONE("%s: Beacon Interval: %d\n",
			__func__, info->beacon_int);
		wlc_set(wl->wlc, WLC_SET_BCNPRD, info->beacon_int);
	}
	if (changed & BSS_CHANGED_BSSID) {
		/* BSSID changed, for whatever reason (IBSS and managed mode) */
		WL_NONE("%s: new BSSID: aid %d  bss:%pM\n", __func__,
			info->aid, info->bssid);
		WL_LOCK(wl);
		wlc_set_addrmatch(wl->wlc, RCM_BSSID_OFFSET,
				  info->bssid);
		WL_UNLOCK(wl);
	}
	if (changed & BSS_CHANGED_BEACON) {
		/* Beacon data changed, retrieve new beacon (beaconing modes) */
		WL_ERROR("%s: beacon changed\n", __func__);
	}
	if (changed & BSS_CHANGED_BEACON_ENABLED) {
		/* Beaconing should be enabled/disabled (beaconing modes) */
		WL_ERROR("%s: Beacon enabled: %s\n", __func__,
			 info->enable_beacon ? "true" : "false");
	}
	if (changed & BSS_CHANGED_CQM) {
		/* Connection quality monitor config changed */
		WL_ERROR("%s: cqm change: threshold %d, hys %d (implement)\n",
			__func__, info->cqm_rssi_thold, info->cqm_rssi_hyst);
	}
	if (changed & BSS_CHANGED_IBSS) {
		/* IBSS join status changed */
		WL_ERROR("%s: IBSS joined: %s (implement)\n", __func__,
			info->ibss_joined ? "true" : "false");
	}
	if (changed & BSS_CHANGED_ARP_FILTER) {
		/* Hardware ARP filter address list or state changed */
		WL_ERROR("%s: arp filtering: enabled %s, count %d (implement)\n",
			__func__, info->arp_filter_enabled ? "true" : "false",
			info->arp_addr_cnt);
	}
	if (changed & BSS_CHANGED_QOS) {
		/*
		 * QoS for this association was enabled/disabled.
		 * Note that it is only ever disabled for station mode.
		 */
		WL_ERROR("%s: qos enabled: %s (implement)\n", __func__,
			info->qos ? "true" : "false");
	}
	if (changed & BSS_CHANGED_IDLE) {
		/* Idle changed for this BSS/interface */
		WL_ERROR("%s: BSS idle: %s (implement)\n", __func__,
			info->idle ? "true" : "false");
	}
	return;
}

static void
wl_ops_configure_filter(struct ieee80211_hw *hw,
			unsigned int changed_flags,
			unsigned int *total_flags, u64 multicast)
{
	struct wl_info *wl = hw->priv;

	changed_flags &= MAC_FILTERS;
	*total_flags &= MAC_FILTERS;
	if (changed_flags & FIF_PROMISC_IN_BSS)
		WL_ERROR("FIF_PROMISC_IN_BSS\n");
	if (changed_flags & FIF_ALLMULTI)
		WL_ERROR("FIF_ALLMULTI\n");
	if (changed_flags & FIF_FCSFAIL)
		WL_ERROR("FIF_FCSFAIL\n");
	if (changed_flags & FIF_PLCPFAIL)
		WL_ERROR("FIF_PLCPFAIL\n");
	if (changed_flags & FIF_CONTROL)
		WL_ERROR("FIF_CONTROL\n");
	if (changed_flags & FIF_OTHER_BSS)
		WL_ERROR("FIF_OTHER_BSS\n");
	if (changed_flags & FIF_BCN_PRBRESP_PROMISC) {
		WL_NONE("FIF_BCN_PRBRESP_PROMISC\n");
		WL_LOCK(wl);
		if (*total_flags & FIF_BCN_PRBRESP_PROMISC) {
			wl->pub->mac80211_state |= MAC80211_PROMISC_BCNS;
			wlc_mac_bcn_promisc_change(wl->wlc, 1);
		} else {
			wlc_mac_bcn_promisc_change(wl->wlc, 0);
			wl->pub->mac80211_state &= ~MAC80211_PROMISC_BCNS;
		}
		WL_UNLOCK(wl);
	}
	return;
}

static int
wl_ops_set_tim(struct ieee80211_hw *hw, struct ieee80211_sta *sta, bool set)
{
	WL_NONE("%s: Enter\n", __func__);
	return 0;
}

static void wl_ops_sw_scan_start(struct ieee80211_hw *hw)
{
	struct wl_info *wl = hw->priv;
	WL_NONE("Scan Start\n");
	WL_LOCK(wl);
	wlc_scan_start(wl->wlc);
	WL_UNLOCK(wl);
	return;
}

static void wl_ops_sw_scan_complete(struct ieee80211_hw *hw)
{
	struct wl_info *wl = hw->priv;
	WL_NONE("Scan Complete\n");
	WL_LOCK(wl);
	wlc_scan_stop(wl->wlc);
	WL_UNLOCK(wl);
	return;
}

static void wl_ops_set_tsf(struct ieee80211_hw *hw, u64 tsf)
{
	WL_ERROR("%s: Enter\n", __func__);
	return;
}

static int
wl_ops_get_stats(struct ieee80211_hw *hw,
		 struct ieee80211_low_level_stats *stats)
{
	struct wl_info *wl = hw->priv;
	struct wl_cnt *cnt;

	WL_LOCK(wl);
	cnt = wl->pub->_cnt;
	stats->dot11ACKFailureCount = cnt->txnoack;
	stats->dot11RTSFailureCount = cnt->txnocts;
	stats->dot11FCSErrorCount = cnt->rxcrc;
	stats->dot11RTSSuccessCount = cnt->txrts;
	WL_UNLOCK(wl);
	return 0;
}

static int wl_ops_set_rts_threshold(struct ieee80211_hw *hw, u32 value)
{
	struct wl_info *wl = hw->priv;

	WL_LOCK(wl);
	wlc_iovar_setint(wl->wlc, "rtsthresh", value & 0xFFFF);
	WL_UNLOCK(wl);
	return 0;
}

static void
wl_ops_sta_notify(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		  enum sta_notify_cmd cmd, struct ieee80211_sta *sta)
{
	WL_NONE("%s: Enter\n", __func__);
	switch (cmd) {
	default:
		WL_ERROR("%s: Unknown cmd = %d\n", __func__, cmd);
		break;
	}
	return;
}

static int
wl_ops_conf_tx(struct ieee80211_hw *hw, u16 queue,
	       const struct ieee80211_tx_queue_params *params)
{
	struct wl_info *wl = hw->priv;

	WL_NONE("%s: Enter (WME config)\n", __func__);
	WL_NONE("queue %d, txop %d, cwmin %d, cwmax %d, aifs %d\n", queue,
		 params->txop, params->cw_min, params->cw_max, params->aifs);

	WL_LOCK(wl);
	wlc_wme_setparams(wl->wlc, queue, (void *)params, true);
	WL_UNLOCK(wl);

	return 0;
}

static u64 wl_ops_get_tsf(struct ieee80211_hw *hw)
{
	WL_ERROR("%s: Enter\n", __func__);
	return 0;
}

static int
wl_ops_sta_add(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
	       struct ieee80211_sta *sta)
{
	struct scb *scb;

	int i;
	struct wl_info *wl = hw->priv;

	/* Init the scb */
	scb = (struct scb *)sta->drv_priv;
	memset(scb, 0, sizeof(struct scb));
	for (i = 0; i < NUMPRIO; i++)
		scb->seqctl[i] = 0xFFFF;
	scb->seqctl_nonqos = 0xFFFF;
	scb->magic = SCB_MAGIC;

	wl->pub->global_scb = scb;
	wl->pub->global_ampdu = &(scb->scb_ampdu);
	wl->pub->global_ampdu->scb = scb;
	wl->pub->global_ampdu->max_pdu = 16;
	pktq_init(&scb->scb_ampdu.txq, AMPDU_MAX_SCB_TID,
		  AMPDU_MAX_SCB_TID * PKTQ_LEN_DEFAULT);

	sta->ht_cap.ht_supported = true;
	sta->ht_cap.ampdu_factor = AMPDU_RX_FACTOR_64K;
	sta->ht_cap.ampdu_density = AMPDU_DEF_MPDU_DENSITY;
	sta->ht_cap.cap = IEEE80211_HT_CAP_GRN_FLD |
	    IEEE80211_HT_CAP_SGI_20 |
	    IEEE80211_HT_CAP_SGI_40 | IEEE80211_HT_CAP_40MHZ_INTOLERANT;

	/* minstrel_ht initiates addBA on our behalf by calling ieee80211_start_tx_ba_session() */
	return 0;
}

static int
wl_ops_sta_remove(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		  struct ieee80211_sta *sta)
{
	WL_NONE("%s: Enter\n", __func__);
	return 0;
}

static int
wl_ops_ampdu_action(struct ieee80211_hw *hw,
		    struct ieee80211_vif *vif,
		    enum ieee80211_ampdu_mlme_action action,
		    struct ieee80211_sta *sta, u16 tid, u16 *ssn)
{
#if defined(BCMDBG)
	struct scb *scb = (struct scb *)sta->drv_priv;
#endif
	struct wl_info *wl = hw->priv;
	int status;

	ASSERT(scb->magic == SCB_MAGIC);
	switch (action) {
	case IEEE80211_AMPDU_RX_START:
		WL_NONE("%s: action = IEEE80211_AMPDU_RX_START\n", __func__);
		break;
	case IEEE80211_AMPDU_RX_STOP:
		WL_NONE("%s: action = IEEE80211_AMPDU_RX_STOP\n", __func__);
		break;
	case IEEE80211_AMPDU_TX_START:
		WL_LOCK(wl);
		status = wlc_aggregatable(wl->wlc, tid);
		WL_UNLOCK(wl);
		if (!status) {
			/* WL_ERROR("START: tid %d is not agg' able, return FAILURE to stack\n", tid); */
			return -1;
		}
		/* XXX: Use the starting sequence number provided ... */
		*ssn = 0;
		ieee80211_start_tx_ba_cb_irqsafe(vif, sta->addr, tid);
		break;

	case IEEE80211_AMPDU_TX_STOP:
		ieee80211_stop_tx_ba_cb_irqsafe(vif, sta->addr, tid);
		break;
	case IEEE80211_AMPDU_TX_OPERATIONAL:
		/* Not sure what to do here */
		/* Power save wakeup */
		WL_NONE("%s: action = IEEE80211_AMPDU_TX_OPERATIONAL\n",
			__func__);
		break;
	default:
		WL_ERROR("%s: Invalid command, ignoring\n", __func__);
	}

	return 0;
}

static void wl_ops_rfkill_poll(struct ieee80211_hw *hw)
{
	struct wl_info *wl = HW_TO_WL(hw);
	bool blocked;

	WL_LOCK(wl);
	blocked = wlc_check_radio_disabled(wl->wlc);
	WL_UNLOCK(wl);

	WL_NONE("wl: rfkill_poll: %d\n", blocked);
	wiphy_rfkill_set_hw_state(wl->pub->ieee_hw->wiphy, blocked);
}

static const struct ieee80211_ops wl_ops = {
	.tx = wl_ops_tx,
	.start = wl_ops_start,
	.stop = wl_ops_stop,
	.add_interface = wl_ops_add_interface,
	.remove_interface = wl_ops_remove_interface,
	.config = wl_ops_config,
	.bss_info_changed = wl_ops_bss_info_changed,
	.configure_filter = wl_ops_configure_filter,
	.set_tim = wl_ops_set_tim,
	.sw_scan_start = wl_ops_sw_scan_start,
	.sw_scan_complete = wl_ops_sw_scan_complete,
	.set_tsf = wl_ops_set_tsf,
	.get_stats = wl_ops_get_stats,
	.set_rts_threshold = wl_ops_set_rts_threshold,
	.sta_notify = wl_ops_sta_notify,
	.conf_tx = wl_ops_conf_tx,
	.get_tsf = wl_ops_get_tsf,
	.sta_add = wl_ops_sta_add,
	.sta_remove = wl_ops_sta_remove,
	.ampdu_action = wl_ops_ampdu_action,
	.rfkill_poll = wl_ops_rfkill_poll,
};

/*
 * is called in wl_pci_probe() context, therefore no locking required.
 */
static int wl_set_hint(struct wl_info *wl, char *abbrev)
{
	WL_NONE("%s: Sending country code %c%c to MAC80211\n",
		 __func__, abbrev[0], abbrev[1]);
	return regulatory_hint(wl->pub->ieee_hw->wiphy, abbrev);
}

/**
 * attach to the WL device.
 *
 * Attach to the WL device identified by vendor and device parameters.
 * regs is a host accessible memory address pointing to WL device registers.
 *
 * wl_attach is not defined as static because in the case where no bus
 * is defined, wl_attach will never be called, and thus, gcc will issue
 * a warning that this function is defined but not used if we declare
 * it as static.
 *
 *
 * is called in wl_pci_probe() context, therefore no locking required.
 */
static struct wl_info *wl_attach(u16 vendor, u16 device, unsigned long regs,
			    uint bustype, void *btparam, uint irq)
{
	struct wl_info *wl;
	struct osl_info *osh;
	int unit, err;

	unsigned long base_addr;
	struct ieee80211_hw *hw;
	u8 perm[ETH_ALEN];

	unit = wl_found;
	err = 0;

	if (unit < 0) {
		WL_ERROR("wl%d: unit number overflow, exiting\n", unit);
		return NULL;
	}

	osh = osl_attach(btparam, bustype);
	ASSERT(osh);

	/* allocate private info */
	hw = pci_get_drvdata(btparam);	/* btparam == pdev */
	wl = hw->priv;
	ASSERT(wl);

	wl->osh = osh;
	atomic_set(&wl->callbacks, 0);

	/* setup the bottom half handler */
	tasklet_init(&wl->tasklet, wl_dpc, (unsigned long) wl);



	base_addr = regs;

	if (bustype == PCI_BUS) {
		wl->piomode = false;
	} else if (bustype == RPC_BUS) {
		/* Do nothing */
	} else {
		bustype = PCI_BUS;
		WL_TRACE("force to PCI\n");
	}
	wl->bcm_bustype = bustype;

	wl->regsva = ioremap_nocache(base_addr, PCI_BAR0_WINSZ);
	if (wl->regsva == NULL) {
		WL_ERROR("wl%d: ioremap() failed\n", unit);
		goto fail;
	}
	spin_lock_init(&wl->lock);
	spin_lock_init(&wl->isr_lock);

	/* prepare ucode */
	if (wl_request_fw(wl, (struct pci_dev *)btparam) < 0) {
		WL_ERROR("%s: Failed to find firmware usually in %s\n",
			 KBUILD_MODNAME, "/lib/firmware/brcm");
		wl_release_fw(wl);
		wl_remove((struct pci_dev *)btparam);
		goto fail1;
	}

	/* common load-time initialization */
	wl->wlc = wlc_attach((void *)wl, vendor, device, unit, wl->piomode, osh,
			     wl->regsva, wl->bcm_bustype, btparam, &err);
	wl_release_fw(wl);
	if (!wl->wlc) {
		WL_ERROR("%s: wlc_attach() failed with code %d\n",
			 KBUILD_MODNAME, err);
		goto fail;
	}
	wl->pub = wlc_pub(wl->wlc);

	wl->pub->ieee_hw = hw;
	ASSERT(wl->pub->ieee_hw);
	ASSERT(wl->pub->ieee_hw->priv == wl);


	if (wlc_iovar_setint(wl->wlc, "mpc", 0)) {
		WL_ERROR("wl%d: Error setting MPC variable to 0\n", unit);
	}

	/* register our interrupt handler */
	if (request_irq(irq, wl_isr, IRQF_SHARED, KBUILD_MODNAME, wl)) {
		WL_ERROR("wl%d: request_irq() failed\n", unit);
		goto fail;
	}
	wl->irq = irq;

	/* register module */
	wlc_module_register(wl->pub, NULL, "linux", wl, NULL, wl_linux_watchdog,
			    NULL);

	if (ieee_hw_init(hw)) {
		WL_ERROR("wl%d: %s: ieee_hw_init failed!\n", unit, __func__);
		goto fail;
	}

	memcpy(perm, &wl->pub->cur_etheraddr, ETH_ALEN);
	ASSERT(is_valid_ether_addr(perm));
	SET_IEEE80211_PERM_ADDR(hw, perm);

	err = ieee80211_register_hw(hw);
	if (err) {
		WL_ERROR("%s: ieee80211_register_hw failed, status %d\n",
			 __func__, err);
	}

	if (wl->pub->srom_ccode[0])
		err = wl_set_hint(wl, wl->pub->srom_ccode);
	else
		err = wl_set_hint(wl, "US");
	if (err) {
		WL_ERROR("%s: regulatory_hint failed, status %d\n",
			 __func__, err);
	}

	wl_found++;
	return wl;

fail:
	wl_free(wl);
fail1:
	return NULL;
}



#define CHAN2GHZ(channel, freqency, chflags)  { \
	.band = IEEE80211_BAND_2GHZ, \
	.center_freq = (freqency), \
	.hw_value = (channel), \
	.flags = chflags, \
	.max_antenna_gain = 0, \
	.max_power = 19, \
}

static struct ieee80211_channel wl_2ghz_chantable[] = {
	CHAN2GHZ(1, 2412, IEEE80211_CHAN_NO_HT40MINUS),
	CHAN2GHZ(2, 2417, IEEE80211_CHAN_NO_HT40MINUS),
	CHAN2GHZ(3, 2422, IEEE80211_CHAN_NO_HT40MINUS),
	CHAN2GHZ(4, 2427, IEEE80211_CHAN_NO_HT40MINUS),
	CHAN2GHZ(5, 2432, 0),
	CHAN2GHZ(6, 2437, 0),
	CHAN2GHZ(7, 2442, 0),
	CHAN2GHZ(8, 2447, IEEE80211_CHAN_NO_HT40PLUS),
	CHAN2GHZ(9, 2452, IEEE80211_CHAN_NO_HT40PLUS),
	CHAN2GHZ(10, 2457, IEEE80211_CHAN_NO_HT40PLUS),
	CHAN2GHZ(11, 2462, IEEE80211_CHAN_NO_HT40PLUS),
	CHAN2GHZ(12, 2467,
		 IEEE80211_CHAN_PASSIVE_SCAN | IEEE80211_CHAN_NO_IBSS |
		 IEEE80211_CHAN_NO_HT40PLUS),
	CHAN2GHZ(13, 2472,
		 IEEE80211_CHAN_PASSIVE_SCAN | IEEE80211_CHAN_NO_IBSS |
		 IEEE80211_CHAN_NO_HT40PLUS),
	CHAN2GHZ(14, 2484,
		 IEEE80211_CHAN_PASSIVE_SCAN | IEEE80211_CHAN_NO_IBSS |
		 IEEE80211_CHAN_NO_HT40PLUS | IEEE80211_CHAN_NO_HT40MINUS)
};

#define CHAN5GHZ(channel, chflags)  { \
	.band = IEEE80211_BAND_5GHZ, \
	.center_freq = 5000 + 5*(channel), \
	.hw_value = (channel), \
	.flags = chflags, \
	.max_antenna_gain = 0, \
	.max_power = 21, \
}

static struct ieee80211_channel wl_5ghz_nphy_chantable[] = {
	/* UNII-1 */
	CHAN5GHZ(36, IEEE80211_CHAN_NO_HT40MINUS),
	CHAN5GHZ(40, IEEE80211_CHAN_NO_HT40PLUS),
	CHAN5GHZ(44, IEEE80211_CHAN_NO_HT40MINUS),
	CHAN5GHZ(48, IEEE80211_CHAN_NO_HT40PLUS),
	/* UNII-2 */
	CHAN5GHZ(52,
		 IEEE80211_CHAN_RADAR | IEEE80211_CHAN_NO_IBSS |
		 IEEE80211_CHAN_PASSIVE_SCAN | IEEE80211_CHAN_NO_HT40MINUS),
	CHAN5GHZ(56,
		 IEEE80211_CHAN_RADAR | IEEE80211_CHAN_NO_IBSS |
		 IEEE80211_CHAN_PASSIVE_SCAN | IEEE80211_CHAN_NO_HT40PLUS),
	CHAN5GHZ(60,
		 IEEE80211_CHAN_RADAR | IEEE80211_CHAN_NO_IBSS |
		 IEEE80211_CHAN_PASSIVE_SCAN | IEEE80211_CHAN_NO_HT40MINUS),
	CHAN5GHZ(64,
		 IEEE80211_CHAN_RADAR | IEEE80211_CHAN_NO_IBSS |
		 IEEE80211_CHAN_PASSIVE_SCAN | IEEE80211_CHAN_NO_HT40PLUS),
	/* MID */
	CHAN5GHZ(100,
		 IEEE80211_CHAN_RADAR | IEEE80211_CHAN_NO_IBSS |
		 IEEE80211_CHAN_PASSIVE_SCAN | IEEE80211_CHAN_NO_HT40MINUS),
	CHAN5GHZ(104,
		 IEEE80211_CHAN_RADAR | IEEE80211_CHAN_NO_IBSS |
		 IEEE80211_CHAN_PASSIVE_SCAN | IEEE80211_CHAN_NO_HT40PLUS),
	CHAN5GHZ(108,
		 IEEE80211_CHAN_RADAR | IEEE80211_CHAN_NO_IBSS |
		 IEEE80211_CHAN_PASSIVE_SCAN | IEEE80211_CHAN_NO_HT40MINUS),
	CHAN5GHZ(112,
		 IEEE80211_CHAN_RADAR | IEEE80211_CHAN_NO_IBSS |
		 IEEE80211_CHAN_PASSIVE_SCAN | IEEE80211_CHAN_NO_HT40PLUS),
	CHAN5GHZ(116,
		 IEEE80211_CHAN_RADAR | IEEE80211_CHAN_NO_IBSS |
		 IEEE80211_CHAN_PASSIVE_SCAN | IEEE80211_CHAN_NO_HT40MINUS),
	CHAN5GHZ(120,
		 IEEE80211_CHAN_RADAR | IEEE80211_CHAN_NO_IBSS |
		 IEEE80211_CHAN_PASSIVE_SCAN | IEEE80211_CHAN_NO_HT40PLUS),
	CHAN5GHZ(124,
		 IEEE80211_CHAN_RADAR | IEEE80211_CHAN_NO_IBSS |
		 IEEE80211_CHAN_PASSIVE_SCAN | IEEE80211_CHAN_NO_HT40MINUS),
	CHAN5GHZ(128,
		 IEEE80211_CHAN_RADAR | IEEE80211_CHAN_NO_IBSS |
		 IEEE80211_CHAN_PASSIVE_SCAN | IEEE80211_CHAN_NO_HT40PLUS),
	CHAN5GHZ(132,
		 IEEE80211_CHAN_RADAR | IEEE80211_CHAN_NO_IBSS |
		 IEEE80211_CHAN_PASSIVE_SCAN | IEEE80211_CHAN_NO_HT40MINUS),
	CHAN5GHZ(136,
		 IEEE80211_CHAN_RADAR | IEEE80211_CHAN_NO_IBSS |
		 IEEE80211_CHAN_PASSIVE_SCAN | IEEE80211_CHAN_NO_HT40PLUS),
	CHAN5GHZ(140,
		 IEEE80211_CHAN_RADAR | IEEE80211_CHAN_NO_IBSS |
		 IEEE80211_CHAN_PASSIVE_SCAN | IEEE80211_CHAN_NO_HT40PLUS |
		 IEEE80211_CHAN_NO_HT40MINUS),
	/* UNII-3 */
	CHAN5GHZ(149, IEEE80211_CHAN_NO_HT40MINUS),
	CHAN5GHZ(153, IEEE80211_CHAN_NO_HT40PLUS),
	CHAN5GHZ(157, IEEE80211_CHAN_NO_HT40MINUS),
	CHAN5GHZ(161, IEEE80211_CHAN_NO_HT40PLUS),
	CHAN5GHZ(165, IEEE80211_CHAN_NO_HT40PLUS | IEEE80211_CHAN_NO_HT40MINUS)
};

#define RATE(rate100m, _flags) { \
	.bitrate = (rate100m), \
	.flags = (_flags), \
	.hw_value = (rate100m / 5), \
}

static struct ieee80211_rate wl_legacy_ratetable[] = {
	RATE(10, 0),
	RATE(20, IEEE80211_RATE_SHORT_PREAMBLE),
	RATE(55, IEEE80211_RATE_SHORT_PREAMBLE),
	RATE(110, IEEE80211_RATE_SHORT_PREAMBLE),
	RATE(60, 0),
	RATE(90, 0),
	RATE(120, 0),
	RATE(180, 0),
	RATE(240, 0),
	RATE(360, 0),
	RATE(480, 0),
	RATE(540, 0),
};

static struct ieee80211_supported_band wl_band_2GHz_nphy = {
	.band = IEEE80211_BAND_2GHZ,
	.channels = wl_2ghz_chantable,
	.n_channels = ARRAY_SIZE(wl_2ghz_chantable),
	.bitrates = wl_legacy_ratetable,
	.n_bitrates = ARRAY_SIZE(wl_legacy_ratetable),
	.ht_cap = {
		   /* from include/linux/ieee80211.h */
		   .cap = IEEE80211_HT_CAP_GRN_FLD |
		   IEEE80211_HT_CAP_SGI_20 |
		   IEEE80211_HT_CAP_SGI_40 | IEEE80211_HT_CAP_40MHZ_INTOLERANT,
		   .ht_supported = true,
		   .ampdu_factor = AMPDU_RX_FACTOR_64K,
		   .ampdu_density = AMPDU_DEF_MPDU_DENSITY,
		   .mcs = {
			   /* placeholders for now */
			   .rx_mask = {0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 0},
			   .rx_highest = 500,
			   .tx_params = IEEE80211_HT_MCS_TX_DEFINED}
		   }
};

static struct ieee80211_supported_band wl_band_5GHz_nphy = {
	.band = IEEE80211_BAND_5GHZ,
	.channels = wl_5ghz_nphy_chantable,
	.n_channels = ARRAY_SIZE(wl_5ghz_nphy_chantable),
	.bitrates = wl_legacy_ratetable + 4,
	.n_bitrates = ARRAY_SIZE(wl_legacy_ratetable) - 4,
	.ht_cap = {
		   /* use IEEE80211_HT_CAP_* from include/linux/ieee80211.h */
		   .cap = IEEE80211_HT_CAP_GRN_FLD | IEEE80211_HT_CAP_SGI_20 | IEEE80211_HT_CAP_SGI_40 | IEEE80211_HT_CAP_40MHZ_INTOLERANT,	/* No 40 mhz yet */
		   .ht_supported = true,
		   .ampdu_factor = AMPDU_RX_FACTOR_64K,
		   .ampdu_density = AMPDU_DEF_MPDU_DENSITY,
		   .mcs = {
			   /* placeholders for now */
			   .rx_mask = {0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 0},
			   .rx_highest = 500,
			   .tx_params = IEEE80211_HT_MCS_TX_DEFINED}
		   }
};

/*
 * is called in wl_pci_probe() context, therefore no locking required.
 */
static int ieee_hw_rate_init(struct ieee80211_hw *hw)
{
	struct wl_info *wl = HW_TO_WL(hw);
	int has_5g;
	char phy_list[4];

	has_5g = 0;

	hw->wiphy->bands[IEEE80211_BAND_2GHZ] = NULL;
	hw->wiphy->bands[IEEE80211_BAND_5GHZ] = NULL;

	if (wlc_get(wl->wlc, WLC_GET_PHYLIST, (int *)&phy_list) < 0) {
		WL_ERROR("Phy list failed\n");
	}
	WL_NONE("%s: phylist = %c\n", __func__, phy_list[0]);

	if (phy_list[0] == 'n' || phy_list[0] == 'c') {
		if (phy_list[0] == 'c') {
			/* Single stream */
			wl_band_2GHz_nphy.ht_cap.mcs.rx_mask[1] = 0;
			wl_band_2GHz_nphy.ht_cap.mcs.rx_highest = 72;
		}
		hw->wiphy->bands[IEEE80211_BAND_2GHZ] = &wl_band_2GHz_nphy;
	} else {
		BUG();
		return -1;
	}

	/* Assume all bands use the same phy.  True for 11n devices. */
	if (NBANDS_PUB(wl->pub) > 1) {
		has_5g++;
		if (phy_list[0] == 'n' || phy_list[0] == 'c') {
			hw->wiphy->bands[IEEE80211_BAND_5GHZ] =
			    &wl_band_5GHz_nphy;
		} else {
			return -1;
		}
	}

	WL_NONE("%s: 2ghz = %d, 5ghz = %d\n", __func__, 1, has_5g);

	return 0;
}

/*
 * is called in wl_pci_probe() context, therefore no locking required.
 */
static int ieee_hw_init(struct ieee80211_hw *hw)
{
	hw->flags = IEEE80211_HW_SIGNAL_DBM
	    /* | IEEE80211_HW_CONNECTION_MONITOR  What is this? */
	    | IEEE80211_HW_REPORTS_TX_ACK_STATUS
	    | IEEE80211_HW_AMPDU_AGGREGATION;

	hw->extra_tx_headroom = wlc_get_header_len();
	/* FIXME: should get this from wlc->machwcap */
	hw->queues = 4;
	/* FIXME: this doesn't seem to be used properly in minstrel_ht.
	 * mac80211/status.c:ieee80211_tx_status() checks this value,
	 * but mac80211/rc80211_minstrel_ht.c:minstrel_ht_get_rate()
	 * appears to always set 3 rates
	 */
	hw->max_rates = 2;	/* Primary rate and 1 fallback rate */

	hw->channel_change_time = 7 * 1000;	/* channel change time is dependant on chip and band  */
	hw->wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);

	hw->rate_control_algorithm = "minstrel_ht";

	hw->sta_data_size = sizeof(struct scb);
	return ieee_hw_rate_init(hw);
}

/**
 * determines if a device is a WL device, and if so, attaches it.
 *
 * This function determines if a device pointed to by pdev is a WL device,
 * and if so, performs a wl_attach() on it.
 *
 * Perimeter lock is initialized in the course of this function.
 */
int __devinit
wl_pci_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	int rc;
	struct wl_info *wl;
	struct ieee80211_hw *hw;
	u32 val;

	ASSERT(pdev);

	WL_TRACE("%s: bus %d slot %d func %d irq %d\n",
		 __func__, pdev->bus->number, PCI_SLOT(pdev->devfn),
		 PCI_FUNC(pdev->devfn), pdev->irq);

	if ((pdev->vendor != PCI_VENDOR_ID_BROADCOM) ||
	    (((pdev->device & 0xff00) != 0x4300) &&
	     ((pdev->device & 0xff00) != 0x4700) &&
	     ((pdev->device < 43000) || (pdev->device > 43999))))
		return -ENODEV;

	rc = pci_enable_device(pdev);
	if (rc) {
		WL_ERROR("%s: Cannot enable device %d-%d_%d\n",
			 __func__, pdev->bus->number, PCI_SLOT(pdev->devfn),
			 PCI_FUNC(pdev->devfn));
		return -ENODEV;
	}
	pci_set_master(pdev);

	pci_read_config_dword(pdev, 0x40, &val);
	if ((val & 0x0000ff00) != 0)
		pci_write_config_dword(pdev, 0x40, val & 0xffff00ff);

	hw = ieee80211_alloc_hw(sizeof(struct wl_info), &wl_ops);
	if (!hw) {
		WL_ERROR("%s: ieee80211_alloc_hw failed\n", __func__);
		rc = -ENOMEM;
		goto err_1;
	}

	SET_IEEE80211_DEV(hw, &pdev->dev);

	pci_set_drvdata(pdev, hw);

	memset(hw->priv, 0, sizeof(*wl));

	wl = wl_attach(pdev->vendor, pdev->device, pci_resource_start(pdev, 0),
		       PCI_BUS, pdev, pdev->irq);

	if (!wl) {
		WL_ERROR("%s: %s: wl_attach failed!\n",
			 KBUILD_MODNAME, __func__);
		return -ENODEV;
	}
	return 0;
 err_1:
	WL_ERROR("%s: err_1: Major hoarkage\n", __func__);
	return 0;
}

static int wl_suspend(struct pci_dev *pdev, pm_message_t state)
{
	struct wl_info *wl;
	struct ieee80211_hw *hw;

	WL_TRACE("wl: wl_suspend\n");

	hw = pci_get_drvdata(pdev);
	wl = HW_TO_WL(hw);
	if (!wl) {
		WL_ERROR("wl: wl_suspend: pci_get_drvdata failed\n");
		return -ENODEV;
	}

	/* only need to flag hw is down for proper resume */
	WL_LOCK(wl);
	wl->pub->hw_up = false;
	WL_UNLOCK(wl);

	pci_save_state(pdev);
	pci_disable_device(pdev);
	return pci_set_power_state(pdev, PCI_D3hot);
}

static int wl_resume(struct pci_dev *pdev)
{
	struct wl_info *wl;
	struct ieee80211_hw *hw;
	int err = 0;
	u32 val;

	WL_TRACE("wl: wl_resume\n");
	hw = pci_get_drvdata(pdev);
	wl = HW_TO_WL(hw);
	if (!wl) {
		WL_ERROR("wl: wl_resume: pci_get_drvdata failed\n");
		return -ENODEV;
	}

	err = pci_set_power_state(pdev, PCI_D0);
	if (err)
		return err;

	pci_restore_state(pdev);

	err = pci_enable_device(pdev);
	if (err)
		return err;

	pci_set_master(pdev);

	pci_read_config_dword(pdev, 0x40, &val);
	if ((val & 0x0000ff00) != 0)
		pci_write_config_dword(pdev, 0x40, val & 0xffff00ff);

	/*
	*  done. driver will be put in up state
	*  in wl_ops_add_interface() call.
	*/
	return err;
}

/*
* called from both kernel as from wl_*()
* precondition: perimeter lock is not acquired.
*/
static void wl_remove(struct pci_dev *pdev)
{
	struct wl_info *wl;
	struct ieee80211_hw *hw;
	int status;

	hw = pci_get_drvdata(pdev);
	wl = HW_TO_WL(hw);
	if (!wl) {
		WL_ERROR("wl: wl_remove: pci_get_drvdata failed\n");
		return;
	}

	WL_LOCK(wl);
	status = wlc_chipmatch(pdev->vendor, pdev->device);
	WL_UNLOCK(wl);
	if (!status) {
		WL_ERROR("wl: wl_remove: wlc_chipmatch failed\n");
		return;
	}
	if (wl->wlc) {
		wiphy_rfkill_set_hw_state(wl->pub->ieee_hw->wiphy, false);
		wiphy_rfkill_stop_polling(wl->pub->ieee_hw->wiphy);
		ieee80211_unregister_hw(hw);
		WL_LOCK(wl);
		wl_down(wl);
		WL_UNLOCK(wl);
		WL_NONE("%s: Down\n", __func__);
	}
	pci_disable_device(pdev);

	wl_free(wl);

	pci_set_drvdata(pdev, NULL);
	ieee80211_free_hw(hw);
}

static struct pci_driver wl_pci_driver = {
	.name     = KBUILD_MODNAME,
	.probe    = wl_pci_probe,
	.suspend  = wl_suspend,
	.resume   = wl_resume,
	.remove   = __devexit_p(wl_remove),
	.id_table = wl_id_table,
};

/**
 * This is the main entry point for the WL driver.
 *
 * This function determines if a device pointed to by pdev is a WL device,
 * and if so, performs a wl_attach() on it.
 *
 */
static int __init wl_module_init(void)
{
	int error = -ENODEV;

#ifdef BCMDBG
	if (msglevel != 0xdeadbeef)
		wl_msg_level = msglevel;
	else {
		char *var = getvar(NULL, "wl_msglevel");
		if (var)
			wl_msg_level = simple_strtoul(var, NULL, 0);
	}
	{
		extern u32 phyhal_msg_level;

		if (phymsglevel != 0xdeadbeef)
			phyhal_msg_level = phymsglevel;
		else {
			char *var = getvar(NULL, "phy_msglevel");
			if (var)
				phyhal_msg_level = simple_strtoul(var, NULL, 0);
		}
	}
#endif				/* BCMDBG */

	error = pci_register_driver(&wl_pci_driver);
	if (!error)
		return 0;



	return error;
}

/**
 * This function unloads the WL driver from the system.
 *
 * This function unconditionally unloads the WL driver module from the
 * system.
 *
 */
static void __exit wl_module_exit(void)
{
	pci_unregister_driver(&wl_pci_driver);

}

module_init(wl_module_init);
module_exit(wl_module_exit);

/**
 * This function frees the WL per-device resources.
 *
 * This function frees resources owned by the WL device pointed to
 * by the wl parameter.
 *
 * precondition: can both be called locked and unlocked
 *
 */
void wl_free(struct wl_info *wl)
{
	wl_timer_t *t, *next;
	struct osl_info *osh;

	ASSERT(wl);
	/* free ucode data */
	if (wl->fw.fw_cnt)
		wl_ucode_data_free();
	if (wl->irq)
		free_irq(wl->irq, wl);

	/* kill dpc */
	tasklet_kill(&wl->tasklet);

	if (wl->pub) {
		wlc_module_unregister(wl->pub, "linux", wl);
	}

	/* free common resources */
	if (wl->wlc) {
		wlc_detach(wl->wlc);
		wl->wlc = NULL;
		wl->pub = NULL;
	}

	/* virtual interface deletion is deferred so we cannot spinwait */

	/* wait for all pending callbacks to complete */
	while (atomic_read(&wl->callbacks) > 0)
		schedule();

	/* free timers */
	for (t = wl->timers; t; t = next) {
		next = t->next;
#ifdef BCMDBG
		if (t->name)
			kfree(t->name);
#endif
		kfree(t);
	}

	osh = wl->osh;

	/*
	 * unregister_netdev() calls get_stats() which may read chip registers
	 * so we cannot unmap the chip registers until after calling unregister_netdev() .
	 */
	if (wl->regsva && wl->bcm_bustype != SDIO_BUS &&
	    wl->bcm_bustype != JTAG_BUS) {
		iounmap((void *)wl->regsva);
	}
	wl->regsva = NULL;


	osl_detach(osh);
}

/*
 * transmit a packet
 * precondition: perimeter lock has been acquired
 */
static int BCMFASTPATH wl_start(struct sk_buff *skb, struct wl_info *wl)
{
	if (!wl)
		return -ENETDOWN;

	return wl_start_int(wl, WL_TO_HW(wl), skb);
}

static int BCMFASTPATH
wl_start_int(struct wl_info *wl, struct ieee80211_hw *hw, struct sk_buff *skb)
{
	wlc_sendpkt_mac80211(wl->wlc, skb, hw);
	return NETDEV_TX_OK;
}

/*
 * precondition: perimeter lock has been acquired
 */
void wl_txflowcontrol(struct wl_info *wl, struct wl_if *wlif, bool state,
		      int prio)
{
	WL_ERROR("Shouldn't be here %s\n", __func__);
}

/*
 * precondition: perimeter lock has been acquired
 */
void wl_init(struct wl_info *wl)
{
	WL_TRACE("wl%d: wl_init\n", wl->pub->unit);

	wl_reset(wl);

	wlc_init(wl->wlc);
}

/*
 * precondition: perimeter lock has been acquired
 */
uint wl_reset(struct wl_info *wl)
{
	WL_TRACE("wl%d: wl_reset\n", wl->pub->unit);

	wlc_reset(wl->wlc);

	/* dpc will not be rescheduled */
	wl->resched = 0;

	return 0;
}

/*
 * These are interrupt on/off entry points. Disable interrupts
 * during interrupt state transition.
 */
void BCMFASTPATH wl_intrson(struct wl_info *wl)
{
	unsigned long flags;

	INT_LOCK(wl, flags);
	wlc_intrson(wl->wlc);
	INT_UNLOCK(wl, flags);
}

/*
 * precondition: perimeter lock has been acquired
 */
bool wl_alloc_dma_resources(struct wl_info *wl, uint addrwidth)
{
	return true;
}

u32 BCMFASTPATH wl_intrsoff(struct wl_info *wl)
{
	unsigned long flags;
	u32 status;

	INT_LOCK(wl, flags);
	status = wlc_intrsoff(wl->wlc);
	INT_UNLOCK(wl, flags);
	return status;
}

void wl_intrsrestore(struct wl_info *wl, u32 macintmask)
{
	unsigned long flags;

	INT_LOCK(wl, flags);
	wlc_intrsrestore(wl->wlc, macintmask);
	INT_UNLOCK(wl, flags);
}

/*
 * precondition: perimeter lock has been acquired
 */
int wl_up(struct wl_info *wl)
{
	int error = 0;

	if (wl->pub->up)
		return 0;

	error = wlc_up(wl->wlc);

	return error;
}

/*
 * precondition: perimeter lock has been acquired
 */
void wl_down(struct wl_info *wl)
{
	uint callbacks, ret_val = 0;

	/* call common down function */
	ret_val = wlc_down(wl->wlc);
	callbacks = atomic_read(&wl->callbacks) - ret_val;

	/* wait for down callbacks to complete */
	WL_UNLOCK(wl);

	/* For HIGH_only driver, it's important to actually schedule other work,
	 * not just spin wait since everything runs at schedule level
	 */
	SPINWAIT((atomic_read(&wl->callbacks) > callbacks), 100 * 1000);

	WL_LOCK(wl);
}

irqreturn_t BCMFASTPATH wl_isr(int irq, void *dev_id)
{
	struct wl_info *wl;
	bool ours, wantdpc;
	unsigned long flags;

	wl = (struct wl_info *) dev_id;

	WL_ISRLOCK(wl, flags);

	/* call common first level interrupt handler */
	ours = wlc_isr(wl->wlc, &wantdpc);
	if (ours) {
		/* if more to do... */
		if (wantdpc) {

			/* ...and call the second level interrupt handler */
			/* schedule dpc */
			ASSERT(wl->resched == false);
			tasklet_schedule(&wl->tasklet);
		}
	}

	WL_ISRUNLOCK(wl, flags);

	return IRQ_RETVAL(ours);
}

static void BCMFASTPATH wl_dpc(unsigned long data)
{
	struct wl_info *wl;

	wl = (struct wl_info *) data;

	WL_LOCK(wl);

	/* call the common second level interrupt handler */
	if (wl->pub->up) {
		if (wl->resched) {
			unsigned long flags;

			INT_LOCK(wl, flags);
			wlc_intrsupd(wl->wlc);
			INT_UNLOCK(wl, flags);
		}

		wl->resched = wlc_dpc(wl->wlc, true);
	}

	/* wlc_dpc() may bring the driver down */
	if (!wl->pub->up)
		goto done;

	/* re-schedule dpc */
	if (wl->resched)
		tasklet_schedule(&wl->tasklet);
	else {
		/* re-enable interrupts */
		wl_intrson(wl);
	}

 done:
	WL_UNLOCK(wl);
}

static void wl_link_up(struct wl_info *wl, char *ifname)
{
	WL_NONE("wl%d: link up (%s)\n", wl->pub->unit, ifname);
}

static void wl_link_down(struct wl_info *wl, char *ifname)
{
	WL_NONE("wl%d: link down (%s)\n", wl->pub->unit, ifname);
}

/*
 * precondition: perimeter lock has been acquired
 */
void wl_event(struct wl_info *wl, char *ifname, wlc_event_t *e)
{

	switch (e->event.event_type) {
	case WLC_E_LINK:
	case WLC_E_NDIS_LINK:
		if (e->event.flags & WLC_EVENT_MSG_LINK)
			wl_link_up(wl, ifname);
		else
			wl_link_down(wl, ifname);
		break;
	case WLC_E_RADIO:
		break;
	}
}

/*
 * is called by the kernel from software irq context
 */
static void wl_timer(unsigned long data)
{
	_wl_timer((wl_timer_t *) data);
}

/*
* precondition: perimeter lock is not acquired
 */
static void _wl_timer(wl_timer_t *t)
{
	WL_LOCK(t->wl);

	if (t->set) {
		if (t->periodic) {
			t->timer.expires = jiffies + t->ms * HZ / 1000;
			atomic_inc(&t->wl->callbacks);
			add_timer(&t->timer);
			t->set = true;
		} else
			t->set = false;

		t->fn(t->arg);
	}

	atomic_dec(&t->wl->callbacks);

	WL_UNLOCK(t->wl);
}

/*
 * Adds a timer to the list. Caller supplies a timer function.
 * Is called from wlc.
 *
 * precondition: perimeter lock has been acquired
 */
wl_timer_t *wl_init_timer(struct wl_info *wl, void (*fn) (void *arg), void *arg,
			  const char *name)
{
	wl_timer_t *t;

	t = kmalloc(sizeof(wl_timer_t), GFP_ATOMIC);
	if (!t) {
		WL_ERROR("wl%d: wl_init_timer: out of memory\n", wl->pub->unit);
		return 0;
	}

	memset(t, 0, sizeof(wl_timer_t));

	init_timer(&t->timer);
	t->timer.data = (unsigned long) t;
	t->timer.function = wl_timer;
	t->wl = wl;
	t->fn = fn;
	t->arg = arg;
	t->next = wl->timers;
	wl->timers = t;

#ifdef BCMDBG
	t->name = kmalloc(strlen(name) + 1, GFP_ATOMIC);
	if (t->name)
		strcpy(t->name, name);
#endif

	return t;
}

/* BMAC_NOTE: Add timer adds only the kernel timer since it's going to be more accurate
 * as well as it's easier to make it periodic
 *
 * precondition: perimeter lock has been acquired
 */
void wl_add_timer(struct wl_info *wl, wl_timer_t *t, uint ms, int periodic)
{
#ifdef BCMDBG
	if (t->set) {
		WL_ERROR("%s: Already set. Name: %s, per %d\n",
			 __func__, t->name, periodic);
	}
#endif
	ASSERT(!t->set);

	t->ms = ms;
	t->periodic = (bool) periodic;
	t->set = true;
	t->timer.expires = jiffies + ms * HZ / 1000;

	atomic_inc(&wl->callbacks);
	add_timer(&t->timer);
}

/*
 * return true if timer successfully deleted, false if still pending
 *
 * precondition: perimeter lock has been acquired
 */
bool wl_del_timer(struct wl_info *wl, wl_timer_t *t)
{
	if (t->set) {
		t->set = false;
		if (!del_timer(&t->timer)) {
			return false;
		}
		atomic_dec(&wl->callbacks);
	}

	return true;
}

/*
 * precondition: perimeter lock has been acquired
 */
void wl_free_timer(struct wl_info *wl, wl_timer_t *t)
{
	wl_timer_t *tmp;

	/* delete the timer in case it is active */
	wl_del_timer(wl, t);

	if (wl->timers == t) {
		wl->timers = wl->timers->next;
#ifdef BCMDBG
		if (t->name)
			kfree(t->name);
#endif
		kfree(t);
		return;

	}

	tmp = wl->timers;
	while (tmp) {
		if (tmp->next == t) {
			tmp->next = t->next;
#ifdef BCMDBG
			if (t->name)
				kfree(t->name);
#endif
			kfree(t);
			return;
		}
		tmp = tmp->next;
	}

}

/*
 * runs in software irq context
 *
 * precondition: perimeter lock is not acquired
 */
static int wl_linux_watchdog(void *ctx)
{
	struct wl_info *wl = (struct wl_info *) ctx;
	struct wl_cnt *cnt;
	struct net_device_stats *stats = NULL;
	uint id;
	/* refresh stats */
	if (wl->pub->up) {
		ASSERT(wl->stats_id < 2);

		cnt = wl->pub->_cnt;
		id = 1 - wl->stats_id;
		stats = &wl->stats_watchdog[id];
		stats->rx_packets = cnt->rxframe;
		stats->tx_packets = cnt->txframe;
		stats->rx_bytes = cnt->rxbyte;
		stats->tx_bytes = cnt->txbyte;
		stats->rx_errors = cnt->rxerror;
		stats->tx_errors = cnt->txerror;
		stats->collisions = 0;

		stats->rx_length_errors = 0;
		stats->rx_over_errors = cnt->rxoflo;
		stats->rx_crc_errors = cnt->rxcrc;
		stats->rx_frame_errors = 0;
		stats->rx_fifo_errors = cnt->rxoflo;
		stats->rx_missed_errors = 0;

		stats->tx_fifo_errors = cnt->txuflo;

		wl->stats_id = id;
	}

	return 0;
}

struct wl_fw_hdr {
	u32 offset;
	u32 len;
	u32 idx;
};

char *wl_firmwares[WL_MAX_FW] = {
	"brcm/bcm43xx",
	NULL
};

/*
 * precondition: perimeter lock has been acquired
 */
int wl_ucode_init_buf(struct wl_info *wl, void **pbuf, u32 idx)
{
	int i, entry;
	const u8 *pdata;
	struct wl_fw_hdr *hdr;
	for (i = 0; i < wl->fw.fw_cnt; i++) {
		hdr = (struct wl_fw_hdr *)wl->fw.fw_hdr[i]->data;
		for (entry = 0; entry < wl->fw.hdr_num_entries[i];
		     entry++, hdr++) {
			if (hdr->idx == idx) {
				pdata = wl->fw.fw_bin[i]->data + hdr->offset;
				*pbuf = kmalloc(hdr->len, GFP_ATOMIC);
				if (*pbuf == NULL) {
					WL_ERROR("fail to alloc %d bytes\n",
						 hdr->len);
					goto fail;
				}
				memcpy(*pbuf, pdata, hdr->len);
				return 0;
			}
		}
	}
	WL_ERROR("ERROR: ucode buf tag:%d can not be found!\n", idx);
	*pbuf = NULL;
fail:
	return BCME_NOTFOUND;
}

/*
 * Precondition: Since this function is called in wl_pci_probe() context,
 * no locking is required.
 */
int wl_ucode_init_uint(struct wl_info *wl, u32 *data, u32 idx)
{
	int i, entry;
	const u8 *pdata;
	struct wl_fw_hdr *hdr;
	for (i = 0; i < wl->fw.fw_cnt; i++) {
		hdr = (struct wl_fw_hdr *)wl->fw.fw_hdr[i]->data;
		for (entry = 0; entry < wl->fw.hdr_num_entries[i];
		     entry++, hdr++) {
			if (hdr->idx == idx) {
				pdata = wl->fw.fw_bin[i]->data + hdr->offset;
				ASSERT(hdr->len == 4);
				*data = *((u32 *) pdata);
				return 0;
			}
		}
	}
	WL_ERROR("ERROR: ucode tag:%d can not be found!\n", idx);
	return -1;
}

/*
 * Precondition: Since this function is called in wl_pci_probe() context,
 * no locking is required.
 */
static int wl_request_fw(struct wl_info *wl, struct pci_dev *pdev)
{
	int status;
	struct device *device = &pdev->dev;
	char fw_name[100];
	int i;

	memset((void *)&wl->fw, 0, sizeof(struct wl_firmware));
	for (i = 0; i < WL_MAX_FW; i++) {
		if (wl_firmwares[i] == NULL)
			break;
		sprintf(fw_name, "%s-%d.fw", wl_firmwares[i],
			UCODE_LOADER_API_VER);
		WL_NONE("request fw %s\n", fw_name);
		status = request_firmware(&wl->fw.fw_bin[i], fw_name, device);
		if (status) {
			WL_ERROR("%s: fail to load firmware %s\n",
				 KBUILD_MODNAME, fw_name);
			return status;
		}
		WL_NONE("request fw %s\n", fw_name);
		sprintf(fw_name, "%s_hdr-%d.fw", wl_firmwares[i],
			UCODE_LOADER_API_VER);
		status = request_firmware(&wl->fw.fw_hdr[i], fw_name, device);
		if (status) {
			WL_ERROR("%s: fail to load firmware %s\n",
				 KBUILD_MODNAME, fw_name);
			return status;
		}
		wl->fw.hdr_num_entries[i] =
		    wl->fw.fw_hdr[i]->size / (sizeof(struct wl_fw_hdr));
		WL_NONE("request fw %s find: %d entries\n",
			fw_name, wl->fw.hdr_num_entries[i]);
	}
	wl->fw.fw_cnt = i;
	return wl_ucode_data_init(wl);
}

/*
 * precondition: can both be called locked and unlocked
 */
void wl_ucode_free_buf(void *p)
{
	kfree(p);
}

/*
 * Precondition: Since this function is called in wl_pci_probe() context,
 * no locking is required.
 */
static void wl_release_fw(struct wl_info *wl)
{
	int i;
	for (i = 0; i < WL_MAX_FW; i++) {
		release_firmware(wl->fw.fw_bin[i]);
		release_firmware(wl->fw.fw_hdr[i]);
	}
}


/*
 * checks validity of all firmware images loaded from user space
 *
 * Precondition: Since this function is called in wl_pci_probe() context,
 * no locking is required.
 */
int wl_check_firmwares(struct wl_info *wl)
{
	int i;
	int entry;
	int rc = 0;
	const struct firmware *fw;
	const struct firmware *fw_hdr;
	struct wl_fw_hdr *ucode_hdr;
	for (i = 0; i < WL_MAX_FW && rc == 0; i++) {
		fw =  wl->fw.fw_bin[i];
		fw_hdr = wl->fw.fw_hdr[i];
		if (fw == NULL && fw_hdr == NULL) {
			break;
		} else if (fw == NULL || fw_hdr == NULL) {
			WL_ERROR("%s: invalid bin/hdr fw\n", __func__);
			rc = -EBADF;
		} else if (fw_hdr->size % sizeof(struct wl_fw_hdr)) {
			WL_ERROR("%s: non integral fw hdr file size %zu/%zu\n",
				 __func__, fw_hdr->size,
				 sizeof(struct wl_fw_hdr));
			rc = -EBADF;
		} else if (fw->size < MIN_FW_SIZE || fw->size > MAX_FW_SIZE) {
			WL_ERROR("%s: out of bounds fw file size %zu\n",
				 __func__, fw->size);
			rc = -EBADF;
		} else {
			/* check if ucode section overruns firmware image */
			ucode_hdr = (struct wl_fw_hdr *)fw_hdr->data;
			for (entry = 0; entry < wl->fw.hdr_num_entries[i] &&
			     !rc; entry++, ucode_hdr++) {
				if (ucode_hdr->offset + ucode_hdr->len >
				    fw->size) {
					WL_ERROR("%s: conflicting bin/hdr\n",
						 __func__);
					rc = -EBADF;
				}
			}
		}
	}
	if (rc == 0 && wl->fw.fw_cnt != i) {
		WL_ERROR("%s: invalid fw_cnt=%d\n", __func__, wl->fw.fw_cnt);
		rc = -EBADF;
	}
	return rc;
}

/*
 * precondition: perimeter lock has been acquired
 */
bool wl_rfkill_set_hw_state(struct wl_info *wl)
{
	bool blocked = wlc_check_radio_disabled(wl->wlc);

	WL_NONE("%s: update hw state: blocked=%s\n", __func__,
		blocked ? "true" : "false");
	WL_UNLOCK(wl);
	wiphy_rfkill_set_hw_state(wl->pub->ieee_hw->wiphy, blocked);
	if (blocked)
		wiphy_rfkill_start_polling(wl->pub->ieee_hw->wiphy);
	WL_LOCK(wl);
	return blocked;
}
