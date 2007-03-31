/*
 * Copyright 2007 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

/*
 * Copyright (c) 2004, 2005
 *      Damien Bergamini <damien.bergamini@free.fr>. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

#include <sys/types.h>
#include <sys/byteorder.h>
#include <sys/conf.h>
#include <sys/cmn_err.h>
#include <sys/stat.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/strsubr.h>
#include <sys/ethernet.h>
#include <inet/common.h>
#include <inet/nd.h>
#include <inet/mi.h>
#include <sys/note.h>
#include <sys/stream.h>
#include <sys/strsun.h>
#include <sys/modctl.h>
#include <sys/devops.h>
#include <sys/dlpi.h>
#include <sys/mac.h>
#include <sys/mac_wifi.h>
#include <sys/varargs.h>
#include <sys/pci.h>
#include <sys/policy.h>
#include <sys/random.h>

#include "ipw2200.h"
#include "ipw2200_impl.h"
#include <inet/wifi_ioctl.h>

/*
 * minimal size reserved in tx-ring
 */
#define	IPW2200_TX_RING_MIN	(8)
#define	IPW2200_TXBUF_SIZE	(IEEE80211_MAX_LEN)
#define	IPW2200_RXBUF_SIZE	(4096)

static void  *ipw2200_ssp = NULL;
static char ipw2200_ident[] = IPW2200_DRV_DESC " " IPW2200_DRV_REV;

/*
 * PIO access attributor for registers
 */
static ddi_device_acc_attr_t ipw2200_csr_accattr = {
	DDI_DEVICE_ATTR_V0,
	DDI_STRUCTURE_LE_ACC,
	DDI_STRICTORDER_ACC
};

/*
 * DMA access attributor for descriptors
 */
static ddi_device_acc_attr_t ipw2200_dma_accattr = {
	DDI_DEVICE_ATTR_V0,
	DDI_NEVERSWAP_ACC,
	DDI_STRICTORDER_ACC
};

/*
 * Describes the chip's DMA engine
 */
static ddi_dma_attr_t ipw2200_dma_attr = {
	DMA_ATTR_V0,		/* version */
	0x0000000000000000ULL,  /* addr_lo */
	0x00000000ffffffffULL,  /* addr_hi */
	0x00000000ffffffffULL,  /* counter */
	0x0000000000000004ULL,  /* alignment */
	0xfff,			/* burst */
	1,			/* min xfer */
	0x00000000ffffffffULL,  /* max xfer */
	0x00000000ffffffffULL,  /* seg boud */
	1,			/* s/g list */
	1,			/* granularity */
	0			/* flags */
};

static uint8_t ipw2200_broadcast_addr[] = {
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};
static const struct ieee80211_rateset ipw2200_rateset_11a = { 8,
	{12, 18, 24, 36, 48, 72, 96, 108}
};
static const struct ieee80211_rateset ipw2200_rateset_11b = { 4,
	{2, 4, 11, 22}
};
static const struct ieee80211_rateset ipw2200_rateset_11g = { 12,
	{2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108}
};

/*
 * Used by multi function thread
 */
extern pri_t minclsyspri;

/*
 * ipw2200 specific hardware operations
 */
static void	ipw2200_hwconf_get(struct ipw2200_softc *sc);
static int	ipw2200_chip_reset(struct ipw2200_softc *sc);
static void	ipw2200_master_stop(struct ipw2200_softc *sc);
static void	ipw2200_stop(struct ipw2200_softc *sc);
static int	ipw2200_config(struct ipw2200_softc *sc);
static int	ipw2200_cmd(struct ipw2200_softc *sc,
    uint32_t type, void *buf, size_t len, int async);
static void	ipw2200_ring_hwsetup(struct ipw2200_softc *sc);
static int	ipw2200_ring_alloc(struct ipw2200_softc *sc);
static void	ipw2200_ring_free(struct ipw2200_softc *sc);
static void	ipw2200_ring_reset(struct ipw2200_softc *sc);
static int	ipw2200_ring_init(struct ipw2200_softc *sc);

/*
 * GLD specific operations
 */
static int	ipw2200_m_stat(void *arg, uint_t stat, uint64_t *val);
static int	ipw2200_m_start(void *arg);
static void	ipw2200_m_stop(void *arg);
static int	ipw2200_m_unicst(void *arg, const uint8_t *macaddr);
static int	ipw2200_m_multicst(void *arg, boolean_t add, const uint8_t *m);
static int	ipw2200_m_promisc(void *arg, boolean_t on);
static void	ipw2200_m_ioctl(void *arg, queue_t *wq, mblk_t *mp);
static mblk_t  *ipw2200_m_tx(void *arg, mblk_t *mp);

/*
 * Interrupt and Data transferring operations
 */
static uint_t	ipw2200_intr(caddr_t arg);
static int	ipw2200_send(struct ieee80211com *ic, mblk_t *mp, uint8_t type);
static void	ipw2200_rcv_frame(struct ipw2200_softc *sc,
    struct ipw2200_frame *frame);
static void	ipw2200_rcv_notif(struct ipw2200_softc *sc,
    struct ipw2200_notif *notif);

/*
 * WiFi specific operations
 */
static int	ipw2200_newstate(struct ieee80211com *ic,
    enum ieee80211_state state, int arg);
static void	ipw2200_thread(struct ipw2200_softc *sc);

/*
 * IOCTL Handler
 */
static int	ipw2200_ioctl(struct ipw2200_softc *sc, queue_t *q, mblk_t *m);
static int	ipw2200_getset(struct ipw2200_softc *sc,
    mblk_t *m, uint32_t cmd, boolean_t *need_net80211);
static int	iwi_wificfg_radio(struct ipw2200_softc *sc,
    uint32_t cmd,  wldp_t *outfp);
static int	iwi_wificfg_desrates(wldp_t *outfp);

/*
 * Mac Call Back entries
 */
mac_callbacks_t	ipw2200_m_callbacks = {
	MC_IOCTL,
	ipw2200_m_stat,
	ipw2200_m_start,
	ipw2200_m_stop,
	ipw2200_m_promisc,
	ipw2200_m_multicst,
	ipw2200_m_unicst,
	ipw2200_m_tx,
	NULL,
	ipw2200_m_ioctl
};

/*
 * DEBUG Facility
 */
#define		MAX_MSG		(128)
uint32_t	ipw2200_debug = 0;
/*
 * supported debug marks are:
 *	| IPW2200_DBG_CSR
 *	| IPW2200_DBG_TABLE
 *	| IPW2200_DBG_HWCAP
 *	| IPW2200_DBG_TX
 *	| IPW2200_DBG_INIT
 *	| IPW2200_DBG_FW
 *	| IPW2200_DBG_NOTIF
 *	| IPW2200_DBG_SCAN
 *	| IPW2200_DBG_IOCTL
 *	| IPW2200_DBG_RING
 *	| IPW2200_DBG_INT
 *	| IPW2200_DBG_RX
 *	| IPW2200_DBG_DMA
 *	| IPW2200_DBG_GLD
 *	| IPW2200_DBG_WIFI
 *	| IPW2200_DBG_SOFTINT
 */

/*
 * Global tunning parameter to work around unknown hardware issues
 */
static uint32_t delay_config_stable	= 100000;	/* 100ms */
static uint32_t delay_fatal_recover	= 100000 * 20;	/* 2s */
static uint32_t delay_aux_thread	= 100000;	/* 100ms */

#define	IEEE80211_IS_CHAN_2GHZ(_c) \
	(((_c)->ich_flags & IEEE80211_CHAN_2GHZ) != 0)
#define	IEEE80211_IS_CHAN_5GHZ(_c) \
	(((_c)->ich_flags & IEEE80211_CHAN_5GHZ) != 0)
#define	isset(a, i)	((a)[(i)/NBBY] & (1 << ((i)%NBBY)))

void
ipw2200_dbg(dev_info_t *dip, int level, const char *fmt, ...)
{
	va_list	ap;
	char    buf[MAX_MSG];
	int	instance;

	va_start(ap, fmt);
	(void) vsnprintf(buf, sizeof (buf), fmt, ap);
	va_end(ap);

	if (dip) {
		instance = ddi_get_instance(dip);
		cmn_err(level, "%s%d: %s", IPW2200_DRV_NAME, instance, buf);
	} else
		cmn_err(level, "%s: %s", IPW2200_DRV_NAME, buf);

}

/*
 * Device operations
 */
