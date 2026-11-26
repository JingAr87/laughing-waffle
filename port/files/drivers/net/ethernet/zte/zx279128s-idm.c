// SPDX-License-Identifier: GPL-2.0-only
/*
 * ZTE zx279128s/r IDM (Integrated DMA) Wi-Fi <-> Switch/PPE offload engine
 *
 * Exposes the SoC's IDM DMA engine (0x921c8000, GIC_SPI 38) as virtual switch
 * ports bp=6 (idm0, 2.4 GHz phy0-ap*) and bp=7 (idm1, 5 GHz phy1-ap*).
 *
 * - Downstream (LAN/WAN -> Wi-Fi): sbrg (L2) or PPE (L3 NAT) DMAs frames into
 *   the IDM RX ring (4096 x 8B descriptors backed by Linux page_pool fragments)
 *   and zx_idm_poll() hands them straight to dev_queue_xmit(phyX-apY) with
 *   skb_mark_for_recycle(), bypassing GMAC, BMU, memcpy, bridge, and NAT.
 * - Upstream (Wi-Fi -> LAN/WAN): mt76_rx_complete() calls zx_idm_wifi_rx()
 *   pre-GRO; confirmed L2 LAN unicast and PPE-offloaded L3 IPv4 SNAT frames
 *   are submitted to the IDM TX ring with batched IDM_TX_DOORBELL writes.
 */

#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/if_bridge.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ip.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/rcupdate.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <net/ip.h>
#include <net/page_pool/helpers.h>

#include "zx279128s-eth.h"

static struct zx_eth_adapter __rcu *zx_idm_active_adapter;

static inline u8 zx_idm_mac_hash(const u8 *mac)
{
	return mac[0] ^ mac[1] ^ mac[2] ^ mac[3] ^ mac[4] ^ mac[5];
}

/* ---- Wi-Fi netdev registration (phy0-ap* -> idm0/bp=6, phy1-ap* -> idm1/bp=7) ---- */

static bool zx_idm_parse_wifi_name(const char *name, u8 *idm_port, u8 *ssid)
{
	unsigned int phy = 0, ap = 0;

	if (sscanf(name, "phy%u-ap%u", &phy, &ap) == 2 && ap < 8) {
		*idm_port = phy & 1;
		*ssid = ap;
		return true;
	}
	return false;
}

static void zx_idm_update_br_mac(struct zx_eth_adapter *adapter)
{
	int i;

	for (i = 0; i < ZX_NUM_PORTS; i++) {
		struct net_device *br;

		if (!adapter->ports[i])
			continue;
		rcu_read_lock();
		br = netdev_master_upper_dev_get_rcu(adapter->ports[i]);
		if (br) {
			ether_addr_copy(adapter->idm_br_mac, br->dev_addr);
			WRITE_ONCE(adapter->idm_br_mac_valid, true);
			rcu_read_unlock();
			return;
		}
		rcu_read_unlock();
	}
	WRITE_ONCE(adapter->idm_br_mac_valid, false);
}

void zx_idm_netdev_event(struct zx_eth_adapter *adapter, unsigned long event,
			 struct net_device *dev)
{
	u8 idm_port, ssid;
	unsigned long flags;
	int i, free_slot = -1;

	if (!adapter->idm_ready)
		return;

	if (event == NETDEV_CHANGEUPPER || event == NETDEV_CHANGEADDR)
		zx_idm_update_br_mac(adapter);

	if (!zx_idm_parse_wifi_name(dev->name, &idm_port, &ssid))
		return;

	spin_lock_irqsave(&adapter->idm_dev_lock, flags);
	for (i = 0; i < ZX_IDM_MAX_WIFI_DEVS; i++) {
		if (adapter->idm_wifi_devs[i] == dev) {
			if (event == NETDEV_UNREGISTER || event == NETDEV_DOWN) {
				WRITE_ONCE(adapter->idm_wifi_devs[i], NULL);
			} else {
				adapter->idm_wifi_port[i] = idm_port;
				adapter->idm_wifi_ssid[i] = ssid;
			}
			spin_unlock_irqrestore(&adapter->idm_dev_lock, flags);
			if (event == NETDEV_UNREGISTER || event == NETDEV_DOWN) {
				int b;

				for (b = 0; b < ZX_IDM_MAC_BUCKETS; b++)
					if (READ_ONCE(adapter->idm_sta_tab[b].dev) == dev)
						WRITE_ONCE(adapter->idm_sta_tab[b].valid, 0);
			}
			return;
		}
		if (!adapter->idm_wifi_devs[i] && free_slot < 0)
			free_slot = i;
	}

