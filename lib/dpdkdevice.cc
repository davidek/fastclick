/*
 * dpdkdevice.{cc,hh} -- library for interfacing with Intel's DPDK
 *
 * Copyright (c) 2014-2015 Cyril Soldani, University of Liège
 * Copyright (c) 2015 Tom Barbette, University of Liège
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, subject to the conditions
 * listed in the Click LICENSE file. These conditions include: you must
 * preserve this copyright notice, and you cannot mention the copyright
 * holders in advertising related to the Software without their permission.
 * The Software is provided WITHOUT ANY WARRANTY, EXPRESS OR IMPLIED. This
 * notice is a summary of the Click LICENSE file; the license in that file is
 * legally binding.
 */

#include <click/config.h>
#include <click/dpdkdevice.hh>

CLICK_DECLS

/* Wraps rte_eth_dev_socket_id(), which may return -1 for valid ports when NUMA
 * is not well supported. This function will return 0 instead in that case. */
int DPDKDevice::get_port_numa_node(unsigned port_id)
{
    if (port_id >= rte_eth_dev_count())
        return -1;
    int numa_node = rte_eth_dev_socket_id(port_id);
    return (numa_node == -1) ? 0 : numa_node;
}

unsigned int DPDKDevice::get_nb_txdesc(unsigned port_id)
{
    DevInfo *info = _devs.findp(port_id);
    if (!info)
        return 0;

    return info->n_tx_descs;
}

void DPDKDevice::add_pool(const struct rte_mempool * rte, void *arg){
	int* i = (int*)arg;
	if (*i < _nr_pktmbuf_pools)
		_pktmbuf_pools[*i] = const_cast<struct rte_mempool *>(rte);
	(*i)++;
}

bool DPDKDevice::alloc_pktmbufs()
{
    // Count NUMA sockets
    int max_socket = -1;
    for (HashMap<unsigned, DevInfo>::const_iterator it = _devs.begin();
         it != _devs.end(); ++it) {
        int numa_node = DPDKDevice::get_port_numa_node(it.key());
        if (numa_node > max_socket)
            max_socket = numa_node;
    }

    if (max_socket == -1)
        return false;

    _nr_pktmbuf_pools = max_socket + 1;

    // Allocate pktmbuf_pool array
    typedef struct rte_mempool *rte_mempool_p;
    _pktmbuf_pools = new rte_mempool_p[_nr_pktmbuf_pools];
    if (!_pktmbuf_pools)
        return false;
    memset(_pktmbuf_pools, 0, _nr_pktmbuf_pools * sizeof(rte_mempool_p));

    if (rte_eal_process_type() == RTE_PROC_PRIMARY) {
		// Create a pktmbuf pool for each active socket
		for (int i = 0; i < _nr_pktmbuf_pools; i++) {
			if (!_pktmbuf_pools[i]) {
				char name[64];
				snprintf(name, 64, "mbuf_pool_%u", i);
				_pktmbuf_pools[i] =
					rte_mempool_create(
						name, NB_MBUF, MBUF_SIZE, MBUF_CACHE_SIZE,
						sizeof (struct rte_pktmbuf_pool_private),
						rte_pktmbuf_pool_init, NULL, rte_pktmbuf_init, NULL,
						i, 0);
				if (!_pktmbuf_pools[i])
					return false;
			}
		}
    } else {
		int i = 0;
		rte_mempool_walk(add_pool,(void*)&i);
    }

    return true;
}

struct rte_mempool *DPDKDevice::get_mpool(unsigned int socket_id) {
    return _pktmbuf_pools[socket_id];
}

int DPDKDevice::initialize_device(unsigned port_id, DevInfo &info,
                                  ErrorHandler *errh)
{
    struct rte_eth_conf dev_conf;
    struct rte_eth_dev_info dev_info;
    memset(&dev_conf, 0, sizeof dev_conf);

    rte_eth_dev_info_get(port_id, &dev_info);

    dev_conf.rxmode.mq_mode = ETH_MQ_RX_RSS;
    dev_conf.rx_adv_conf.rss_conf.rss_key = NULL;
    dev_conf.rx_adv_conf.rss_conf.rss_hf = ETH_RSS_IP;

    info.n_rx_queues = (info.n_rx_queues>0?info.n_rx_queues:1);
    info.n_tx_queues = (info.n_tx_queues>0?info.n_tx_queues:1);

    if (rte_eth_dev_configure(port_id, info.n_rx_queues,
                              info.n_tx_queues,
                              &dev_conf) < 0)
        return errh->error(
            "Cannot initialize DPDK port %u with %u RX and %u TX queues",
            port_id, info.n_rx_queues, info.n_tx_queues);
    struct rte_eth_rxconf rx_conf;
#if RTE_VER_MAJOR >= 2
    memcpy(&rx_conf, &dev_info.default_rxconf, sizeof rx_conf);
#else
    bzero(&rx_conf,sizeof rx_conf);
#endif
    rx_conf.rx_thresh.pthresh = RX_PTHRESH;
    rx_conf.rx_thresh.hthresh = RX_HTHRESH;
    rx_conf.rx_thresh.wthresh = RX_WTHRESH;

    struct rte_eth_txconf tx_conf;
#if RTE_VER_MAJOR >= 2
    memcpy(&tx_conf, &dev_info.default_txconf, sizeof tx_conf);
#else
    bzero(&tx_conf,sizeof tx_conf);
#endif
    tx_conf.tx_thresh.pthresh = TX_PTHRESH;
    tx_conf.tx_thresh.hthresh = TX_HTHRESH;
    tx_conf.tx_thresh.wthresh = TX_WTHRESH;
    tx_conf.txq_flags |= ETH_TXQ_FLAGS_NOMULTSEGS | ETH_TXQ_FLAGS_NOOFFLOADS;

    int numa_node = DPDKDevice::get_port_numa_node(port_id);
    for (unsigned i = 0; i < info.n_rx_queues; ++i)
        if (rte_eth_rx_queue_setup(port_id, i,
                (info.n_rx_descs>0?info.n_rx_descs:256), numa_node, &rx_conf,
                _pktmbuf_pools[numa_node]) != 0)
            return errh->error(
                "Cannot initialize RX queue %u of port %u on node %u",
                i, port_id, numa_node);

    for (unsigned i = 0; i < info.n_tx_queues; ++i)
        if (rte_eth_tx_queue_setup(port_id, i,
                (info.n_tx_descs>0?info.n_tx_descs:1024), numa_node, &tx_conf)
            != 0)
            return errh->error(
                "Cannot initialize TX queue %u of port %u on node %u",
                i, port_id, numa_node);

    int err = rte_eth_dev_start(port_id);
    if (err < 0)
        return errh->error(
            "Cannot start DPDK port %u: error %d", port_id, err);

    if (info.promisc)
        rte_eth_promiscuous_enable(port_id);

    return 0;
}