int
ipw2200_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	struct ipw2200_softc	*sc;
	ddi_acc_handle_t	cfgh;
	caddr_t			regs;
	struct ieee80211com	*ic;
	int			instance, err, i;
	char			strbuf[32];
	wifi_data_t		wd = { 0 };
	mac_register_t		*macp;
	uint16_t		vendor, device, subven, subdev;

	if (cmd != DDI_ATTACH) {
		err = DDI_FAILURE;
		goto fail1;
	}

	instance = ddi_get_instance(dip);
	err = ddi_soft_state_zalloc(ipw2200_ssp, instance);
	if (err != DDI_SUCCESS) {
		IPW2200_WARN((dip, CE_WARN,
		    "ipw2200_attach(): unable to allocate soft state\n"));
		goto fail1;
	}
	sc = ddi_get_soft_state(ipw2200_ssp, instance);
	sc->sc_dip = dip;

	/*
	 * Map config spaces register to read the vendor id, device id, sub
	 * vendor id, and sub device id.
	 */
	err = ddi_regs_map_setup(dip, IPW2200_PCI_CFG_RNUM, &regs,
	    0, 0, &ipw2200_csr_accattr, &cfgh);
	if (err != DDI_SUCCESS) {
		IPW2200_WARN((dip, CE_WARN,
		    "ipw2200_attach(): unable to map spaces regs\n"));
		goto fail2;
	}
	ddi_put8(cfgh, (uint8_t *)(regs + 0x41), 0);
	vendor = ddi_get16(cfgh, (uint16_t *)(regs + PCI_CONF_VENID));
	device = ddi_get16(cfgh, (uint16_t *)(regs + PCI_CONF_DEVID));
	subven = ddi_get16(cfgh, (uint16_t *)(regs + PCI_CONF_SUBVENID));
	subdev = ddi_get16(cfgh, (uint16_t *)(regs + PCI_CONF_SUBSYSID));
	IPW2200_DBG(IPW2200_DBG_WIFI, (sc->sc_dip, CE_CONT,
	    "ipw2200_attach(): vendor = 0x%04x, devic = 0x%04x,"
	    "subversion = 0x%04x, subdev = 0x%04x",
	    vendor, device, subven, subdev));
	ddi_regs_map_free(&cfgh);

	/*
	 * Map operating registers
	 */
	err = ddi_regs_map_setup(dip, IPW2200_PCI_CSR_RNUM, &sc->sc_regs,
	    0, 0, &ipw2200_csr_accattr, &sc->sc_ioh);
	if (err != DDI_SUCCESS) {
		IPW2200_WARN((dip, CE_WARN,
		    "ipw2200_attach(): ddi_regs_map_setup() failed\n"));
		goto fail2;
	}

	/*
	 * Reset the chip
	 */
	err = ipw2200_chip_reset(sc);
	if (err != DDI_SUCCESS) {
		IPW2200_WARN((dip, CE_WARN,
		    "ipw2200_attach(): ipw2200_chip_reset() failed\n"));
		goto fail3;
	}

	/*
	 * Get the hardware configuration, including the MAC address
	 * Then, init all the rings needed.
	 */
	ipw2200_hwconf_get(sc);
	err = ipw2200_ring_init(sc);
	if (err != DDI_SUCCESS) {
		IPW2200_WARN((dip, CE_WARN,
		    "ipw2200_attach(): ipw2200_ring_init() failed\n"));
		goto fail3;
	}

	/*
	 * Initialize mutexs and condvars
	 */
	err = ddi_get_iblock_cookie(dip, 0, &sc->sc_iblk);
	if (err != DDI_SUCCESS) {
		IPW2200_WARN((dip, CE_WARN,
		    "ipw2200_attach(): ddi_get_iblock_cookie() failed\n"));
		goto fail4;
	}

	/*
	 * interrupt lock
	 */
	mutex_init(&sc->sc_ilock, "intr-lock", MUTEX_DRIVER,
	    (void *) sc->sc_iblk);
	cv_init(&sc->sc_fw_cond, "firmware-ok", CV_DRIVER, NULL);
	cv_init(&sc->sc_cmd_status_cond, "cmd-status-ring", CV_DRIVER, NULL);

	/*
	 * command ring lock
	 */
	mutex_init(&sc->sc_cmd_lock, "cmd-ring", MUTEX_DRIVER,
	    (void *) sc->sc_iblk);
	cv_init(&sc->sc_cmd_cond, "cmd-ring", CV_DRIVER, NULL);

	/*
	 * tx ring lock
	 */
	mutex_init(&sc->sc_tx_lock, "tx-ring", MUTEX_DRIVER,
	    (void *) sc->sc_iblk);

	/*
	 * multi-function lock, may acquire this during interrupt
	 */
	mutex_init(&sc->sc_mflock, "function-lock", MUTEX_DRIVER,
	    (void *) sc->sc_iblk);
	cv_init(&sc->sc_mfthread_cv, NULL, CV_DRIVER, NULL);
	sc->sc_mf_thread = NULL;
	sc->sc_mfthread_switch = 0;

	/*
	 * Initialize the WiFi part, which will be used by generic layer
	 * Need support more features in the furture, such as
	 * IEEE80211_C_IBSS
	 */
	ic = &sc->sc_ic;
	ic->ic_phytype  = IEEE80211_T_OFDM;
	ic->ic_opmode   = IEEE80211_M_STA;
	ic->ic_state    = IEEE80211_S_INIT;
	ic->ic_maxrssi  = 100; /* experimental number */
	ic->ic_caps = IEEE80211_C_SHPREAMBLE | IEEE80211_C_TXPMGT
	    | IEEE80211_C_PMGT | IEEE80211_C_WEP;

	/*
	 * set mac addr
	 */
	IEEE80211_ADDR_COPY(ic->ic_macaddr, sc->sc_macaddr);

	/*
	 * set supported .11a rates and channel - (2915ABG only)
	 */
	if (device >= 0x4223) {
		/* .11a rates */
		ic->ic_sup_rates[IEEE80211_MODE_11A] = ipw2200_rateset_11a;
		/* .11a channels */
		for (i = 36; i <= 64; i += 4) {
			ic->ic_sup_channels[i].ich_freq =
			    ieee80211_ieee2mhz(i, IEEE80211_CHAN_5GHZ);
			ic->ic_sup_channels[i].ich_flags = /* CHAN_A */
			    IEEE80211_CHAN_5GHZ | IEEE80211_CHAN_OFDM;
		}
		for (i = 149; i <= 165; i += 4) {
			ic->ic_sup_channels[i].ich_freq =
			    ieee80211_ieee2mhz(i, IEEE80211_CHAN_5GHZ);
			ic->ic_sup_channels[i].ich_flags = /* CHAN_A */
			    IEEE80211_CHAN_5GHZ | IEEE80211_CHAN_OFDM;
		}
	}

	/*
	 * set supported .11b and .11g rates
	 */
	ic->ic_sup_rates[IEEE80211_MODE_11B] = ipw2200_rateset_11b;
	ic->ic_sup_rates[IEEE80211_MODE_11G] = ipw2200_rateset_11g;

	/*
	 * set supported .11b and .11g channels(1 through 14)
	 */
	for (i = 1; i < 14; i++) {
		ic->ic_sup_channels[i].ich_freq  =
		    ieee80211_ieee2mhz(i, IEEE80211_CHAN_2GHZ);
		ic->ic_sup_channels[i].ich_flags =
		    IEEE80211_CHAN_CCK | IEEE80211_CHAN_OFDM |
		    IEEE80211_CHAN_DYN | IEEE80211_CHAN_2GHZ;
	}

	/*
	 * IBSS channal undefined for now
	 */
	ic->ic_ibss_chan = &ic->ic_sup_channels[0];
	ic->ic_xmit = ipw2200_send;

	/*
	 * init generic layer, then override state transition machine
	 */
	ieee80211_attach(ic);

	/*
	 * Override 80211 default routines
	 */
	ieee80211_media_init(ic); /* initial the node table and bss */
	sc->sc_newstate = ic->ic_newstate;
	ic->ic_newstate = ipw2200_newstate;
	ic->ic_def_txkey = 0;
	sc->sc_authmode = IEEE80211_AUTH_OPEN;

	/*
	 * Add the interrupt handler
	 */
	err = ddi_add_intr(dip, 0, &sc->sc_iblk, NULL,
	    ipw2200_intr, (caddr_t)sc);
	if (err != DDI_SUCCESS) {
		IPW2200_WARN((dip, CE_WARN,
		    "ipw2200_attach(): ddi_add_intr() failed\n"));
		goto fail5;
	}

	/*
	 * Initialize pointer to device specific functions
	 */
	wd.wd_secalloc = WIFI_SEC_NONE;
	wd.wd_opmode = ic->ic_opmode;
	IEEE80211_ADDR_COPY(wd.wd_bssid, ic->ic_macaddr);

	macp = mac_alloc(MAC_VERSION);
	if (err != 0) {
		IPW2200_WARN((dip, CE_WARN,
		    "ipw2200_attach(): mac_alloc() failed\n"));
		goto fail6;
	}

	macp->m_type_ident	= MAC_PLUGIN_IDENT_WIFI;
	macp->m_driver		= sc;
	macp->m_dip		= dip;
	macp->m_src_addr	= ic->ic_macaddr;
	macp->m_callbacks	= &ipw2200_m_callbacks;
	macp->m_min_sdu		= 0;
	macp->m_max_sdu		= IEEE80211_MTU;
	macp->m_pdata		= &wd;
	macp->m_pdata_size	= sizeof (wd);

	/*
	 * Register the macp to mac
	 */
	err = mac_register(macp, &ic->ic_mach);
	mac_free(macp);
	if (err != DDI_SUCCESS) {
		IPW2200_WARN((dip, CE_WARN,
		    "ipw2200_attach(): mac_register() failed\n"));
		goto fail6;
	}

	/*
	 * Create minor node of type DDI_NT_NET_WIFI
	 */
	(void) snprintf(strbuf, sizeof (strbuf), "%s%d",
	    IPW2200_DRV_NAME, instance);
	err = ddi_create_minor_node(dip, strbuf, S_IFCHR,
	    instance + 1, DDI_NT_NET_WIFI, 0);
	if (err != DDI_SUCCESS)
		IPW2200_WARN((dip, CE_WARN,
		    "ipw2200_attach(): ddi_create_minor_node() failed\n"));

	/*
	 * Cache firmware will always be true
	 */
	(void) ipw2200_cache_firmware(sc);

	/*
	 * Notify link is down now
	 */
	mac_link_update(ic->ic_mach, LINK_STATE_DOWN);

	/*
	 * Create the mf thread to handle the link status,
	 * recovery fatal error, etc.
	 */
	sc->sc_mfthread_switch = 1;
	if (sc->sc_mf_thread == NULL)
		sc->sc_mf_thread = thread_create((caddr_t)NULL, 0,
		    ipw2200_thread, sc, 0, &p0, TS_RUN, minclsyspri);

	return (DDI_SUCCESS);

fail6:
	ddi_remove_intr(dip, 0, sc->sc_iblk);
fail5:
	ieee80211_detach(ic);

	mutex_destroy(&sc->sc_ilock);
	mutex_destroy(&sc->sc_cmd_lock);
	mutex_destroy(&sc->sc_tx_lock);
	mutex_destroy(&sc->sc_mflock);
	cv_destroy(&sc->sc_fw_cond);
	cv_destroy(&sc->sc_cmd_status_cond);
	cv_destroy(&sc->sc_cmd_cond);
	cv_destroy(&sc->sc_mfthread_cv);
fail4:
	ipw2200_ring_free(sc);
fail3:
	ddi_regs_map_free(&sc->sc_ioh);
fail2:
	ddi_soft_state_free(ipw2200_ssp, instance);
fail1:
	return (err);
}


int
ipw2200_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	struct ipw2200_softc	*sc =
	    ddi_get_soft_state(ipw2200_ssp, ddi_get_instance(dip));
	int			err;
	ASSERT(sc != NULL);

	if (cmd != DDI_DETACH)
		return (DDI_FAILURE);

	ipw2200_stop(sc);

	/*
	 * Destroy the mf_thread
	 */
	mutex_enter(&sc->sc_mflock);
	sc->sc_mfthread_switch = 0;
	while (sc->sc_mf_thread != NULL) {
		if (cv_wait_sig(&sc->sc_mfthread_cv, &sc->sc_mflock) == 0)
			break;
	}
	mutex_exit(&sc->sc_mflock);

	/*
	 * Unregister from the MAC layer subsystem
	 */
	err = mac_unregister(sc->sc_ic.ic_mach);
	if (err != DDI_SUCCESS)
		return (err);

	ddi_remove_intr(dip, IPW2200_PCI_INTR_NUM, sc->sc_iblk);

	mutex_destroy(&sc->sc_ilock);
	mutex_destroy(&sc->sc_cmd_lock);
	mutex_destroy(&sc->sc_tx_lock);
	mutex_destroy(&sc->sc_mflock);
	cv_destroy(&sc->sc_fw_cond);
	cv_destroy(&sc->sc_cmd_status_cond);
	cv_destroy(&sc->sc_cmd_cond);
	cv_destroy(&sc->sc_mfthread_cv);

	/*
	 * Detach ieee80211
	 */
	ieee80211_detach(&sc->sc_ic);

	(void) ipw2200_free_firmware(sc);
	ipw2200_ring_free(sc);

	ddi_regs_map_free(&sc->sc_ioh);
	ddi_remove_minor_node(dip, NULL);
	ddi_soft_state_free(ipw2200_ssp, ddi_get_instance(dip));

	return (DDI_SUCCESS);
}

static void
ipw2200_stop(struct ipw2200_softc *sc)
{
	struct ieee80211com	*ic = &sc->sc_ic;

	IPW2200_DBG(IPW2200_DBG_HWCAP, (sc->sc_dip, CE_CONT,
	    "ipw2200_stop(): enter\n"));

	ipw2200_master_stop(sc);
	ipw2200_csr_put32(sc, IPW2200_CSR_RST, IPW2200_RST_SW_RESET);

	/*
	 * Reset ring
	 */
	ipw2200_ring_reset(sc);

	ieee80211_new_state(ic, IEEE80211_S_INIT, -1);
	sc->sc_flags &= ~IPW2200_FLAG_SCANNING;

	IPW2200_DBG(IPW2200_DBG_HWCAP, (sc->sc_dip, CE_CONT,
	    "ipw2200_stop(): exit\n"));
}