	if ((event == NETDEV_UP || event == NETDEV_CHANGEUPPER) &&
	    (dev->flags & IFF_UP) && free_slot >= 0) {
		adapter->idm_wifi_port[free_slot] = idm_port;
		adapter->idm_wifi_ssid[free_slot] = ssid;
		WRITE_ONCE(adapter->idm_wifi_devs[free_slot], dev);
		dev_info(adapter->dev, "IDM bound %s -> idm%u (bp=%u, ssid=%u)\n",
			 dev->name, idm_port, ZX_IDM_BRPORT(idm_port), ssid);
	}
	spin_unlock_irqrestore(&adapter->idm_dev_lock, flags);
}

bool zx_idm_is_wifi_dev(struct zx_eth_adapter *adapter,
			const struct net_device *dev)
{
	int i;

	if (!adapter->idm_ready || !dev)
		return false;
	for (i = 0; i < ZX_IDM_MAX_WIFI_DEVS; i++)
		if (READ_ONCE(adapter->idm_wifi_devs[i]) == dev)
			return true;
	return false;
}

bool zx_idm_dev_to_brport(struct zx_eth_adapter *adapter,
			  const struct net_device *dev, int *brport)
{
	int i;

	if (!adapter->idm_ready || !dev)
		return false;
	for (i = 0; i < ZX_IDM_MAX_WIFI_DEVS; i++) {
		if (READ_ONCE(adapter->idm_wifi_devs[i]) == dev) {
			*brport = ZX_IDM_BRPORT(adapter->idm_wifi_port[i]);
			return true;
		}
	}
	return false;
}

bool zx_idm_ifindex_is_wifi(struct zx_eth_adapter *adapter, int ifindex)
{
	int i;

	if (!adapter->idm_ready)
		return false;
	for (i = 0; i < ZX_IDM_MAX_WIFI_DEVS; i++) {
		struct net_device *d = READ_ONCE(adapter->idm_wifi_devs[i]);

		if (d && d->ifindex == ifindex)
			return true;
	}
	return false;
}

bool zx_idm_ifindex_is_bridge(struct zx_eth_adapter *adapter, int ifindex)
{
	int i;

	if (!adapter->idm_ready)
		return false;
	for (i = 0; i < ZX_NUM_PORTS; i++) {
		struct net_device *br;

		if (!adapter->ports[i])
			continue;
		rcu_read_lock();
		br = netdev_master_upper_dev_get_rcu(adapter->ports[i]);
		if (br && br->ifindex == ifindex) {
			rcu_read_unlock();
			return true;
		}
		rcu_read_unlock();
	}
	return false;
}

bool zx_idm_mac_to_brport(struct zx_eth_adapter *adapter,
			  const u8 *mac, int *brport)
{
	u8 h;

	if (!adapter->idm_ready)
		return false;
	h = zx_idm_mac_hash(mac);
	if (READ_ONCE(adapter->idm_sta_tab[h].valid)) {
		smp_rmb();
		if (ether_addr_equal(adapter->idm_sta_tab[h].mac, mac)) {
			struct net_device *d = READ_ONCE(adapter->idm_sta_tab[h].dev);

			if (d && zx_idm_dev_to_brport(adapter, d, brport))
				return true;
		}
	}
	return false;
}

struct net_device *zx_idm_get_bridge_dev(struct zx_eth_adapter *adapter,
					 int idm_port)
{
	int i;

	if (!adapter->idm_ready)
		return NULL;
	for (i = 0; i < ZX_IDM_MAX_WIFI_DEVS; i++) {
		struct net_device *d = READ_ONCE(adapter->idm_wifi_devs[i]);

		if (d && adapter->idm_wifi_port[i] == idm_port &&
		    netif_is_bridge_port(d))
			return d;
	}
	return NULL;
}

/* ---- Fast MAC learning / lookup helpers ---- */

void zx_idm_learn_lan_mac(struct zx_eth_adapter *adapter, const u8 *mac)
{
	u8 h;

	if (!adapter->idm_ready || is_multicast_ether_addr(mac))
		return;
	h = zx_idm_mac_hash(mac);
	if (READ_ONCE(adapter->idm_lan_tab[h].valid) &&
	    ether_addr_equal(adapter->idm_lan_tab[h].mac, mac))
		return;
	ether_addr_copy(adapter->idm_lan_tab[h].mac, mac);
	smp_wmb();
	WRITE_ONCE(adapter->idm_lan_tab[h].valid, 1);
}

static __maybe_unused bool zx_idm_is_lan_mac(struct zx_eth_adapter *adapter, const u8 *mac)
{
	u8 h = zx_idm_mac_hash(mac);

	if (!READ_ONCE(adapter->idm_lan_tab[h].valid))
		return false;
	smp_rmb();
	return ether_addr_equal(adapter->idm_lan_tab[h].mac, mac);
}

static void zx_idm_learn_sta_mac(struct zx_eth_adapter *adapter, const u8 *mac,
				 struct net_device *dev)
{
	u8 h = zx_idm_mac_hash(mac);