int DPDKDevice::add_device(unsigned port_id, DPDKDevice::Dir dir,
                           int &queue_id, bool promisc, unsigned n_desc,
                           ErrorHandler *errh)
{
    if (_is_initialized)
        return errh->error(
            "Trying to configure DPDK device after initialization");

    DevInfo *info = _devs.findp(port_id);
    if (!info) {
        DevInfo info(dir, (queue_id < 0) ? 0 : queue_id, promisc, n_desc);
        _devs.insert(port_id, info);
    } else {
        if (dir == RX) {
            if (info->n_rx_queues > 0 && promisc != info->promisc)
                return errh->error(
                    "Some elements disagree on whether or not device %u should"
                    " be in promiscuous mode", port_id);
            info->promisc |= promisc;
            if (n_desc != 0) {
                if (n_desc != info->n_rx_descs && info->n_rx_descs != 0)
                    return errh->error(
                        "Some elements disagree on the number of RX descriptors "
                        "for device %u (want %d, actual is %d)", port_id, n_desc,
                        info->n_rx_descs);
                info->n_rx_descs = n_desc;
            }
            info->n_rx_queues =
                1 + ((queue_id <= 0) ? info->n_rx_queues : queue_id);
        } else {
            if (n_desc != 0) {
                if (n_desc != info->n_tx_descs && info->n_tx_descs != 0)
                    return errh->error(
                        "Some elements disagree on the number of TX descriptors "
                        "for device %u (want %d, actual is %d)", port_id, n_desc,
                        info->n_tx_descs);
                info->n_tx_descs = n_desc;
            }
            info->n_tx_queues =
                1 + ((queue_id <= 0) ? info->n_tx_queues : queue_id);
        }
    }

    return 0;
}

int DPDKDevice::add_rx_device(unsigned port_id, int &queue_id, bool promisc,
                              unsigned n_desc, ErrorHandler *errh)
{
    return add_device(
        port_id, DPDKDevice::RX, queue_id, promisc, n_desc, errh);
}

int DPDKDevice::add_tx_device(unsigned port_id, int &queue_id, unsigned n_desc,
                              ErrorHandler *errh)
{
    return add_device(port_id, DPDKDevice::TX, queue_id, false, n_desc, errh);
}

int DPDKDevice::initialize(ErrorHandler *errh)
{
    if (_is_initialized)
        return 0;

    click_chatter("Initializing DPDK");
#if RTE_VER_MAJOR < 2
    if (rte_eal_pci_probe())
        return errh->error("Cannot probe the PCI bus");
#endif

    const unsigned n_ports = rte_eth_dev_count();
    if (n_ports == 0)
        return errh->error("No DPDK-enabled ethernet port found");

    for (HashMap<unsigned, DevInfo>::const_iterator it = _devs.begin();
         it != _devs.end(); ++it)
        if (it.key() >= n_ports)
            return errh->error("Cannot find DPDK port %u", it.key());

    if (!alloc_pktmbufs())
        return errh->error("Could not allocate packet MBuf pools");

    for (HashMap<unsigned, DevInfo>::iterator it = _devs.begin();
         it != _devs.end(); ++it) {
        int ret = initialize_device(it.key(), it.value(), errh);
        if (ret < 0)
            return ret;
    }

    _is_initialized = true;
    return 0;
}

void DPDKDevice::free_pkt(unsigned char *, size_t, void *pktmbuf)
{
    rte_pktmbuf_free((struct rte_mbuf *) pktmbuf);
}

int DPDKDevice::NB_MBUF = 65536*8;
int DPDKDevice::DATA_SIZE = 2048;
int DPDKDevice::MBUF_SIZE =
	DPDKDevice::DATA_SIZE + sizeof (struct rte_mbuf) + RTE_PKTMBUF_HEADROOM;
int DPDKDevice::MBUF_CACHE_SIZE = 256;
int DPDKDevice::RX_PTHRESH = 8;
int DPDKDevice::RX_HTHRESH = 8;
int DPDKDevice::RX_WTHRESH = 4;
int DPDKDevice::TX_PTHRESH = 36;
int DPDKDevice::TX_HTHRESH = 0;
int DPDKDevice::TX_WTHRESH = 0;

bool DPDKDevice::_is_initialized = false;
HashMap<unsigned, DPDKDevice::DevInfo> DPDKDevice::_devs;
struct rte_mempool** DPDKDevice::_pktmbuf_pools;
int DPDKDevice::_nr_pktmbuf_pools;

CLICK_ENDDECLS