static int
ipw2200_config(struct ipw2200_softc *sc)
{
	struct ieee80211com		*ic = &sc->sc_ic;
	struct ipw2200_configuration	cfg;
	uint32_t			data;
	struct ipw2200_txpower		pwr;
	struct ipw2200_rateset		rs;
	struct ipw2200_wep_key		wkey;
	int				err, i;

	/*
	 * Set the IBSS mode channel: Tx power
	 */
	if (ic->ic_opmode == IEEE80211_M_IBSS) {
		pwr.mode  = IPW2200_MODE_11B;
		pwr.nchan = 11;
		for (i = 0; i < pwr.nchan; i++) {
			pwr.chan[i].chan  = i + 1;
			pwr.chan[i].power = IPW2200_TXPOWER_MAX;
		}
		IPW2200_DBG(IPW2200_DBG_WIFI, (sc->sc_dip, CE_CONT,
		    "ipw2200_config(): Setting .11b channels Tx power\n"));
		err = ipw2200_cmd(sc, IPW2200_CMD_SET_TX_POWER,
		    &pwr, sizeof (pwr), 0);
		if (err != DDI_SUCCESS)
			return (err);

		pwr.mode  = IPW2200_MODE_11G;
		IPW2200_DBG(IPW2200_DBG_WIFI, (sc->sc_dip, CE_CONT,
		    "ipw2200_config(): Setting .11g channels Tx power\n"));
		err = ipw2200_cmd(sc, IPW2200_CMD_SET_TX_POWER,
		    &pwr, sizeof (pwr), 0);
		if (err != DDI_SUCCESS)
			return (err);
	}

	/*
	 * Set MAC address
	 */
	IPW2200_DBG(IPW2200_DBG_WIFI, (sc->sc_dip, CE_CONT,
	    "ipw2200_config(): Setting MAC address to "
	    "%02x:%02x:%02x:%02x:%02x:%02x\n",
	    ic->ic_macaddr[0], ic->ic_macaddr[1], ic->ic_macaddr[2],
	    ic->ic_macaddr[3], ic->ic_macaddr[4], ic->ic_macaddr[5]));
	err = ipw2200_cmd(sc, IPW2200_CMD_SET_MAC_ADDRESS, ic->ic_macaddr,
	    IEEE80211_ADDR_LEN, 0);
	if (err != DDI_SUCCESS)
		return (err);

	/*
	 * Set basic system config settings: configuration(capabilities)
	 */
	(void) memset(&cfg, 0, sizeof (cfg));
	cfg.bluetooth_coexistence	 = 1;
	cfg.multicast_enabled		 = 1;
	cfg.answer_pbreq		 = 1;
	cfg.noise_reported		 = 1;

	IPW2200_DBG(IPW2200_DBG_WIFI, (sc->sc_dip, CE_CONT,
	    "ipw2200_config(): Configuring adapter\n"));
	err = ipw2200_cmd(sc, IPW2200_CMD_SET_CONFIG,
	    &cfg, sizeof (cfg), 0);
	if (err != DDI_SUCCESS)
		return (err);

	/*
	 * Set power mode
	 */
	data = LE_32(IPW2200_POWER_MODE_CAM);
	IPW2200_DBG(IPW2200_DBG_WIFI, (sc->sc_dip, CE_CONT,
	    "ipw2200_config(): Setting power mode to %u\n", LE_32(data)));
	err = ipw2200_cmd(sc, IPW2200_CMD_SET_POWER_MODE,
	    &data, sizeof (data), 0);
	if (err != DDI_SUCCESS)
		return (err);

	/*
	 * Set supported rates
	 */
	rs.mode = IPW2200_MODE_11G;
	rs.type = IPW2200_RATESET_TYPE_SUPPORTED;
	rs.nrates = ic->ic_sup_rates[IEEE80211_MODE_11G].ir_nrates;
	(void) memcpy(rs.rates, ic->ic_sup_rates[IEEE80211_MODE_11G].ir_rates,
	    rs.nrates);
	IPW2200_DBG(IPW2200_DBG_WIFI, (sc->sc_dip, CE_CONT,
	    "ipw2200_config(): Setting .11g supported rates(%u)\n", rs.nrates));
	err = ipw2200_cmd(sc, IPW2200_CMD_SET_RATES, &rs, sizeof (rs), 0);
	if (err != DDI_SUCCESS)
		return (err);

	rs.mode = IPW2200_MODE_11A;
	rs.type = IPW2200_RATESET_TYPE_SUPPORTED;
	rs.nrates = ic->ic_sup_rates[IEEE80211_MODE_11A].ir_nrates;
	(void) memcpy(rs.rates, ic->ic_sup_rates[IEEE80211_MODE_11A].ir_rates,
	    rs.nrates);
	IPW2200_DBG(IPW2200_DBG_WIFI, (sc->sc_dip, CE_CONT,
	    "ipw2200_config(): Setting .11a supported rates(%u)\n", rs.nrates));
	err = ipw2200_cmd(sc, IPW2200_CMD_SET_RATES, &rs, sizeof (rs), 0);
	if (err != DDI_SUCCESS)
		return (err);

	/*
	 * Set RTS(request-to-send) threshold
	 */
	data = LE_32(ic->ic_rtsthreshold);
	IPW2200_DBG(IPW2200_DBG_WIFI, (sc->sc_dip, CE_CONT,
	    "ipw2200_config(): Setting RTS threshold to %u\n", LE_32(data)));
	err = ipw2200_cmd(sc, IPW2200_CMD_SET_RTS_THRESHOLD, &data,
	    sizeof (data), 0);
	if (err != DDI_SUCCESS)
		return (err);

	/*
	 * Set fragmentation threshold
	 */
	data = LE_32(ic->ic_fragthreshold);
	IPW2200_DBG(IPW2200_DBG_WIFI, (sc->sc_dip, CE_CONT,
	    "ipw2200_config(): Setting fragmentation threshold to %u\n",
	    LE_32(data)));
	err = ipw2200_cmd(sc, IPW2200_CMD_SET_FRAG_THRESHOLD, &data,
	    sizeof (data), 0);
	if (err != DDI_SUCCESS)
		return (err);

	/*
	 * Set desired ESSID if we have
	 */
	if (ic->ic_des_esslen != 0) {
		IPW2200_DBG(IPW2200_DBG_WIFI, (sc->sc_dip, CE_CONT,
		    "ipw2200_config(): Setting desired ESSID to "
		    "(%u),%c%c%c%c%c%c%c%c\n",
		    ic->ic_des_esslen,
		    ic->ic_des_essid[0], ic->ic_des_essid[1],
		    ic->ic_des_essid[2], ic->ic_des_essid[3],
		    ic->ic_des_essid[4], ic->ic_des_essid[5],
		    ic->ic_des_essid[6], ic->ic_des_essid[7]));
		err = ipw2200_cmd(sc, IPW2200_CMD_SET_ESSID, ic->ic_des_essid,
		    ic->ic_des_esslen, 0);
		if (err != DDI_SUCCESS)
			return (err);
	}

	/*
	 * Set WEP initial vector(random seed)
	 */
	(void) random_get_pseudo_bytes((uint8_t *)&data, sizeof (data));
	IPW2200_DBG(IPW2200_DBG_WIFI, (sc->sc_dip, CE_CONT,
	    "ipw2200_config(): Setting initialization vector to %u\n",
	    LE_32(data)));
	err = ipw2200_cmd(sc, IPW2200_CMD_SET_IV, &data, sizeof (data), 0);
	if (err != DDI_SUCCESS)
		return (err);

	/*
	 * Set WEP if any
	 */
	if (ic->ic_flags & IEEE80211_F_PRIVACY) {
		IPW2200_DBG(IPW2200_DBG_WIFI, (sc->sc_dip, CE_CONT,
		    "ipw2200_config(): Setting Wep Key\n", LE_32(data)));
		for (i = 0; i < IEEE80211_WEP_NKID; i++) {
			wkey.cmd = IPW2200_WEP_KEY_CMD_SETKEY;
			wkey.idx = (uint8_t)i;
			wkey.len = ic->ic_nw_keys[i].wk_keylen;
			(void) memset(wkey.key, 0, sizeof (wkey.key));
			if (ic->ic_nw_keys[i].wk_keylen)
				(void) memcpy(wkey.key,
				    ic->ic_nw_keys[i].wk_key,
				    ic->ic_nw_keys[i].wk_keylen);
			err = ipw2200_cmd(sc, IPW2200_CMD_SET_WEP_KEY,
			    &wkey, sizeof (wkey), 0);
			if (err != DDI_SUCCESS)
				return (err);
		}
	}

	IPW2200_DBG(IPW2200_DBG_WIFI, (sc->sc_dip, CE_CONT,
	    "ipw2200_config(): Enabling adapter\n"));

	return (ipw2200_cmd(sc, IPW2200_CMD_ENABLE, NULL, 0, 0));
}

static int
ipw2200_cmd(struct ipw2200_softc *sc,
	uint32_t type, void *buf, size_t len, int async)
{
	struct		ipw2200_cmd_desc *cmd;
	clock_t		clk;
	uint32_t	idx;

	mutex_enter(&sc->sc_cmd_lock);
	while (sc->sc_cmd_free < 1)
		cv_wait(&sc->sc_cmd_cond, &sc->sc_cmd_lock);

	idx = sc->sc_cmd_cur;
	cmd = &sc->sc_cmdsc[idx];
	(void) memset(cmd, 0, sizeof (*cmd));

	IPW2200_DBG(IPW2200_DBG_RING, (sc->sc_dip, CE_CONT,
	    "ipw2200_cmd(): cmd-cur=%d\n", idx));

	cmd->hdr.type   = IPW2200_HDR_TYPE_COMMAND;
	cmd->hdr.flags  = IPW2200_HDR_FLAG_IRQ;
	cmd->type	= (uint8_t)type;
	if (len == 0 || buf == NULL)
		cmd->len  = 0;
	else {
		cmd->len  = (uint8_t)len;
		(void) memcpy(cmd->data, buf, len);
	}
	sc->sc_done[idx] = 0;

	/*
	 * DMA sync
	 */
	(void) ddi_dma_sync(sc->sc_dma_cmdsc.dr_hnd,
	    idx * sizeof (struct ipw2200_cmd_desc),
	    sizeof (struct ipw2200_cmd_desc), DDI_DMA_SYNC_FORDEV);

	sc->sc_cmd_cur = RING_FORWARD(sc->sc_cmd_cur, 1, IPW2200_CMD_RING_SIZE);
	sc->sc_cmd_free--;

	ipw2200_csr_put32(sc, IPW2200_CSR_CMD_WRITE_INDEX, sc->sc_cmd_cur);

	mutex_exit(&sc->sc_cmd_lock);

	if (async)
		goto out;

	/*
	 * Wait for command done
	 */
	mutex_enter(&sc->sc_ilock);
	while (sc->sc_done[idx] == 0) {
		/* pending */
		clk = ddi_get_lbolt() + drv_usectohz(5000000);  /* 5 second */
		if (cv_timedwait(&sc->sc_cmd_status_cond, &sc->sc_ilock, clk)
		    < 0)
			break;
	}
	mutex_exit(&sc->sc_ilock);

	IPW2200_DBG(IPW2200_DBG_RING, (sc->sc_dip, CE_CONT,
	    "ipw2200_cmd(): cmd-done=%s\n", sc->sc_done[idx] ? "yes" : "no"));

	if (sc->sc_done[idx] == 0)
		return (DDI_FAILURE);

out:
	return (DDI_SUCCESS);
}

/*
 * If init failed, it will call stop internally. Therefore, it's unnecessary
 * to call ipw2200_stop() when this subroutine is failed. Otherwise, it may
 * be called twice.
 */
int
ipw2200_init(struct ipw2200_softc *sc)
{
	int	err;

	/*
	 * No firmware is available, failed
	 */
	if (!(sc->sc_flags & IPW2200_FLAG_FW_CACHED)) {
		IPW2200_WARN((sc->sc_dip, CE_WARN,
		    "ipw2200_init(): no firmware is available\n"));
		return (DDI_FAILURE); /* return directly */
	}

	ipw2200_stop(sc);

	err = ipw2200_chip_reset(sc);
	if (err != DDI_SUCCESS) {
		IPW2200_WARN((sc->sc_dip, CE_WARN,
		    "ipw2200_init(): could not reset adapter\n"));
		goto fail;
	}

	/*
	 * Load boot code
	 */
	err = ipw2200_load_fw(sc, sc->sc_fw.boot_base, sc->sc_fw.boot_size);
	if (err != DDI_SUCCESS) {
		IPW2200_WARN((sc->sc_dip, CE_WARN,
		    "ipw2200_init(): could not load boot code\n"));
		goto fail;
	}

	/*
	 * Load boot microcode
	 */
	err = ipw2200_load_uc(sc, sc->sc_fw.uc_base, sc->sc_fw.uc_size);
	if (err != DDI_SUCCESS) {
		IPW2200_WARN((sc->sc_dip, CE_WARN,
		    "ipw2200_init(): could not load microcode\n"));
		goto fail;
	}

	ipw2200_master_stop(sc);
	ipw2200_ring_hwsetup(sc);

	/*
	 * Load firmware
	 */
	err = ipw2200_load_fw(sc, sc->sc_fw.fw_base, sc->sc_fw.fw_size);
	if (err != DDI_SUCCESS) {
		IPW2200_WARN((sc->sc_dip, CE_WARN,
		    "ipw2200_init(): could not load firmware\n"));
		goto fail;
	}

	sc->sc_flags |= IPW2200_FLAG_FW_INITED;

	/*
	 * Hardware will be enabled after configuration
	 */
	err = ipw2200_config(sc);
	if (err != DDI_SUCCESS) {
		IPW2200_WARN((sc->sc_dip, CE_WARN,
		    "ipw2200_init(): device configuration failed\n"));
		goto fail;
	}

	/*
	 * workround to prevent too many h/w error.
	 * delay for a while till h/w is stable.
	 */
	delay(drv_usectohz(delay_config_stable));

	ieee80211_begin_scan(&sc->sc_ic, 1); /* reset scan */
	return (DDI_SUCCESS); /* return successfully */
fail:
	ipw2200_stop(sc);
	return (err);
}

/*
 * get hardware configurations from EEPROM embedded within PRO/2200
 */
static void
ipw2200_hwconf_get(struct ipw2200_softc *sc)
{
	int		i;
	uint16_t	val;

	/*
	 * Get mac address
	 */
	i = 0;
	val = ipw2200_rom_get16(sc, IPW2200_EEPROM_MAC + 0);
	sc->sc_macaddr[i++] = val >> 8;
	sc->sc_macaddr[i++] = val & 0xff;
	val = ipw2200_rom_get16(sc, IPW2200_EEPROM_MAC + 1);
	sc->sc_macaddr[i++] = val >> 8;
	sc->sc_macaddr[i++] = val & 0xff;
	val = ipw2200_rom_get16(sc, IPW2200_EEPROM_MAC + 2);
	sc->sc_macaddr[i++] = val >> 8;
	sc->sc_macaddr[i++] = val & 0xff;

	/*
	 * formatted MAC address string
	 */
	(void) snprintf(sc->sc_macstr, sizeof (sc->sc_macstr),
	    "%02x:%02x:%02x:%02x:%02x:%02x",
	    sc->sc_macaddr[0], sc->sc_macaddr[1],
	    sc->sc_macaddr[2], sc->sc_macaddr[3],
	    sc->sc_macaddr[4], sc->sc_macaddr[5]);

}

/*
 * all ipw2200 interrupts will be masked by this routine
 */