	if (READ_ONCE(adapter->idm_sta_tab[h].valid) &&
	    READ_ONCE(adapter->idm_sta_tab[h].dev) == dev &&
	    ether_addr_equal(adapter->idm_sta_tab[h].mac, mac))
		return;
	ether_addr_copy(adapter->idm_sta_tab[h].mac, mac);
	WRITE_ONCE(adapter->idm_sta_tab[h].dev, dev);
	smp_wmb();
	WRITE_ONCE(adapter->idm_sta_tab[h].valid, 1);
}

void zx_idm_forget_sta_mac(struct zx_eth_adapter *adapter, const u8 *mac)
{
	u8 h = zx_idm_mac_hash(mac);

	if (READ_ONCE(adapter->idm_sta_tab[h].valid) &&
	    ether_addr_equal(adapter->idm_sta_tab[h].mac, mac))
		WRITE_ONCE(adapter->idm_sta_tab[h].valid, 0);
}

static struct net_device *zx_idm_lookup_wifi_tx_dev(struct zx_eth_adapter *adapter,
						    int idm_port, u8 ssid,
						    bool ssid_vld, const u8 *dmac)
{
	u8 h = zx_idm_mac_hash(dmac);
	struct net_device *fallback = NULL;
	int i;

	/* 1. Exact STA MAC table (handles ssid_vld == 0 and multi-SSID) */
	if (READ_ONCE(adapter->idm_sta_tab[h].valid)) {
		smp_rmb();
		if (ether_addr_equal(adapter->idm_sta_tab[h].mac, dmac)) {
			struct net_device *d = READ_ONCE(adapter->idm_sta_tab[h].dev);

			if (d && (d->flags & IFF_UP))
				return d;
		}
	}

	/* 2. (idm_port, ssid) if hardware set ssid_vld, else default AP on idm_port */
	for (i = 0; i < ZX_IDM_MAX_WIFI_DEVS; i++) {
		struct net_device *d = READ_ONCE(adapter->idm_wifi_devs[i]);

		if (!d || !(d->flags & IFF_UP) ||
		    adapter->idm_wifi_port[i] != idm_port)
			continue;
		if (ssid_vld && adapter->idm_wifi_ssid[i] == ssid)
			return d;
		if (!fallback)
			fallback = d;
	}
	return fallback;
}

struct net_device *zx_idm_resolve_cpu_dev(struct zx_eth_adapter *adapter,
					  int idm_port, const u8 *frame)
{
	const struct ethhdr *eth = (const struct ethhdr *)frame;

	if (!adapter->idm_ready)
		return NULL;
	return zx_idm_lookup_wifi_tx_dev(adapter, idm_port, 0, false, eth->h_source);
}

/* ---- IDM TX-done reclaim & TX submit (Wi-Fi RX -> IDM TX) ---- */

static u32 zx_idm_tx_reclaim_locked(struct zx_eth_adapter *adapter,
				    struct sk_buff **free_list, u32 max_free)
{
	void __iomem *base = adapter->base;
	u32 done = readl_relaxed(base + ZX_IDM_TX_DONE_CNT) & 0xffff;
	u32 submitted = adapter->idm_tx_pending - adapter->idm_tx_unflushed;
	u32 n, i, freed = 0;

	if (!done || !submitted)
		return 0;

	n = min_t(u32, done, submitted);
	if (n > max_free)
		n = max_free;

	for (i = 0; i < n; i++) {
		u32 idx = adapter->idm_tx_cons & ZX_IDM_RING_MASK;
		struct sk_buff *skb = adapter->idm_tx_skbs[idx];

		if (likely(skb)) {
			dma_unmap_single(adapter->dev, adapter->idm_tx_dmas[idx],
					 adapter->idm_tx_lens[idx], DMA_TO_DEVICE);
			free_list[freed++] = skb;
			adapter->idm_tx_skbs[idx] = NULL;
		}
		adapter->idm_tx_cons++;
	}

	adapter->idm_tx_pending -= n;
	adapter->idm_tx_done += n;
	/* Acknowledge completed TX descriptors in [15:0] and readback so
	 * the other CPU cannot observe a stale TX_DONE_CNT and underflow it.
	 */
	writel(n & 0xffff, base + ZX_IDM_TX_DOORBELL);
	(void)readl_relaxed(base + ZX_IDM_TX_DONE_CNT);
	return freed;
}

static bool zx_idm_check_l3_ul(struct zx_eth_adapter *adapter,
			       struct sk_buff *skb)
{
	struct zx_ppe_tuple t;
	const struct iphdr *iph;
	unsigned int ihl;

	if (!READ_ONCE(adapter->ppe_active))
		return false;
	if (skb->protocol != htons(ETH_P_IP))
		return false;
	if (unlikely(!pskb_may_pull(skb, sizeof(struct iphdr))))
		return false;