static void
ipw2200_master_stop(struct ipw2200_softc *sc)
{
	int	ntries;

	/*
	 * disable interrupts
	 */
	ipw2200_csr_put32(sc, IPW2200_CSR_INTR_MASK, 0);
	ipw2200_csr_put32(sc, IPW2200_CSR_RST, IPW2200_RST_STOP_MASTER);

	/*
	 * wait long enough to ensure hardware stop successfully.
	 */
	for (ntries = 0; ntries < 500; ntries++) {
		if (ipw2200_csr_get32(sc, IPW2200_CSR_RST) &
		    IPW2200_RST_MASTER_DISABLED)
			break;
		/* wait for a while */
		drv_usecwait(100);
	}
	if (ntries == 500)
		IPW2200_WARN((sc->sc_dip, CE_WARN,
		    "ipw2200_master_stop(): timeout\n"));

	ipw2200_csr_put32(sc, IPW2200_CSR_RST,
	    IPW2200_RST_PRINCETON_RESET |
	    ipw2200_csr_get32(sc, IPW2200_CSR_RST));

	sc->sc_flags &= ~IPW2200_FLAG_FW_INITED;
}

/*
 * all ipw2200 interrupts will be masked by this routine
 */
static int
ipw2200_chip_reset(struct ipw2200_softc *sc)
{
	uint32_t	tmp;
	int		ntries, i;

	ipw2200_master_stop(sc);

	/*
	 * Move adapter to DO state
	 */
	tmp = ipw2200_csr_get32(sc, IPW2200_CSR_CTL);
	ipw2200_csr_put32(sc, IPW2200_CSR_CTL, tmp | IPW2200_CTL_INIT);

	/*
	 * Initialize Phase-Locked Level (PLL)
	 */
	ipw2200_csr_put32(sc, IPW2200_CSR_READ_INT, IPW2200_READ_INT_INIT_HOST);

	/*
	 * Wait for clock stabilization
	 */
	for (ntries = 0; ntries < 1000; ntries++) {
		if (ipw2200_csr_get32(sc, IPW2200_CSR_CTL) &
		    IPW2200_CTL_CLOCK_READY)
			break;
		drv_usecwait(200);
	}
	if (ntries == 1000) {
		IPW2200_WARN((sc->sc_dip, CE_WARN,
		    "ipw2200_chip_reset(): timeout\n"));
		return (DDI_FAILURE);
	}

	tmp = ipw2200_csr_get32(sc, IPW2200_CSR_RST);
	ipw2200_csr_put32(sc, IPW2200_CSR_RST, tmp | IPW2200_RST_SW_RESET);

	drv_usecwait(10);

	tmp = ipw2200_csr_get32(sc, IPW2200_CSR_CTL);
	ipw2200_csr_put32(sc, IPW2200_CSR_CTL, tmp | IPW2200_CTL_INIT);

	/*
	 * clear NIC memory
	 */
	ipw2200_csr_put32(sc, IPW2200_CSR_AUTOINC_ADDR, 0);
	for (i = 0; i < 0xc000; i++)
		ipw2200_csr_put32(sc, IPW2200_CSR_AUTOINC_DATA, 0);

	return (DDI_SUCCESS);
}

/*
 * This function is used by wificonfig/dladm to get the current
 * radio status, it is off/on
 */
int
ipw2200_radio_status(struct ipw2200_softc *sc)
{
	int	val;

	val = (ipw2200_csr_get32(sc, IPW2200_CSR_IO) &
	    IPW2200_IO_RADIO_ENABLED) ? 1 : 0;

	return (val);
}
/*
 * This function is used to get the statistic
 */
void
ipw2200_get_statistics(struct ipw2200_softc *sc)
{
	struct ieee80211com	*ic = &sc->sc_ic;

	uint32_t size, buf[128];

	if (!(sc->sc_flags & IPW2200_FLAG_FW_INITED)) {
		IPW2200_DBG(IPW2200_DBG_IOCTL, (sc->sc_dip, CE_CONT,
		    "ipw2200_get_statistic(): fw doesn't download yet."));
		return;
	}

	size = min(ipw2200_csr_get32(sc, IPW2200_CSR_TABLE0_SIZE), 128 - 1);
	ipw2200_csr_getbuf32(sc, IPW2200_CSR_TABLE0_BASE, &buf[1], size);

	/*
	 * To retrieve the statistic information into proper places. There are
	 * lot of information. These table will be read once a second.
	 * Hopefully, it will not effect the performance.
	 */

	/*
	 * For the tx/crc information, we can get them from chip directly;
	 * For the rx/wep error/(rts) related information, leave them net80211.
	 */
	/* WIFI_STAT_TX_FRAGS */
	ic->ic_stats.is_tx_frags = (uint32_t)buf[5];
	/* WIFI_STAT_MCAST_TX */
	ic->ic_stats.is_tx_mcast = (uint32_t)buf[31];
	/* WIFI_STAT_TX_RETRANS */
	ic->ic_stats.is_tx_retries = (uint32_t)buf[56];
	/* WIFI_STAT_TX_FAILED */
	ic->ic_stats.is_tx_failed = (uint32_t)buf[57];
	/* MAC_STAT_OBYTES */
	ic->ic_stats.is_tx_bytes = (uint32_t)buf[64];
}

/*
 * DMA region alloc subroutine
 */
int
ipw2200_dma_region_alloc(struct ipw2200_softc *sc, struct dma_region *dr,
	size_t size, uint_t dir, uint_t flags)
{
	dev_info_t	*dip = sc->sc_dip;
	int		err;

	IPW2200_DBG(IPW2200_DBG_DMA, (sc->sc_dip, CE_CONT,
	    "ipw2200_dma_region_alloc(): size =%u\n", size));

	err = ddi_dma_alloc_handle(dip, &ipw2200_dma_attr, DDI_DMA_SLEEP, NULL,
	    &dr->dr_hnd);
	if (err != DDI_SUCCESS) {
		IPW2200_DBG(IPW2200_DBG_DMA, (sc->sc_dip, CE_CONT,
		    "ipw2200_dma_region_alloc(): "
		    "ddi_dma_alloc_handle() failed\n"));
		goto fail0;
	}

	err = ddi_dma_mem_alloc(dr->dr_hnd, size, &ipw2200_dma_accattr,
	    flags, DDI_DMA_SLEEP, NULL,
	    &dr->dr_base, &dr->dr_size, &dr->dr_acc);
	if (err != DDI_SUCCESS) {
		IPW2200_DBG(IPW2200_DBG_DMA, (sc->sc_dip, CE_CONT,
		    "ipw2200_dma_region_alloc(): "
		    "ddi_dma_mem_alloc() failed\n"));
		goto fail1;
	}

	err = ddi_dma_addr_bind_handle(dr->dr_hnd, NULL,
	    dr->dr_base, dr->dr_size,
	    dir | flags, DDI_DMA_SLEEP, NULL,
	    &dr->dr_cookie, &dr->dr_ccnt);
	if (err != DDI_DMA_MAPPED) {
		IPW2200_DBG(IPW2200_DBG_DMA, (sc->sc_dip, CE_CONT,
		    "ipw2200_dma_region_alloc(): "
		    "ddi_dma_addr_bind_handle() failed\n"));
		goto fail2;
	}

	IPW2200_DBG(IPW2200_DBG_DMA, (sc->sc_dip, CE_CONT,
	    "ipw2200_dma_region_alloc(): ccnt=%u\n", dr->dr_ccnt));

	if (dr->dr_ccnt != 1) {
		err = DDI_FAILURE;
		goto fail3;
	}

	dr->dr_pbase = dr->dr_cookie.dmac_address;

	IPW2200_DBG(IPW2200_DBG_DMA, (sc->sc_dip, CE_CONT,
	    "ipw2200_dma_region_alloc(): get physical-base=0x%08x\n",
	    dr->dr_pbase));

	return (DDI_SUCCESS);

fail3:
	(void) ddi_dma_unbind_handle(dr->dr_hnd);
fail2:
	ddi_dma_mem_free(&dr->dr_acc);
fail1:
	ddi_dma_free_handle(&dr->dr_hnd);
fail0:
	return (err);
}

void
ipw2200_dma_region_free(struct dma_region *dr)
{
	(void) ddi_dma_unbind_handle(dr->dr_hnd);
	ddi_dma_mem_free(&dr->dr_acc);
	ddi_dma_free_handle(&dr->dr_hnd);
}

static int
ipw2200_ring_alloc(struct ipw2200_softc *sc)
{
	int	err, i;

	/*
	 * tx desc ring
	 */
	sc->sc_dma_txdsc.dr_name = "ipw2200-tx-desc-ring";
	err = ipw2200_dma_region_alloc(sc, &sc->sc_dma_txdsc,
	    IPW2200_TX_RING_SIZE * sizeof (struct ipw2200_tx_desc),
	    DDI_DMA_WRITE, DDI_DMA_CONSISTENT);
	if (err != DDI_SUCCESS)
		goto fail0;
	/*
	 * tx buffer array
	 */
	for (i = 0; i < IPW2200_TX_RING_SIZE; i++) {
		sc->sc_dma_txbufs[i].dr_name = "ipw2200-tx-buf";
		err = ipw2200_dma_region_alloc(sc, &sc->sc_dma_txbufs[i],
		    IPW2200_TXBUF_SIZE, DDI_DMA_WRITE, DDI_DMA_STREAMING);
		if (err != DDI_SUCCESS) {
			while (i >= 0) {
				ipw2200_dma_region_free(&sc->sc_dma_txbufs[i]);
				i--;
			}
			goto fail1;
		}
	}
	/*
	 * rx buffer array
	 */
	for (i = 0; i < IPW2200_RX_RING_SIZE; i++) {
		sc->sc_dma_rxbufs[i].dr_name = "ipw2200-rx-buf";
		err = ipw2200_dma_region_alloc(sc, &sc->sc_dma_rxbufs[i],
		    IPW2200_RXBUF_SIZE, DDI_DMA_READ, DDI_DMA_STREAMING);
		if (err != DDI_SUCCESS) {
			while (i >= 0) {
				ipw2200_dma_region_free(&sc->sc_dma_rxbufs[i]);
				i--;
			}
			goto fail2;
		}
	}
	/*
	 * cmd desc ring
	 */
	sc->sc_dma_cmdsc.dr_name = "ipw2200-cmd-desc-ring";
	err = ipw2200_dma_region_alloc(sc, &sc->sc_dma_cmdsc,
	    IPW2200_CMD_RING_SIZE * sizeof (struct ipw2200_cmd_desc),
	    DDI_DMA_WRITE, DDI_DMA_CONSISTENT);
	if (err != DDI_SUCCESS)
		goto fail3;

	return (DDI_SUCCESS);

fail3:
	for (i = 0; i < IPW2200_RX_RING_SIZE; i++)
		ipw2200_dma_region_free(&sc->sc_dma_rxbufs[i]);
fail2:
	for (i = 0; i < IPW2200_TX_RING_SIZE; i++)
		ipw2200_dma_region_free(&sc->sc_dma_txbufs[i]);
fail1:
	ipw2200_dma_region_free(&sc->sc_dma_txdsc);
fail0:
	return (err);
}

static void
ipw2200_ring_free(struct ipw2200_softc *sc)
{
	int	i;

	/*
	 * tx ring desc
	 */
	ipw2200_dma_region_free(&sc->sc_dma_txdsc);
	/*
	 * tx buf
	 */
	for (i = 0; i < IPW2200_TX_RING_SIZE; i++)
		ipw2200_dma_region_free(&sc->sc_dma_txbufs[i]);
	/*
	 * rx buf
	 */
	for (i = 0; i < IPW2200_RX_RING_SIZE; i++)
		ipw2200_dma_region_free(&sc->sc_dma_rxbufs[i]);
	/*
	 * command ring desc
	 */
	ipw2200_dma_region_free(&sc->sc_dma_cmdsc);
}

static void
ipw2200_ring_reset(struct ipw2200_softc *sc)
{
	int i;

	/*
	 * tx desc ring & buffer array
	 */
	sc->sc_tx_cur   = 0;
	sc->sc_tx_free  = IPW2200_TX_RING_SIZE;
	sc->sc_txdsc    = (struct ipw2200_tx_desc *)sc->sc_dma_txdsc.dr_base;
	for (i = 0; i < IPW2200_TX_RING_SIZE; i++)
		sc->sc_txbufs[i] = (uint8_t *)sc->sc_dma_txbufs[i].dr_base;
	/*
	 * rx buffer array
	 */
	sc->sc_rx_cur   = 0;
	sc->sc_rx_free  = IPW2200_RX_RING_SIZE;
	for (i = 0; i < IPW2200_RX_RING_SIZE; i++)
		sc->sc_rxbufs[i] = (uint8_t *)sc->sc_dma_rxbufs[i].dr_base;

	/*
	 * command desc ring
	 */
	sc->sc_cmd_cur  = 0;
	sc->sc_cmd_free = IPW2200_CMD_RING_SIZE;
	sc->sc_cmdsc    = (struct ipw2200_cmd_desc *)sc->sc_dma_cmdsc.dr_base;
}

/*
 * tx, rx rings and command initialization
 */
static int
ipw2200_ring_init(struct ipw2200_softc *sc)
{
	int	err;

	err = ipw2200_ring_alloc(sc);
	if (err != DDI_SUCCESS)
		return (err);

	ipw2200_ring_reset(sc);

	return (DDI_SUCCESS);
}

static void
ipw2200_ring_hwsetup(struct ipw2200_softc *sc)
{
	int	i;

	/*
	 * command desc ring
	 */
	ipw2200_csr_put32(sc, IPW2200_CSR_CMD_BASE, sc->sc_dma_cmdsc.dr_pbase);
	ipw2200_csr_put32(sc, IPW2200_CSR_CMD_SIZE, IPW2200_CMD_RING_SIZE);
	ipw2200_csr_put32(sc, IPW2200_CSR_CMD_WRITE_INDEX, sc->sc_cmd_cur);

	/*
	 * tx desc ring.  only tx1 is used, tx2, tx3, and tx4 are unused
	 */
	ipw2200_csr_put32(sc, IPW2200_CSR_TX1_BASE, sc->sc_dma_txdsc.dr_pbase);
	ipw2200_csr_put32(sc, IPW2200_CSR_TX1_SIZE, IPW2200_TX_RING_SIZE);
	ipw2200_csr_put32(sc, IPW2200_CSR_TX1_WRITE_INDEX, sc->sc_tx_cur);

	/*
	 * tx2, tx3, tx4 is not used
	 */
	ipw2200_csr_put32(sc, IPW2200_CSR_TX2_BASE, sc->sc_dma_txdsc.dr_pbase);
	ipw2200_csr_put32(sc, IPW2200_CSR_TX2_SIZE, IPW2200_TX_RING_SIZE);
	ipw2200_csr_put32(sc, IPW2200_CSR_TX2_READ_INDEX, 0);
	ipw2200_csr_put32(sc, IPW2200_CSR_TX2_WRITE_INDEX, 0);
	ipw2200_csr_put32(sc, IPW2200_CSR_TX3_BASE, sc->sc_dma_txdsc.dr_pbase);
	ipw2200_csr_put32(sc, IPW2200_CSR_TX3_SIZE, IPW2200_TX_RING_SIZE);
	ipw2200_csr_put32(sc, IPW2200_CSR_TX3_READ_INDEX, 0);
	ipw2200_csr_put32(sc, IPW2200_CSR_TX3_WRITE_INDEX, 0);
	ipw2200_csr_put32(sc, IPW2200_CSR_TX4_BASE, sc->sc_dma_txdsc.dr_pbase);
	ipw2200_csr_put32(sc, IPW2200_CSR_TX4_SIZE, IPW2200_TX_RING_SIZE);
	ipw2200_csr_put32(sc, IPW2200_CSR_TX4_READ_INDEX, 0);
	ipw2200_csr_put32(sc, IPW2200_CSR_TX4_WRITE_INDEX, 0);

	/*
	 * rx buffer ring
	 */
	for (i = 0; i < IPW2200_RX_RING_SIZE; i++)
		ipw2200_csr_put32(sc, IPW2200_CSR_RX_BASE + i * 4,
		    sc->sc_dma_rxbufs[i].dr_pbase);
	/*
	 * all rx buffer are empty, rx-rd-index == 0 && rx-wr-index == N-1
	 */
	ipw2200_csr_put32(sc, IPW2200_CSR_RX_WRITE_INDEX,
	    RING_BACKWARD(sc->sc_rx_cur, 1, IPW2200_RX_RING_SIZE));
}

int
ipw2200_start_scan(struct ipw2200_softc *sc)
{
	struct ieee80211com	*ic = &sc->sc_ic;
	struct ipw2200_scan	scan;
	uint8_t			*ch;
	int			cnt, i;

	IPW2200_DBG(IPW2200_DBG_SCAN, (sc->sc_dip, CE_CONT,
	    "ipw2200_start_scan(): start scanning \n"));

	/*
	 * start scanning
	 */
	sc->sc_flags |= IPW2200_FLAG_SCANNING;

	(void) memset(&scan, 0, sizeof (scan));
	scan.type = (ic->ic_des_esslen != 0) ? IPW2200_SCAN_TYPE_BDIRECTED :
	    IPW2200_SCAN_TYPE_BROADCAST;
	scan.dwelltime = LE_16(40); /* The interval is set up to 40 */

	/*
	 * Compact supported channel number(5G) into a single buffer
	 */
	ch = scan.channels;
	cnt = 0;
	for (i = 0; i <= IEEE80211_CHAN_MAX; i++) {
		if (IEEE80211_IS_CHAN_5GHZ(&ic->ic_sup_channels[i]) &&
		    isset(ic->ic_chan_active, i)) {
			*++ch = (uint8_t)i;
			cnt++;
		}
	}
	*(ch - cnt) = IPW2200_CHAN_5GHZ | (uint8_t)cnt;

	/*
	 * Compact supported channel number(2G) into a single buffer
	 */
	cnt = 0;
	for (i = 0; i <= IEEE80211_CHAN_MAX; i++) {
		if (IEEE80211_IS_CHAN_2GHZ(&ic->ic_sup_channels[i]) &&
		    isset(ic->ic_chan_active, i)) {
			*++ch = (uint8_t)i;
			cnt++;
		}
	}
	*(ch - cnt) = IPW2200_CHAN_2GHZ | cnt;

	return (ipw2200_cmd(sc, IPW2200_CMD_SCAN, &scan, sizeof (scan), 1));
}

int
ipw2200_auth_and_assoc(struct ipw2200_softc *sc)
{
	struct ieee80211com		*ic = &sc->sc_ic;
	struct ieee80211_node		*in = ic->ic_bss;
	struct ipw2200_configuration	cfg;
	struct ipw2200_rateset		rs;
	struct ipw2200_associate	assoc;
	uint32_t			data;
	int				err;

	/*
	 * set the confiuration
	 */
	if (IEEE80211_IS_CHAN_2GHZ(in->in_chan)) {
		/* enable b/g auto-detection */
		(void) memset(&cfg, 0, sizeof (cfg));
		cfg.bluetooth_coexistence = 1;
		cfg.multicast_enabled	  = 1;
		cfg.use_protection	  = 1;
		cfg.answer_pbreq	  = 1;
		cfg.noise_reported	  = 1;
		err = ipw2200_cmd(sc, IPW2200_CMD_SET_CONFIG,
		    &cfg, sizeof (cfg), 1);
		if (err != DDI_SUCCESS)
			return (err);
	}

	/*
	 * set the essid, may be null/hidden AP
	 */
	IPW2200_DBG(IPW2200_DBG_WIFI, (sc->sc_dip, CE_CONT,
	    "ipw2200_auth_and_assoc(): "
	    "setting ESSID to(%u),%c%c%c%c%c%c%c%c\n",
	    in->in_esslen,
	    in->in_essid[0], in->in_essid[1],
	    in->in_essid[2], in->in_essid[3],
	    in->in_essid[4], in->in_essid[5],
	    in->in_essid[6], in->in_essid[7]));
	err = ipw2200_cmd(sc, IPW2200_CMD_SET_ESSID, in->in_essid,
	    in->in_esslen, 1);
	if (err != DDI_SUCCESS)
		return (err);

	/*
	 * set the rate: the rate set has already been ''negocitated''
	 */
	rs.mode = IEEE80211_IS_CHAN_5GHZ(in->in_chan) ?
	    IPW2200_MODE_11A : IPW2200_MODE_11G;
	rs.type = IPW2200_RATESET_TYPE_NEGOCIATED;
	rs.nrates = in->in_rates.ir_nrates;
	(void) memcpy(rs.rates, in->in_rates.ir_rates, in->in_rates.ir_nrates);
	IPW2200_DBG(IPW2200_DBG_WIFI, (sc->sc_dip, CE_CONT,
	    "ipw2200_auth_and_assoc(): "
	    "setting negotiated rates to(nrates = %u)\n", rs.nrates));

	err = ipw2200_cmd(sc, IPW2200_CMD_SET_RATES, &rs, sizeof (rs), 1);
	if (err != DDI_SUCCESS)
		return (err);

	/*
	 * set the sensitive
	 */
	data = LE_32(in->in_rssi);
	IPW2200_DBG(IPW2200_DBG_WIFI, (sc->sc_dip, CE_CONT,
	    "ipw2200_auth_and_assoc(): "
	    "setting sensitivity to rssi:(%u)\n", (uint8_t)in->in_rssi));
	err = ipw2200_cmd(sc, IPW2200_CMD_SET_SENSITIVITY,
	    &data, sizeof (data), 1);
	if (err != DDI_SUCCESS)
		return (err);

	/*
	 * invoke command associate
	 */
	(void) memset(&assoc, 0, sizeof (assoc));
	assoc.mode = IEEE80211_IS_CHAN_5GHZ(in->in_chan) ?
	    IPW2200_MODE_11A : IPW2200_MODE_11G;
	assoc.chan = ieee80211_chan2ieee(ic, in->in_chan);
	/*
	 * use the value set to ic_bss to retraive current sharedmode
	 */
	if (ic->ic_bss->in_authmode == WL_SHAREDKEY) {
		assoc.auth = (ic->ic_def_txkey << 4) | IPW2200_AUTH_SHARED;
		IPW2200_DBG(IPW2200_DBG_IOCTL, (sc->sc_dip, CE_CONT,
		    "ipw2200_auth_and_assoc(): "
		    "associate to shared key mode, set thru. ioctl"));
	}
	(void) memcpy(assoc.tstamp, in->in_tstamp.data, 8);
	assoc.capinfo = LE_16(in->in_capinfo);
	assoc.lintval = LE_16(ic->ic_lintval);
	assoc.intval  = LE_16(in->in_intval);
	IEEE80211_ADDR_COPY(assoc.bssid, in->in_bssid);
	if (ic->ic_opmode == IEEE80211_M_IBSS)
		IEEE80211_ADDR_COPY(assoc.dst, ipw2200_broadcast_addr);
	else
		IEEE80211_ADDR_COPY(assoc.dst, in->in_bssid);

	IPW2200_DBG(IPW2200_DBG_WIFI, (sc->sc_dip, CE_CONT,
	    "ipw2200_auth_and_assoc(): "
	    "associate to bssid(%2x:%2x:%2x:%2x:%2x:%2x:), "
	    "chan(%u), auth(%u)\n",
	    assoc.bssid[0], assoc.bssid[1], assoc.bssid[2],
	    assoc.bssid[3], assoc.bssid[4], assoc.bssid[5],
	    assoc.chan, assoc.auth));
	return (ipw2200_cmd(sc, IPW2200_CMD_ASSOCIATE,
	    &assoc, sizeof (assoc), 1));
}

/* ARGSUSED */
static int
ipw2200_newstate(struct ieee80211com *ic, enum ieee80211_state state, int arg)
{
	struct ipw2200_softc	*sc = (struct ipw2200_softc *)ic;
	wifi_data_t		wd = { 0 };

	switch (state) {
	case IEEE80211_S_SCAN:
		if (!(sc->sc_flags & IPW2200_FLAG_SCANNING)) {
			ic->ic_flags |= IEEE80211_F_SCAN | IEEE80211_F_ASCAN;
			(void) ipw2200_start_scan(sc);
		}
		break;
	case IEEE80211_S_AUTH:
		(void) ipw2200_auth_and_assoc(sc);
		break;
	case IEEE80211_S_RUN:
		/*
		 * We can send data now; update the fastpath with our
		 * current associated BSSID and other relevant settings.
		 *
		 * Hardware to ahndle the wep encryption. Set the encryption
		 * as false.
		 * wd.wd_wep    = ic->ic_flags & IEEE80211_F_WEPON;
		 */
		wd.wd_secalloc	= WIFI_SEC_NONE;
		wd.wd_opmode	= ic->ic_opmode;
		IEEE80211_ADDR_COPY(wd.wd_bssid, ic->ic_bss->in_bssid);
		(void) mac_pdata_update(ic->ic_mach, &wd, sizeof (wd));
		break;
	case IEEE80211_S_ASSOC:
	case IEEE80211_S_INIT:
		break;
	}

	/*
	 * notify to update the link
	 */
	if ((ic->ic_state != IEEE80211_S_RUN) && (state == IEEE80211_S_RUN)) {
		mac_link_update(ic->ic_mach, LINK_STATE_UP);
	} else if ((ic->ic_state == IEEE80211_S_RUN) &&
		(state != IEEE80211_S_RUN)) {
		mac_link_update(ic->ic_mach, LINK_STATE_DOWN);
	}

	IPW2200_DBG(IPW2200_DBG_WIFI, (sc->sc_dip, CE_CONT,
	    "ipw2200_newstat(): %s -> %s\n",
	    ieee80211_state_name[ic->ic_state],
	    ieee80211_state_name[state]));

	ic->ic_state = state;
	return (DDI_SUCCESS);
}

/*
 * GLD operations
 */