	iph = (const struct iphdr *)skb->data;
	if (iph->version != 4 || iph->ihl != 5)
		return false;
	if (iph->frag_off & htons(IP_MF | IP_OFFSET))
		return false;

	ihl = iph->ihl * 4;
	if (iph->protocol == IPPROTO_TCP) {
		const struct tcphdr *th;

		if (unlikely(skb->len < ihl + sizeof(struct tcphdr)))
			return false;
		th = (const struct tcphdr *)(skb->data + ihl);
		/* Keep SYN/FIN/RST on the Linux stack for conntrack */
		if (th->syn || th->fin || th->rst)
			return false;
		t.sport = th->source;
		t.dport = th->dest;
	} else if (iph->protocol == IPPROTO_UDP) {
		const struct udphdr *uh;

		if (unlikely(skb->len < ihl + sizeof(struct udphdr)))
			return false;
		uh = (const struct udphdr *)(skb->data + ihl);
		t.sport = uh->source;
		t.dport = uh->dest;
	} else {
		return false;
	}

	t.proto = iph->protocol;
	t.saddr = iph->saddr;
	t.daddr = iph->daddr;

	return rhashtable_lookup_fast(&adapter->ppe_wifi_ul_flows, &t,
				      zx_ppe_ul_ht_params) != NULL;
}

bool zx_idm_wifi_rx(struct sk_buff *skb)
{
	struct zx_eth_adapter *adapter;
	const struct ethhdr *eth;
	u8 idm_port = 0, ssid = 0;
	bool found = false, is_l2 = false, is_l3 = false;
	dma_addr_t dma;
	unsigned int len;
	u32 idx;
	__le32 *desc;
	int i;

	rcu_read_lock();
	adapter = rcu_dereference(zx_idm_active_adapter);
	if (unlikely(!adapter)) {
		rcu_read_unlock();
		return false;
	}

	/* Fast linear lookup over registered Wi-Fi AP netdevs (typically 2) */
	for (i = 0; i < ZX_IDM_MAX_WIFI_DEVS; i++) {
		if (READ_ONCE(adapter->idm_wifi_devs[i]) == skb->dev) {
			idm_port = adapter->idm_wifi_port[i];
			ssid = adapter->idm_wifi_ssid[i];
			found = true;
			break;
		}
	}
	if (!found) {
		rcu_read_unlock();
		return false;
	}

	if (unlikely(skb_is_nonlinear(skb) ||
		     skb->data - skb_mac_header(skb) != ETH_HLEN ||
		     skb->len + ETH_HLEN > 1518)) {
		rcu_read_unlock();
		return false;
	}

	eth = eth_hdr(skb);
	zx_idm_learn_sta_mac(adapter, eth->h_source, skb->dev);

	/* Keep broadcast/multicast on normal Linux stack */
	if (unlikely(is_multicast_ether_addr(eth->h_dest))) {
		rcu_read_unlock();
		return false;
	}

	/* L3 PPE-offloaded IPv4 SNAT unicast (WAN <-> Wi-Fi).
	 * L2 LAN <-> Wi-Fi UL stays on the stall-free Linux bridge/GMAC path
	 * because sustained L2 IDM TX bursts into sbrg can stall after ~8 s.
	 */
	if (READ_ONCE(adapter->idm_br_mac_valid) &&
	    ether_addr_equal(eth->h_dest, adapter->idm_br_mac) &&
	    zx_idm_check_l3_ul(adapter, skb)) {
		is_l3 = true;
	} else {
		rcu_read_unlock();
		return false;
	}

	/* Restore 14B Ethernet header for IDM DMA */
	skb_push(skb, ETH_HLEN);
	if (unlikely(skb->len < 32)) {
		if (skb_put_padto(skb, 32)) {
			rcu_read_unlock();
			return true;
		}
	}
	len = skb->len;

	dma = dma_map_single(adapter->dev, skb->data, len, DMA_TO_DEVICE);
	if (unlikely(dma_mapping_error(adapter->dev, dma))) {
		skb_pull(skb, ETH_HLEN);
		rcu_read_unlock();
		return false;
	}

	{
		struct sk_buff *free_list[64];
		u32 freed = 0, fi;

		spin_lock_bh(&adapter->idm_tx_lock);
		if (adapter->idm_tx_pending >= 64)
			freed = zx_idm_tx_reclaim_locked(adapter, free_list,
							 ARRAY_SIZE(free_list));

		if (unlikely(adapter->idm_tx_pending >= ZX_IDM_RING_LEN - 16)) {
			adapter->idm_tx_full++;
			spin_unlock_bh(&adapter->idm_tx_lock);
			for (fi = 0; fi < freed; fi++)
				napi_consume_skb(free_list[fi], 64);
			dma_unmap_single(adapter->dev, dma, len, DMA_TO_DEVICE);
			skb_pull(skb, ETH_HLEN);
			rcu_read_unlock();
			return false;
		}

		idx = adapter->idm_tx_prod & ZX_IDM_RING_MASK;
		desc = adapter->idm_tx_ring + idx * 2;
		desc[0] = cpu_to_le32((u32)dma);
		desc[1] = cpu_to_le32((len & 0x3fff) |
				      ((u32)(ssid & 0x7) << 28) |
				      ((u32)(idm_port & 0x1) << 31));

		adapter->idm_tx_skbs[idx] = skb;
		adapter->idm_tx_dmas[idx] = dma;
		adapter->idm_tx_lens[idx] = len;
		adapter->idm_tx_prod++;
		adapter->idm_tx_pending++;
		adapter->idm_tx_unflushed++;

		if (is_l2)
			adapter->idm_tx_l2++;
		else
			adapter->idm_tx_l3++;

		if (adapter->idm_tx_unflushed >= 64) {
			dma_wmb();
			writel(adapter->idm_tx_unflushed << 16,
			       adapter->base + ZX_IDM_TX_DOORBELL);
			adapter->idm_tx_unflushed = 0;
		}
		spin_unlock_bh(&adapter->idm_tx_lock);
		for (fi = 0; fi < freed; fi++)
			napi_consume_skb(free_list[fi], 64);
	}
	rcu_read_unlock();
	return true;
}
EXPORT_SYMBOL_GPL(zx_idm_wifi_rx);