/* ARGSUSED */
static int
ipw2200_m_stat(void *arg, uint_t stat, uint64_t *val)
{
	ieee80211com_t	*ic = (ieee80211com_t *)arg;

	IPW2200_DBG(IPW2200_DBG_GLD, (((struct ipw2200_softc *)arg)->sc_dip,
	    CE_CONT,
	    "ipw2200_m_stat(): enter\n"));

	/*
	 * Some of below statistic data are from hardware, some from net80211
	 */
	switch (stat) {
	case MAC_STAT_RBYTES:
		*val = ic->ic_stats.is_rx_bytes;
		break;
	case MAC_STAT_IPACKETS:
		*val = ic->ic_stats.is_rx_frags;
		break;
	case MAC_STAT_OBYTES:
		*val = ic->ic_stats.is_tx_bytes;
		break;
	case MAC_STAT_OPACKETS:
		*val = ic->ic_stats.is_tx_frags;
		break;
	/*
	 * Get below from hardware statistic, retraive net80211 value once 1s
	 */
	case WIFI_STAT_TX_FRAGS:
	case WIFI_STAT_MCAST_TX:
	case WIFI_STAT_TX_FAILED:
	case WIFI_STAT_TX_RETRANS:
	/*
	 * Get blow information from net80211
	 */
	case WIFI_STAT_RTS_SUCCESS:
	case WIFI_STAT_RTS_FAILURE:
	case WIFI_STAT_ACK_FAILURE:
	case WIFI_STAT_RX_FRAGS:
	case WIFI_STAT_MCAST_RX:
	case WIFI_STAT_RX_DUPS:
	case WIFI_STAT_FCS_ERRORS:
	case WIFI_STAT_WEP_ERRORS:
		return (ieee80211_stat(ic, stat, val));
	/*
	 * Need be supported later
	 */
	case MAC_STAT_IFSPEED:
	case MAC_STAT_NOXMTBUF:
	case MAC_STAT_IERRORS:
	case MAC_STAT_OERRORS:
	default:
		return (ENOTSUP);
	}
	return (0);
}

/* ARGSUSED */
static int
ipw2200_m_multicst(void *arg, boolean_t add, const uint8_t *mca)
{
	/* not supported */
	IPW2200_DBG(IPW2200_DBG_GLD, (((struct ipw2200_softc *)arg)->sc_dip,
	    CE_CONT,
	    "ipw2200_m_multicst(): enter\n"));

	return (DDI_SUCCESS);
}

/*
 * Multithread handler for linkstatus, fatal error recovery, get statistic
 */
static void
ipw2200_thread(struct ipw2200_softc *sc)
{
	struct ieee80211com	*ic = &sc->sc_ic;
	int32_t			nlstate;
	int			stat_cnt = 0;

	IPW2200_DBG(IPW2200_DBG_SOFTINT, (sc->sc_dip, CE_CONT,
	    "ipw2200_thread(): enter, linkstate %d\n", sc->sc_linkstate));

	mutex_enter(&sc->sc_mflock);

	while (sc->sc_mfthread_switch) {
		/*
		 * notify the link state
		 */
		if (ic->ic_mach && (sc->sc_flags & IPW2200_FLAG_LINK_CHANGE)) {

			IPW2200_DBG(IPW2200_DBG_SOFTINT, (sc->sc_dip, CE_CONT,
			    "ipw2200_thread(): link status --> %d\n",
			    sc->sc_linkstate));

			sc->sc_flags &= ~IPW2200_FLAG_LINK_CHANGE;
			nlstate = sc->sc_linkstate;

			mutex_exit(&sc->sc_mflock);
			mac_link_update(ic->ic_mach, nlstate);
			mutex_enter(&sc->sc_mflock);
		}

		/*
		 * recovery fatal error
		 */
		if (ic->ic_mach &&
		    (sc->sc_flags & IPW2200_FLAG_HW_ERR_RECOVER)) {

			IPW2200_DBG(IPW2200_DBG_FATAL, (sc->sc_dip, CE_CONT,
			    "ipw2200_thread(): "
			    "try to recover fatal hw error\n"));

			sc->sc_flags &= ~IPW2200_FLAG_HW_ERR_RECOVER;

			mutex_exit(&sc->sc_mflock);
			(void) ipw2200_init(sc); /* Force state machine */
			/*
			 * workround. Delay for a while after init especially
			 * when something wrong happened already.
			 */
			delay(drv_usectohz(delay_fatal_recover));
			mutex_enter(&sc->sc_mflock);
		}

		/*
		 * get statistic, the value will be retrieved by m_stat
		 */
		if (stat_cnt == 10) {

			stat_cnt = 0; /* re-start */
			mutex_exit(&sc->sc_mflock);
			ipw2200_get_statistics(sc);
			mutex_enter(&sc->sc_mflock);

		} else
			stat_cnt++; /* until 1s */

		mutex_exit(&sc->sc_mflock);
		delay(drv_usectohz(delay_aux_thread));
		mutex_enter(&sc->sc_mflock);

	}
	sc->sc_mf_thread = NULL;
	cv_signal(&sc->sc_mfthread_cv);
	mutex_exit(&sc->sc_mflock);
}

static int
ipw2200_m_start(void *arg)
{
	struct ipw2200_softc	*sc = (struct ipw2200_softc *)arg;

	IPW2200_DBG(IPW2200_DBG_GLD, (sc->sc_dip, CE_CONT,
	    "ipw2200_m_start(): enter\n"));
	/*
	 * initialize ipw2200 hardware, everything ok will start scan
	 */
	(void) ipw2200_init(sc);

	sc->sc_flags |= IPW2200_FLAG_RUNNING;

	return (DDI_SUCCESS);
}

static void
ipw2200_m_stop(void *arg)
{
	struct ipw2200_softc	*sc = (struct ipw2200_softc *)arg;

	IPW2200_DBG(IPW2200_DBG_GLD, (sc->sc_dip, CE_CONT,
	    "ipw2200_m_stop(): enter\n"));

	ipw2200_stop(sc);

	sc->sc_flags &= ~IPW2200_FLAG_RUNNING;
}

static int
ipw2200_m_unicst(void *arg, const uint8_t *macaddr)
{
	struct ipw2200_softc	*sc = (struct ipw2200_softc *)arg;
	struct ieee80211com	*ic = &sc->sc_ic;
	int			err;

	IPW2200_DBG(IPW2200_DBG_GLD, (sc->sc_dip, CE_CONT,
	    "ipw2200_m_unicst(): enter\n"));

	IPW2200_DBG(IPW2200_DBG_GLD, (sc->sc_dip, CE_CONT,
	    "ipw2200_m_unicst(): GLD setting MAC address to "
	    "%02x:%02x:%02x:%02x:%02x:%02x\n",
	    macaddr[0], macaddr[1], macaddr[2],
	    macaddr[3], macaddr[4], macaddr[5]));

	if (!IEEE80211_ADDR_EQ(ic->ic_macaddr, macaddr)) {

		IEEE80211_ADDR_COPY(ic->ic_macaddr, macaddr);

		if (sc->sc_flags & IPW2200_FLAG_RUNNING) {
			err = ipw2200_config(sc);
			if (err != DDI_SUCCESS) {
				IPW2200_WARN((sc->sc_dip, CE_WARN,
				    "ipw2200_m_unicst(): "
				    "device configuration failed\n"));
				goto fail;
			}
		}
	}
	return (DDI_SUCCESS);
fail:
	return (err);
}

static int
ipw2200_m_promisc(void *arg, boolean_t on)
{
	/* not supported */
	struct ipw2200_softc	*sc = (struct ipw2200_softc *)arg;

	IPW2200_DBG(IPW2200_DBG_GLD, (sc->sc_dip, CE_CONT,
	    "ipw2200_m_promisc(): enter. "
	    "GLD setting promiscuous mode - %d\n", on));

	return (DDI_SUCCESS);
}

static mblk_t *
ipw2200_m_tx(void *arg, mblk_t *mp)
{
	struct ipw2200_softc	*sc = (struct ipw2200_softc *)arg;
	struct ieee80211com	*ic = &sc->sc_ic;
	mblk_t			*next;

	/*
	 * No data frames go out unless we're associated; this
	 * should not happen as the 802.11 layer does not enable
	 * the xmit queue until we enter the RUN state.
	 */
	if (ic->ic_state != IEEE80211_S_RUN) {
		IPW2200_DBG(IPW2200_DBG_GLD, (sc->sc_dip, CE_CONT,
		    "ipw2200_m_tx(): discard msg, ic_state = %u\n",
		    ic->ic_state));
		freemsgchain(mp);
		return (NULL);
	}

	while (mp != NULL) {
		next = mp->b_next;
		mp->b_next = NULL;
		if (ipw2200_send(ic, mp, IEEE80211_FC0_TYPE_DATA) ==
		    DDI_FAILURE) {
			mp->b_next = next;
			break;
		}
		mp = next;
	}
	return (mp);
}

/* ARGSUSED */
static int
ipw2200_send(ieee80211com_t *ic, mblk_t *mp, uint8_t type)
{
	struct ipw2200_softc	*sc = (struct ipw2200_softc *)ic;
	struct ieee80211_node	*in;
	struct ieee80211_frame	*wh;
	mblk_t			*m0;
	size_t			cnt, off;
	struct ipw2200_tx_desc	*txdsc;
	struct dma_region	*dr;
	uint32_t		idx;
	int			err;
	/* tmp pointer, used to pack header and payload */
	uint8_t			*p;

	ASSERT(mp->b_next == NULL);

	IPW2200_DBG(IPW2200_DBG_GLD, (sc->sc_dip, CE_CONT,
	    "ipw2200_send(): enter\n"));

	m0 = NULL;
	err = DDI_SUCCESS;

	if ((type & IEEE80211_FC0_TYPE_MASK) != IEEE80211_FC0_TYPE_DATA) {
		/*
		 * skip all management frames since ipw2200 won't generate any
		 * management frames. Therefore, drop this package.
		 */
		freemsg(mp);
		err = DDI_SUCCESS;
		goto fail0;
	}

	mutex_enter(&sc->sc_tx_lock);

	/*
	 * need 1 empty descriptor
	 */
	if (sc->sc_tx_free <= IPW2200_TX_RING_MIN) {
		IPW2200_DBG(IPW2200_DBG_RING, (sc->sc_dip, CE_WARN,
		    "ipw2200_send(): no enough descriptors(%d)\n",
		    sc->sc_tx_free));
		ic->ic_stats.is_tx_nobuf++; /* no enough buffer */
		sc->sc_flags |= IPW2200_FLAG_TX_SCHED;
		err = DDI_FAILURE;
		goto fail1;
	}
	IPW2200_DBG(IPW2200_DBG_RING, (sc->sc_dip, CE_CONT,
	    "ipw2200_send():  tx-free=%d,tx-curr=%d\n",
	    sc->sc_tx_free, sc->sc_tx_cur));

	wh = (struct ieee80211_frame *)mp->b_rptr;
	in = ieee80211_find_txnode(ic, wh->i_addr1);
	if (in == NULL) { /* can not find the tx node, drop the package */
		ic->ic_stats.is_tx_failed++;
		freemsg(mp);
		err = DDI_SUCCESS;
		goto fail1;
	}
	in->in_inact = 0;
	(void) ieee80211_encap(ic, mp, in);
	ieee80211_free_node(in);

	/*
	 * get txdsc and wh
	 */
	idx	= sc->sc_tx_cur;
	txdsc	= &sc->sc_txdsc[idx];
	(void) memset(txdsc, 0, sizeof (*txdsc));
	wh	= (struct ieee80211_frame *)&txdsc->wh;

	/*
	 * extract header from message
	 */
	p	= (uint8_t *)&txdsc->wh;
	off	= 0;
	m0	= mp;
	while (off < sizeof (struct ieee80211_frame)) {
		cnt = MBLKL(m0);
		if (cnt > (sizeof (struct ieee80211_frame) - off))
			cnt = sizeof (struct ieee80211_frame) - off;
		if (cnt) {
			(void) memcpy(p + off, m0->b_rptr, cnt);
			off += cnt;
			m0->b_rptr += cnt;
		} else
			m0 = m0->b_cont;
	}

	/*
	 * extract payload from message
	 */
	dr	= &sc->sc_dma_txbufs[idx];
	p	= sc->sc_txbufs[idx];
	off	= 0;
	while (m0) {
		cnt = MBLKL(m0);
		if (cnt)
			(void) memcpy(p + off, m0->b_rptr, cnt);
		off += cnt;
		m0 = m0->b_cont;
	}

	txdsc->hdr.type   = IPW2200_HDR_TYPE_DATA;
	txdsc->hdr.flags  = IPW2200_HDR_FLAG_IRQ;
	txdsc->cmd	  = IPW2200_DATA_CMD_TX;
	txdsc->len	  = LE_16(off);
	txdsc->flags	  = 0;

	if (ic->ic_opmode == IEEE80211_M_IBSS) {
		if (!IEEE80211_IS_MULTICAST(wh->i_addr1))
			txdsc->flags |= IPW2200_DATA_FLAG_NEED_ACK;
	} else if (!IEEE80211_IS_MULTICAST(wh->i_addr3))
		txdsc->flags |= IPW2200_DATA_FLAG_NEED_ACK;

	if (ic->ic_flags & IEEE80211_F_PRIVACY) {
		wh->i_fc[1] |= IEEE80211_FC1_WEP;
		txdsc->wep_txkey = ic->ic_def_txkey;
	}
	else
		txdsc->flags |= IPW2200_DATA_FLAG_NO_WEP;

	if (ic->ic_flags & IEEE80211_F_SHPREAMBLE)
		txdsc->flags |= IPW2200_DATA_FLAG_SHPREAMBLE;

	txdsc->nseg	    = LE_32(1);
	txdsc->seg_addr[0]  = LE_32(dr->dr_pbase);
	txdsc->seg_len[0]   = LE_32(off);

	/*
	 * DMA sync: buffer and desc
	 */
	(void) ddi_dma_sync(dr->dr_hnd, 0,
	    IPW2200_TXBUF_SIZE, DDI_DMA_SYNC_FORDEV);
	(void) ddi_dma_sync(sc->sc_dma_txdsc.dr_hnd,
	    idx * sizeof (struct ipw2200_tx_desc),
	    sizeof (struct ipw2200_tx_desc), DDI_DMA_SYNC_FORDEV);

	sc->sc_tx_cur = RING_FORWARD(sc->sc_tx_cur, 1, IPW2200_TX_RING_SIZE);
	sc->sc_tx_free--;

	/*
	 * update txcur
	 */
	ipw2200_csr_put32(sc, IPW2200_CSR_TX1_WRITE_INDEX, sc->sc_tx_cur);

	/*
	 * success, free the original message
	 */
	if (mp)
		freemsg(mp);

fail1:
	mutex_exit(&sc->sc_tx_lock);
fail0:
	IPW2200_DBG(IPW2200_DBG_GLD, (sc->sc_dip, CE_CONT,
	    "ipw2200_send(): exit - err=%d\n", err));

	return (err);
}

/*
 * IOCTL handlers
 */
#define	IEEE80211_IOCTL_REQUIRED	(1)
#define	IEEE80211_IOCTL_NOT_REQUIRED	(0)
static void
ipw2200_m_ioctl(void *arg, queue_t *q, mblk_t *m)
{
	struct ipw2200_softc	*sc = (struct ipw2200_softc *)arg;
	struct ieee80211com	*ic = &sc->sc_ic;
	uint32_t		err;

	IPW2200_DBG(IPW2200_DBG_GLD, (sc->sc_dip, CE_CONT,
	    "ipw2200_m_ioctl(): enter\n"));

	/*
	 * Check whether or not need to handle this in net80211
	 *
	 */
	if (ipw2200_ioctl(sc, q, m) == IEEE80211_IOCTL_NOT_REQUIRED)
		return;

	err = ieee80211_ioctl(ic, q, m);
	if (err == ENETRESET) {
		if (sc->sc_flags & IPW2200_FLAG_RUNNING) {
			(void) ipw2200_m_start(sc);
			(void) ieee80211_new_state(ic,
			    IEEE80211_S_SCAN, -1);
		}
	}
	if (err == ERESTART) {
		if (sc->sc_flags & IPW2200_FLAG_RUNNING)
			(void) ipw2200_chip_reset(sc);
	}
}
static int
ipw2200_ioctl(struct ipw2200_softc *sc, queue_t *q, mblk_t *m)
{
	struct iocblk	*iocp;
	uint32_t	len, ret, cmd;
	mblk_t		*m0;
	boolean_t	need_privilege;
	boolean_t	need_net80211;

	if (MBLKL(m) < sizeof (struct iocblk)) {
		IPW2200_DBG(IPW2200_DBG_IOCTL, (sc->sc_dip, CE_CONT,
		    "ipw2200_ioctl(): ioctl buffer too short, %u\n",
		    MBLKL(m)));
		miocnak(q, m, 0, EINVAL);
		/*
		 * Buf not enough, do not need net80211 either
		 */
		return (IEEE80211_IOCTL_NOT_REQUIRED);
	}

	/*
	 * Validate the command
	 */
	iocp = (struct iocblk *)m->b_rptr;
	iocp->ioc_error = 0;
	cmd = iocp->ioc_cmd;
	need_privilege = B_TRUE;
	switch (cmd) {
	case WLAN_SET_PARAM:
	case WLAN_COMMAND:
		break;
	case WLAN_GET_PARAM:
		need_privilege = B_FALSE;
		break;
	default:
		IPW2200_DBG(IPW2200_DBG_IOCTL, (sc->sc_dip, CE_CONT,
		    "ipw2200_ioctl(): unknown cmd 0x%x", cmd));
		miocnak(q, m, 0, EINVAL);
		/*
		 * Unknown cmd, do not need net80211 either
		 */
		return (IEEE80211_IOCTL_NOT_REQUIRED);
	}

	if (need_privilege) {
		/*
		 * Check for specific net_config privilege on Solaris 10+.
		 * Otherwise just check for root access ...
		 */
		if (secpolicy_net_config != NULL)
			ret = secpolicy_net_config(iocp->ioc_cr, B_FALSE);
		else
			ret = drv_priv(iocp->ioc_cr);
		if (ret != 0) {
			miocnak(q, m, 0, ret);
			/*
			 * privilege check fail, do not need net80211 either
			 */
			return (IEEE80211_IOCTL_NOT_REQUIRED);
		}
	}
	/*
	 * sanity check
	 */
	m0 = m->b_cont;
	if (iocp->ioc_count == 0 || iocp->ioc_count < sizeof (wldp_t) ||
	    m0 == NULL) {
		miocnak(q, m, 0, EINVAL);
		/*
		 * invalid format, do not need net80211 either
		 */
		return (IEEE80211_IOCTL_NOT_REQUIRED);
	}
	/*
	 * assuming single data block
	 */
	if (m0->b_cont) {
		freemsg(m0->b_cont);
		m0->b_cont = NULL;
	}

	need_net80211 = B_FALSE;
	ret = ipw2200_getset(sc, m0, cmd, &need_net80211);
	if (!need_net80211) {
		len = msgdsize(m0);

		IPW2200_DBG(IPW2200_DBG_IOCTL, (sc->sc_dip, CE_CONT,
		    "ipw2200_ioctl(): go to call miocack with "
		    "ret = %d, len = %d\n", ret, len));
		miocack(q, m, len, ret);
		return (IEEE80211_IOCTL_NOT_REQUIRED);
	}

	/*
	 * IEEE80211_IOCTL - need net80211 handle
	 */
	return (IEEE80211_IOCTL_REQUIRED);
}

static int
ipw2200_getset(struct ipw2200_softc *sc, mblk_t *m, uint32_t cmd,
	boolean_t *need_net80211)
{
	wldp_t		*infp, *outfp;
	uint32_t	id;
	int		ret;

	infp = (wldp_t *)m->b_rptr;
	outfp = (wldp_t *)m->b_rptr;
	outfp->wldp_result = WL_NOTSUPPORTED;

	id = infp->wldp_id;
	IPW2200_DBG(IPW2200_DBG_IOCTL, (sc->sc_dip, CE_CONT,
	    "ipw2200_getset(): id = 0x%x\n", id));
	switch (id) {
	case WL_RADIO: /* which is not supported by net80211 */
		ret = iwi_wificfg_radio(sc, cmd, outfp);
		break;
	case WL_DESIRED_RATES: /* hardware doesn't support fix-rates */
		ret = iwi_wificfg_desrates(outfp);
		break;
	default:
		/*
		 * The wifi IOCTL net80211 supported:
		 *	case WL_ESSID:
		 *	case WL_BSSID:
		 *	case WL_WEP_KEY_TAB:
		 *	case WL_WEP_KEY_ID:
		 *	case WL_AUTH_MODE:
		 *	case WL_ENCRYPTION:
		 *	case WL_BSS_TYPE:
		 *	case WL_ESS_LIST:
		 *	case WL_LINKSTATUS:
		 *	case WL_RSSI:
		 *	case WL_SCAN:
		 *	case WL_LOAD_DEFAULTS:
		 *	case WL_DISASSOCIATE:
		 */
		*need_net80211 = B_TRUE; /* let net80211 do the rest */
		return (0);
	}
	/*
	 * we will overwrite everything
	 */
	m->b_wptr = m->b_rptr + outfp->wldp_length;
	return (ret);
}

static int
iwi_wificfg_radio(struct ipw2200_softc *sc, uint32_t cmd, wldp_t *outfp)
{
	uint32_t	ret = ENOTSUP;

	switch (cmd) {
	case WLAN_GET_PARAM:
		*(wl_linkstatus_t *)(outfp->wldp_buf) =
		    ipw2200_radio_status(sc);
		outfp->wldp_length = WIFI_BUF_OFFSET + sizeof (wl_linkstatus_t);
		outfp->wldp_result = WL_SUCCESS;
		ret = 0; /* command success */
		break;
	case WLAN_SET_PARAM:
	default:
		break;
	}
	return (ret);
}

static int
iwi_wificfg_desrates(wldp_t *outfp)
{
	/* return success, but with result NOTSUPPORTED */
	outfp->wldp_length = WIFI_BUF_OFFSET;
	outfp->wldp_result = WL_NOTSUPPORTED;
	return (0);
}
/* End of IOCTL Handlers */

void
ipw2200_fix_channel(struct ieee80211com *ic, mblk_t *m)
{
	struct ieee80211_frame	*wh;
	uint8_t			subtype;
	uint8_t			*frm, *efrm;

	wh = (struct ieee80211_frame *)m->b_rptr;

	if ((wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK) != IEEE80211_FC0_TYPE_MGT)
		return;

	subtype = wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK;

	if (subtype != IEEE80211_FC0_SUBTYPE_BEACON &&
	    subtype != IEEE80211_FC0_SUBTYPE_PROBE_RESP)
		return;

	/*
	 * assume the message contains only 1 block
	 */
	frm   = (uint8_t *)(wh + 1);
	efrm  = (uint8_t *)m->b_wptr;
	frm  += 12;  /* skip tstamp, bintval and capinfo fields */
	while (frm < efrm) {
		if (*frm == IEEE80211_ELEMID_DSPARMS)
#if IEEE80211_CHAN_MAX < 255
		if (frm[2] <= IEEE80211_CHAN_MAX)
#endif
			ic->ic_curchan = &ic->ic_sup_channels[frm[2]];
		frm += frm[1] + 2;
	}
}

static void
ipw2200_rcv_frame(struct ipw2200_softc *sc, struct ipw2200_frame *frame)
{
	struct ieee80211com	*ic = &sc->sc_ic;
	uint8_t			*data = (uint8_t *)frame;
	uint32_t		len;
	struct ieee80211_frame	*wh;
	struct ieee80211_node	*in;
	mblk_t			*m;
	int			i;

	len = LE_16(frame->len);

	/*
	 * Skip the frame header, get the real data from the input
	 */
	data += sizeof (struct ipw2200_frame);

	if ((len < sizeof (struct ieee80211_frame_min)) ||
	    (len > IPW2200_RXBUF_SIZE)) {
		IPW2200_DBG(IPW2200_DBG_RX, (sc->sc_dip, CE_CONT,
		    "ipw2200_rcv_frame(): bad frame length=%u\n",
		    LE_16(frame->len)));
		return;
	}

	IPW2200_DBG(IPW2200_DBG_RX, (sc->sc_dip, CE_CONT,
	    "ipw2200_rcv_frame(): chan = %d, length = %d\n", frame->chan, len));

	m = allocb(len, BPRI_MED);
	if (m) {
		(void) memcpy(m->b_wptr, data, len);
		m->b_wptr += len;

		wh = (struct ieee80211_frame *)m->b_rptr;
		if (wh->i_fc[1] & IEEE80211_FC1_WEP) {
			/*
			 * h/w decyption leaves the WEP bit, iv and CRC fields
			 */
			wh->i_fc[1] &= ~IEEE80211_FC1_WEP;
			for (i = sizeof (struct ieee80211_frame) - 1;
			    i >= 0; i--)
				*(m->b_rptr + IEEE80211_WEP_IVLEN +
				    IEEE80211_WEP_KIDLEN + i) =
				    *(m->b_rptr + i);
			m->b_rptr += IEEE80211_WEP_IVLEN + IEEE80211_WEP_KIDLEN;
			m->b_wptr -= IEEE80211_WEP_CRCLEN;
			wh = (struct ieee80211_frame *)m->b_rptr;
		}

		if (ic->ic_state == IEEE80211_S_SCAN) {
			ic->ic_ibss_chan = &ic->ic_sup_channels[frame->chan];
			ipw2200_fix_channel(ic, m);
		}

		in = ieee80211_find_rxnode(ic, wh);
		IPW2200_DBG(IPW2200_DBG_RX, (sc->sc_dip, CE_CONT,
		    "ipw2200_rcv_frame(): "
		    "ni_esslen:%d, ni_essid[0-5]:%c%c%c%c%c%c\n",
		    in->in_esslen,
		    in->in_essid[0], in->in_essid[1], in->in_essid[2],
		    in->in_essid[3], in->in_essid[4], in->in_essid[5]));

		(void) ieee80211_input(ic, m, in, frame->rssi_dbm, 0);

		ieee80211_free_node(in);
	}
	else
		IPW2200_WARN((sc->sc_dip, CE_WARN,
		    "ipw2200_rcv_frame(): "
		    "cannot allocate receive message(%u)\n",
		    LE_16(frame->len)));
}