void zx_idm_wifi_rx_flush(void)
{
	struct zx_eth_adapter *adapter;

	rcu_read_lock();
	adapter = rcu_dereference(zx_idm_active_adapter);
	if (likely(adapter)) {
		spin_lock_bh(&adapter->idm_tx_lock);
		if (adapter->idm_tx_unflushed) {
			dma_wmb();
			writel(adapter->idm_tx_unflushed << 16,
			       adapter->base + ZX_IDM_TX_DOORBELL);
			adapter->idm_tx_unflushed = 0;
		}
		spin_unlock_bh(&adapter->idm_tx_lock);
	}
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(zx_idm_wifi_rx_flush);

/* ---- IDM RX refill & NAPI poll (Switch/PPE -> IDM RX -> Wi-Fi TX) ---- */

static int zx_idm_rx_refill_slot(struct zx_eth_adapter *adapter, u32 idx)
{
	struct page *page;
	u32 offset = 0;
	dma_addr_t dma;
	__le32 *desc;

	page = page_pool_dev_alloc_frag(adapter->idm_rx_pool, &offset,
					ZX_IDM_RX_FRAG_SIZE);
	if (unlikely(!page))
		return -ENOMEM;

	dma = page_pool_get_dma_addr(page) + offset + ZX_IDM_RX_HEADROOM;
	adapter->idm_rx_pages[idx] = page;
	adapter->idm_rx_offsets[idx] = offset;

	desc = adapter->idm_rx_ring + idx * 2;
	desc[0] = cpu_to_le32((u32)dma);
	desc[1] = 0;
	return 0;
}

static int zx_idm_poll(struct napi_struct *napi, int budget)
{
	struct zx_eth_adapter *adapter =
		container_of(napi, struct zx_eth_adapter, idm_napi);
	void __iomem *base = adapter->base;
	u32 avail, consumed = 0, refilled = 0;
	int work_done = 0;

	/* Reclaim any completed IDM TX descriptors (skip lock when idle) */
	if (READ_ONCE(adapter->idm_tx_pending)) {
		struct sk_buff *free_list[64];
		u32 freed, fi;

		spin_lock_bh(&adapter->idm_tx_lock);
		freed = zx_idm_tx_reclaim_locked(adapter, free_list,
						 ARRAY_SIZE(free_list));
		spin_unlock_bh(&adapter->idm_tx_lock);
		for (fi = 0; fi < freed; fi++)
			napi_consume_skb(free_list[fi], budget);
	}

	avail = readl_relaxed(base + ZX_IDM_RX_AVAIL_CNT) & 0xffff;
	while (avail && work_done < budget) {
		u32 idx = adapter->idm_rx_cons & ZX_IDM_RING_MASK;
		__le32 *desc = adapter->idm_rx_ring + idx * 2;
		struct page *page = adapter->idm_rx_pages[idx];
		u32 offset = adapter->idm_rx_offsets[idx];
		u32 w1, len;
		int idm_port;
		u8 ssid;
		bool ssid_vld;
		dma_addr_t dma;
		struct net_device *tx_dev;
		struct sk_buff *skb;
		u8 *data;

		dma_rmb();
		w1 = le32_to_cpu(desc[1]);
		len = w1 & 0x3fff;
		ssid = (w1 >> 16) & 0x7;
		ssid_vld = !!(w1 & BIT(19));
		idm_port = (w1 >> 31) & 0x1;

		adapter->idm_rx_pages[idx] = NULL;
		adapter->idm_rx_cons++;
		consumed++;
		avail--;
		work_done++;

		if (unlikely(!page || len < ETH_HLEN || len > 1536)) {
			if (page)
				page_pool_recycle_direct(adapter->idm_rx_pool, page);
			adapter->idm_rx_drop++;
			goto refill;
		}

		dma = page_pool_get_dma_addr(page) + offset + ZX_IDM_RX_HEADROOM;
		/* Sync L2/L3/L4 headers (<=128B); payload stays in DRAM for mt7915e PCIe DMA */
		dma_sync_single_for_cpu(adapter->dev, dma, min_t(u32, len, 128),
					DMA_FROM_DEVICE);
		data = page_address(page) + offset + ZX_IDM_RX_HEADROOM;

		/* Never duplicate BC/MC frames (Linux bridge handles them) */
		if (unlikely(is_multicast_ether_addr(data))) {
			page_pool_recycle_direct(adapter->idm_rx_pool, page);
			adapter->idm_rx_bcmc_drop++;
			goto refill;
		}

		tx_dev = zx_idm_lookup_wifi_tx_dev(adapter, idm_port, ssid,
						   ssid_vld, data);
		if (unlikely(!tx_dev)) {
			page_pool_recycle_direct(adapter->idm_rx_pool, page);
			adapter->idm_rx_nomap++;
			goto refill;
		}

		skb = napi_build_skb(page_address(page) + offset,
				     ZX_IDM_RX_FRAG_SIZE);
		if (unlikely(!skb)) {
			page_pool_recycle_direct(adapter->idm_rx_pool, page);
			adapter->idm_rx_drop++;
			goto refill;
		}

		skb_mark_for_recycle(skb);
		skb_reserve(skb, ZX_IDM_RX_HEADROOM);
		skb_put(skb, len);
		skb->dev = tx_dev;
		skb_reset_mac_header(skb);
		skb_set_network_header(skb, ETH_HLEN);
		skb->protocol = eth_hdr(skb)->h_proto;

		dev_queue_xmit(skb);
		adapter->idm_rx_pkts++;

refill:
		if (!zx_idm_rx_refill_slot(adapter, idx))
			refilled++;
	}

	if (consumed || refilled) {
		dma_wmb();
		writel((refilled << 16) | (consumed & 0xffff),
		       base + ZX_IDM_RX_DOORBELL);
	}

	if (work_done < budget && napi_complete_done(napi, work_done)) {
		/* Re-enable RX IRQ only (stock idm_net_open behavior; TX-done reclaimed inline) */
		writel_relaxed(ZX_IDM_INT_ALL & ~ZX_IDM_INT_RX,
			       base + ZX_IDM_INT_MASK);
	}

	return work_done;
}

static irqreturn_t zx_idm_isr(int irq, void *dev_id)
{
	struct zx_eth_adapter *adapter = dev_id;
	u32 status = readl_relaxed(adapter->base + ZX_IDM_INT_STATUS);

	if (!(status & ZX_IDM_INT_RX))
		return IRQ_NONE;

	adapter->idm_irq_cnt++;
	writel_relaxed(ZX_IDM_INT_ALL, adapter->base + ZX_IDM_INT_MASK);
	napi_schedule(&adapter->idm_napi);
	return IRQ_HANDLED;
}

/* ---- Hardware init / quiesce / resume ---- */

static int zx_idm_hw_init(struct zx_eth_adapter *adapter)
{
	void __iomem *base = adapter->base;
	u32 val;
	int i, refilled = 0;

	/* Mask all IDM IRQs */
	writel(ZX_IDM_INT_ALL, base + ZX_IDM_INT_MASK);

	/* Stock pon_npp_idm_init(): wifi_mac_len = 1600 + byteorder LE + ctrl[27:20]=0x02 */
	writel(1600, base + 0x3a0028);
	writel(0, base + ZX_IDM_DMA_CTRL2);
	val = readl(base + ZX_IDM_CTRL);
	val &= ~0x0fff0000;
	val |= 0x020f0000;
	writel(val, base + ZX_IDM_CTRL);

	memset(adapter->idm_ring_mem, 0, ZX_IDM_RING_ALLOC_SIZE);
	adapter->idm_rx_cons = 0;
	adapter->idm_tx_prod = 0;
	adapter->idm_tx_cons = 0;
	adapter->idm_tx_pending = 0;
	adapter->idm_tx_unflushed = 0;

	/* Program ring bases and size (4096 TX | 4096 RX) */
	writel((u32)adapter->idm_tx_ring_dma, base + ZX_IDM_TX_RING_BASE);
	writel((u32)adapter->idm_rx_ring_dma, base + ZX_IDM_RX_RING_BASE);
	writel((ZX_IDM_RING_LEN << 16) | ZX_IDM_RING_LEN,
	       base + ZX_IDM_RING_SIZE);

	/* Low-latency RX interrupt coalescing (32 pkts / ~100 us timer) */
	writel_relaxed(512, base + ZX_IDM_TX_INT_THRESH);
	writel_relaxed(32, base + ZX_IDM_RX_INT_THRESH);
	writel_relaxed(5000, base + ZX_IDM_INT_TIMER);

	/* Populate all 4096 RX descriptors */
	for (i = 0; i < ZX_IDM_RING_LEN; i++) {
		if (zx_idm_rx_refill_slot(adapter, i))
			break;
		refilled++;
	}

	dma_wmb();
	/* Stock rings doorbell in chunks or 1-by-1; submit in 512-desc batches */
	for (i = 0; i < refilled; i += 512) {
		u32 chunk = min_t(u32, 512, refilled - i);

		writel(chunk << 16, base + ZX_IDM_RX_DOORBELL);
	}

	dev_info(adapter->dev, "IDM HW initialized (rx_refilled=%d, irq=%d)\n",
		 refilled, adapter->idm_irq);
	return 0;
}

void zx_idm_quiesce(struct zx_eth_adapter *adapter)
{
	int i;

	if (!adapter->idm_ready)
		return;

	RCU_INIT_POINTER(zx_idm_active_adapter, NULL);
	synchronize_rcu();

	writel(ZX_IDM_INT_ALL, adapter->base + ZX_IDM_INT_MASK);
	napi_disable(&adapter->idm_napi);
	synchronize_irq(adapter->idm_irq);

	/* Free any in-flight TX SKBs and RX pages before HW reset */
	spin_lock_bh(&adapter->idm_tx_lock);
	for (i = 0; i < ZX_IDM_RING_LEN; i++) {
		if (adapter->idm_tx_skbs[i]) {
			dma_unmap_single(adapter->dev, adapter->idm_tx_dmas[i],
					 adapter->idm_tx_lens[i], DMA_TO_DEVICE);
			dev_kfree_skb_any(adapter->idm_tx_skbs[i]);
			adapter->idm_tx_skbs[i] = NULL;
		}
	}
	adapter->idm_tx_pending = 0;
	adapter->idm_tx_unflushed = 0;
	spin_unlock_bh(&adapter->idm_tx_lock);

	for (i = 0; i < ZX_IDM_RING_LEN; i++) {
		if (adapter->idm_rx_pages[i]) {
			page_pool_recycle_direct(adapter->idm_rx_pool,
						 adapter->idm_rx_pages[i]);
			adapter->idm_rx_pages[i] = NULL;
		}
	}
}

void zx_idm_resume(struct zx_eth_adapter *adapter)
{
	if (!adapter->idm_ready)
		return;

	zx_idm_hw_init(adapter);
	napi_enable(&adapter->idm_napi);
	writel_relaxed(ZX_IDM_INT_ALL & ~ZX_IDM_INT_RX,
		       adapter->base + ZX_IDM_INT_MASK);
	rcu_assign_pointer(zx_idm_active_adapter, adapter);
}

void zx_idm_init(struct zx_eth_adapter *adapter)
{
	struct device *dev = adapter->dev;
	struct page_pool_params pp_params = {
		.flags		= PP_FLAG_DMA_MAP | PP_FLAG_DMA_SYNC_DEV,
		.order		= 0,
		.pool_size	= ZX_IDM_RING_LEN / 2 + 256,
		.nid		= NUMA_NO_NODE,
		.dev		= dev,
		.dma_dir	= DMA_FROM_DEVICE,
		.max_len	= PAGE_SIZE,
		.offset		= 0,
	};
	struct net_device *napi_dev = NULL;
	int i, ret;

	if (adapter->idm_irq <= 0) {
		dev_info(dev, "IDM IRQ not specified in DT; IDM offload disabled\n");
		return;
	}

	spin_lock_init(&adapter->idm_tx_lock);
	spin_lock_init(&adapter->idm_dev_lock);

	for (i = 0; i < ZX_NUM_PORTS; i++) {
		if (adapter->ports[i]) {
			napi_dev = adapter->ports[i];
			break;
		}
	}
	if (!napi_dev)
		return;

	adapter->idm_ring_mem = dma_alloc_coherent(dev, ZX_IDM_RING_ALLOC_SIZE,
						   &adapter->idm_ring_dma,
						   GFP_KERNEL);
	if (!adapter->idm_ring_mem)
		return;

	adapter->idm_rx_ring = adapter->idm_ring_mem;
	adapter->idm_rx_ring_dma = adapter->idm_ring_dma;
	adapter->idm_tx_ring = adapter->idm_ring_mem + 0x8000;
	adapter->idm_tx_ring_dma = adapter->idm_ring_dma + 0x8000;

	adapter->idm_rx_pages = devm_kcalloc(dev, ZX_IDM_RING_LEN,
					     sizeof(*adapter->idm_rx_pages),
					     GFP_KERNEL);
	adapter->idm_rx_offsets = devm_kcalloc(dev, ZX_IDM_RING_LEN,
					       sizeof(*adapter->idm_rx_offsets),
					       GFP_KERNEL);
	adapter->idm_tx_skbs = devm_kcalloc(dev, ZX_IDM_RING_LEN,
					    sizeof(*adapter->idm_tx_skbs),
					    GFP_KERNEL);
	adapter->idm_tx_dmas = devm_kcalloc(dev, ZX_IDM_RING_LEN,
					    sizeof(*adapter->idm_tx_dmas),
					    GFP_KERNEL);
	adapter->idm_tx_lens = devm_kcalloc(dev, ZX_IDM_RING_LEN,
					    sizeof(*adapter->idm_tx_lens),
					    GFP_KERNEL);
	if (!adapter->idm_rx_pages || !adapter->idm_rx_offsets ||
	    !adapter->idm_tx_skbs || !adapter->idm_tx_dmas ||
	    !adapter->idm_tx_lens)
		goto err_ring;

	adapter->idm_rx_pool = page_pool_create(&pp_params);
	if (IS_ERR(adapter->idm_rx_pool)) {
		dev_err(dev, "IDM page_pool_create failed: %ld\n",
			PTR_ERR(adapter->idm_rx_pool));
		adapter->idm_rx_pool = NULL;
		goto err_ring;
	}

	netif_napi_add(napi_dev, &adapter->idm_napi, zx_idm_poll);

	zx_idm_hw_init(adapter);

	ret = devm_request_irq(dev, adapter->idm_irq, zx_idm_isr, 0,
			       "zx279128s-idm", adapter);
	if (ret) {
		dev_err(dev, "IDM request_irq(%d) failed: %d\n",
			adapter->idm_irq, ret);
		goto err_napi;
	}

	adapter->idm_ready = true;
	if (num_online_cpus() > 1)
		irq_set_affinity_hint(adapter->idm_irq, cpumask_of(1));
	napi_enable(&adapter->idm_napi);
	writel_relaxed(ZX_IDM_INT_ALL & ~ZX_IDM_INT_RX,
		       adapter->base + ZX_IDM_INT_MASK);
	rcu_assign_pointer(zx_idm_active_adapter, adapter);
	return;

err_napi:
	netif_napi_del(&adapter->idm_napi);
	page_pool_destroy(adapter->idm_rx_pool);
	adapter->idm_rx_pool = NULL;
err_ring:
	dma_free_coherent(dev, ZX_IDM_RING_ALLOC_SIZE, adapter->idm_ring_mem,
			  adapter->idm_ring_dma);
	adapter->idm_ring_mem = NULL;
}

void zx_idm_debugfs_show(struct zx_eth_adapter *adapter, struct seq_file *s)
{
	int i;

	if (!adapter->idm_ready) {
		seq_puts(s, "idm=disabled\n");
		return;
	}
	seq_printf(s, "idm_irq=%d idm_irqs=%u idm_int_st=%08x idm_int_msk=%08x\n",
		   adapter->idm_irq, adapter->idm_irq_cnt,
		   readl(adapter->base + ZX_IDM_INT_STATUS),
		   readl(adapter->base + ZX_IDM_INT_MASK));
	seq_printf(s, "idm_rx_pkts=%u idm_rx_drop=%u idm_rx_bcmc=%u idm_rx_nomap=%u idm_rx_avail=%u\n",
		   adapter->idm_rx_pkts, adapter->idm_rx_drop,
		   adapter->idm_rx_bcmc_drop, adapter->idm_rx_nomap,
		   readl(adapter->base + ZX_IDM_RX_AVAIL_CNT) & 0xffff);
	seq_printf(s, "idm_tx_l2=%u idm_tx_l3=%u idm_tx_done=%u idm_tx_pending=%u idm_tx_full=%u idm_hw_tx_done=%u idm_cpu_fb=%u\n",
		   adapter->idm_tx_l2, adapter->idm_tx_l3,
		   adapter->idm_tx_done, adapter->idm_tx_pending,
		   adapter->idm_tx_full,
		   readl(adapter->base + ZX_IDM_TX_DONE_CNT) & 0xffff,
		   adapter->idm_cpu_fallback);
	seq_puts(s, "idm_wifi_devs=");
	for (i = 0; i < ZX_IDM_MAX_WIFI_DEVS; i++) {
		struct net_device *d = READ_ONCE(adapter->idm_wifi_devs[i]);

		if (d)
			seq_printf(s, "%s(bp%u,s%u) ", d->name,
				   ZX_IDM_BRPORT(adapter->idm_wifi_port[i]),
				   adapter->idm_wifi_ssid[i]);
	}
	seq_puts(s, "\n");
}