static void
ipw2200_rcv_notif(struct ipw2200_softc *sc, struct ipw2200_notif *notif)
{
	struct ieee80211com			*ic = &sc->sc_ic;
	struct ipw2200_notif_association	*assoc;
	struct ipw2200_notif_authentication	*auth;
	uint8_t					*ndata = (uint8_t *)notif;

	IPW2200_DBG(IPW2200_DBG_NOTIF, (sc->sc_dip, CE_CONT,
	    "ipw2200_rcv_notif(): type=%u\n", notif->type));

	ndata += sizeof (struct ipw2200_notif);
	switch (notif->type) {
	case IPW2200_NOTIF_TYPE_ASSOCIATION:
		assoc = (struct ipw2200_notif_association *)ndata;

		IPW2200_DBG(IPW2200_DBG_WIFI, (sc->sc_dip, CE_CONT,
		    "ipw2200_rcv_notif(): association=%u,%u\n",
		    assoc->state, assoc->status));

		switch (assoc->state) {
		case IPW2200_ASSOC_SUCCESS:
			ieee80211_new_state(ic, IEEE80211_S_RUN, -1);
			break;
		case IPW2200_ASSOC_FAIL:
			ieee80211_begin_scan(ic, 1); /* reset */
			break;
		default:
			break;
		}
		break;

	case IPW2200_NOTIF_TYPE_AUTHENTICATION:
		auth = (struct ipw2200_notif_authentication *)ndata;

		IPW2200_DBG(IPW2200_DBG_WIFI, (sc->sc_dip, CE_CONT,
		    "ipw2200_rcv_notif(): authentication=%u\n", auth->state));

		switch (auth->state) {
		case IPW2200_AUTH_SUCCESS:
			ieee80211_new_state(ic, IEEE80211_S_ASSOC, -1);
			break;
		case IPW2200_AUTH_FAIL:
			break;
		default:
			IPW2200_DBG(IPW2200_DBG_NOTIF, (sc->sc_dip, CE_CONT,
			    "ipw2200_rcv_notif(): "
			    "unknown authentication state(%u)\n", auth->state));
			break;
		}
		break;

	case IPW2200_NOTIF_TYPE_SCAN_CHANNEL:
		IPW2200_DBG(IPW2200_DBG_SCAN, (sc->sc_dip, CE_CONT,
		    "ipw2200_rcv_notif(): scan-channel=%u\n",
		    ((struct ipw2200_notif_scan_channel *)ndata)->nchan));
		break;

	case IPW2200_NOTIF_TYPE_SCAN_COMPLETE:
		IPW2200_DBG(IPW2200_DBG_SCAN, (sc->sc_dip, CE_CONT,
		    "ipw2200_rcv_notif():scan-completed,(%u,%u)\n",
		    ((struct ipw2200_notif_scan_complete *)ndata)->nchan,
		    ((struct ipw2200_notif_scan_complete *)ndata)->status));

		/*
		 * scan complete
		 */
		sc->sc_flags &= ~IPW2200_FLAG_SCANNING;
		ieee80211_end_scan(ic);
		break;

	case IPW2200_NOTIF_TYPE_BEACON:
	case IPW2200_NOTIF_TYPE_CALIBRATION:
	case IPW2200_NOTIF_TYPE_NOISE:
		/*
		 * just ignore
		 */
		break;
	default:
		IPW2200_DBG(IPW2200_DBG_NOTIF, (sc->sc_dip, CE_CONT,
		    "ipw2200_rcv_notif(): unknown notification type(%u)\n",
		    notif->type));
		break;
	}
}

static uint_t
ipw2200_intr(caddr_t arg)
{
	struct ipw2200_softc	*sc = (struct ipw2200_softc *)arg;
	struct ieee80211com	*ic = &sc->sc_ic;
	uint32_t		ireg, ridx, len, i;
	uint8_t			*p, *rxbuf;
	struct dma_region	*dr;
	struct ipw2200_hdr	*hdr;
	int			need_sched;
	uint32_t		widx;

	ireg = ipw2200_csr_get32(sc, IPW2200_CSR_INTR);

	if (ireg == 0xffffffff)
		return (DDI_INTR_UNCLAIMED);

	if (!(ireg & IPW2200_INTR_MASK_ALL))
		return (DDI_INTR_UNCLAIMED);

	/*
	 * mask all interrupts
	 */
	ipw2200_csr_put32(sc, IPW2200_CSR_INTR_MASK, 0);

	/*
	 * acknowledge all fired interrupts
	 */
	ipw2200_csr_put32(sc, IPW2200_CSR_INTR, ireg);

	IPW2200_DBG(IPW2200_DBG_INT, (sc->sc_dip, CE_CONT,
	    "ipw2200_intr(): enter. interrupt fired, int=0x%08x\n", ireg));

	if (ireg & IPW2200_INTR_MASK_ERR) {

		IPW2200_DBG(IPW2200_DBG_FATAL, (sc->sc_dip, CE_CONT,
		    "ipw2200 interrupt(): int= 0x%08x\n", ireg));

		/*
		 * inform mfthread to recover hw error by stopping it
		 */
		mutex_enter(&sc->sc_mflock);
		sc->sc_flags |= IPW2200_FLAG_HW_ERR_RECOVER;
		mutex_exit(&sc->sc_mflock);

	} else {
		if (ireg & IPW2200_INTR_FW_INITED) {
			mutex_enter(&sc->sc_ilock);
			sc->sc_fw_ok = 1;
			cv_signal(&sc->sc_fw_cond);
			mutex_exit(&sc->sc_ilock);
		}
		if (ireg & IPW2200_INTR_RADIO_OFF) {
			IPW2200_REPORT((sc->sc_dip, CE_CONT,
			    "ipw2200_intr(): radio is OFF\n"));
			/*
			 * Stop hardware, will notify LINK is down
			 */
			ipw2200_stop(sc);
		}
		if (ireg & IPW2200_INTR_CMD_TRANSFER) {
			mutex_enter(&sc->sc_cmd_lock);
			ridx = ipw2200_csr_get32(sc,
			    IPW2200_CSR_CMD_READ_INDEX);
			i = RING_FORWARD(sc->sc_cmd_cur,
			sc->sc_cmd_free, IPW2200_CMD_RING_SIZE);
			len = RING_FLEN(i, ridx, IPW2200_CMD_RING_SIZE);

			IPW2200_DBG(IPW2200_DBG_INT, (sc->sc_dip, CE_CONT,
			    "ipw2200_intr(): cmd-ring,i=%u,ridx=%u,len=%u\n",
			    i, ridx, len));

			if (len > 0) {
				sc->sc_cmd_free += len;
				cv_signal(&sc->sc_cmd_cond);
			}
			for (; i != ridx;
			    i = RING_FORWARD(i, 1, IPW2200_CMD_RING_SIZE))
				sc->sc_done[i] = 1;
			mutex_exit(&sc->sc_cmd_lock);

			mutex_enter(&sc->sc_ilock);
			cv_signal(&sc->sc_cmd_status_cond);
			mutex_exit(&sc->sc_ilock);
		}
		if (ireg & IPW2200_INTR_RX_TRANSFER) {
			ridx = ipw2200_csr_get32(sc,
			    IPW2200_CSR_RX_READ_INDEX);
			widx = ipw2200_csr_get32(sc,
			    IPW2200_CSR_RX_WRITE_INDEX);

			IPW2200_DBG(IPW2200_DBG_INT, (sc->sc_dip, CE_CONT,
			    "ipw2200_intr(): rx-ring,widx=%u,ridx=%u\n",
			    ridx, widx));

			for (; sc->sc_rx_cur != ridx;
			    sc->sc_rx_cur = RING_FORWARD(sc->sc_rx_cur, 1,
			    IPW2200_RX_RING_SIZE)) {
				i	= sc->sc_rx_cur;
				rxbuf	= sc->sc_rxbufs[i];
				dr	= &sc->sc_dma_rxbufs[i];

				/*
				 * DMA sync
				 */
				(void) ddi_dma_sync(dr->dr_hnd, 0,
				    IPW2200_RXBUF_SIZE, DDI_DMA_SYNC_FORKERNEL);
				/*
				 * Get rx header(hdr) and rx data(p) from rxbuf
				 */
				p	= rxbuf;
				hdr	= (struct ipw2200_hdr *)p;
				p	+= sizeof (struct ipw2200_hdr);

				IPW2200_DBG(IPW2200_DBG_INT,
				    (sc->sc_dip, CE_CONT,
				    "ipw2200_intr(): Rx hdr type %u\n",
				    hdr->type));

				switch (hdr->type) {
				case IPW2200_HDR_TYPE_FRAME:
					ipw2200_rcv_frame(sc,
					    (struct ipw2200_frame *)p);
					break;

				case IPW2200_HDR_TYPE_NOTIF:
					ipw2200_rcv_notif(sc,
					    (struct ipw2200_notif *)p);
					break;
				default:
					IPW2200_DBG(IPW2200_DBG_INT,
					    (sc->sc_dip, CE_CONT,
					    "ipw2200_intr(): "
					    "unknown Rx hdr type %u\n",
					    hdr->type));
					break;
				}
			}
			/*
			 * write sc_rx_cur backward 1 step into RX_WRITE_INDEX
			 */
			ipw2200_csr_put32(sc, IPW2200_CSR_RX_WRITE_INDEX,
			    RING_BACKWARD(sc->sc_rx_cur, 1,
				IPW2200_RX_RING_SIZE));
		}
		if (ireg & IPW2200_INTR_TX1_TRANSFER) {
			mutex_enter(&sc->sc_tx_lock);
			ridx = ipw2200_csr_get32(sc,
			    IPW2200_CSR_TX1_READ_INDEX);
			len  = RING_FLEN(RING_FORWARD(sc->sc_tx_cur,
			    sc->sc_tx_free, IPW2200_TX_RING_SIZE),
			    ridx, IPW2200_TX_RING_SIZE);
			IPW2200_DBG(IPW2200_DBG_RING, (sc->sc_dip, CE_CONT,
			    "ipw2200_intr(): tx-ring,ridx=%u,len=%u\n",
			    ridx, len));
			sc->sc_tx_free += len;

			need_sched = 0;
			if ((sc->sc_tx_free > IPW2200_TX_RING_MIN) &&
			    (sc->sc_flags & IPW2200_FLAG_TX_SCHED)) {
				IPW2200_DBG(IPW2200_DBG_RING, (sc->sc_dip,
				    CE_CONT,
				    "ipw2200_intr(): Need Reschedule!"));
				need_sched = 1;
				sc->sc_flags &= ~IPW2200_FLAG_TX_SCHED;
			}
			mutex_exit(&sc->sc_tx_lock);

			if (need_sched)
				mac_tx_update(ic->ic_mach);
		}
	}

	/*
	 * enable all interrupts
	 */
	ipw2200_csr_put32(sc, IPW2200_CSR_INTR_MASK, IPW2200_INTR_MASK_ALL);

	return (DDI_INTR_CLAIMED);
}


/*
 *  Module Loading Data & Entry Points
 */
DDI_DEFINE_STREAM_OPS(ipw2200_devops, nulldev, nulldev, ipw2200_attach,
    ipw2200_detach, nodev, NULL, D_MP, NULL);

static struct modldrv ipw2200_modldrv = {
	&mod_driverops,
	ipw2200_ident,
	&ipw2200_devops
};

static struct modlinkage ipw2200_modlinkage = {
	MODREV_1,
	&ipw2200_modldrv,
	NULL
};

int
_init(void)
{
	int status;

	status = ddi_soft_state_init(&ipw2200_ssp,
	    sizeof (struct ipw2200_softc), 1);
	if (status != DDI_SUCCESS)
		return (status);

	mac_init_ops(&ipw2200_devops, IPW2200_DRV_NAME);
	status = mod_install(&ipw2200_modlinkage);
	if (status != DDI_SUCCESS) {
		mac_fini_ops(&ipw2200_devops);
		ddi_soft_state_fini(&ipw2200_ssp);
	}

	return (status);
}

int
_fini(void)
{
	int status;

	status = mod_remove(&ipw2200_modlinkage);
	if (status == DDI_SUCCESS) {
		mac_fini_ops(&ipw2200_devops);
		ddi_soft_state_fini(&ipw2200_ssp);
	}

	return (status);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&ipw2200_modlinkage, modinfop));
}