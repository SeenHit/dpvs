/*
 * Soft:        Keepalived is a failover program for the LVS project
 *              <www.linuxvirtualserver.org>. It monitor & manipulate
 *              a loadbalanced server pool using multi-layer checks.
 *
 * Part:        Configuration file parser/reader. Place into the dynamic
 *              data structure representation the conf file representing
 *              the loadbalanced server pool.
 *
 * Author:      Alexandre Cassen, <acassen@linux-vs.org>
 *
 *              This program is distributed in the hope that it will be useful,
 *              but WITHOUT ANY WARRANTY; without even the implied warranty of
 *              MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *              See the GNU General Public License for more details.
 *
 *              This program is free software; you can redistribute it and/or
 *              modify it under the terms of the GNU General Public License
 *              as published by the Free Software Foundation; either version
 *              2 of the License, or (at your option) any later version.
 *
 * Copyright (C) 2001-2017 Alexandre Cassen, <acassen@gmail.com>
 */

#include "config.h"

#include <errno.h>
#include <stdint.h>
#include <arpa/inet.h>

#include "check_parser.h"
#include "check_data.h"
#include "check_api.h"
#include "global_data.h"
#include "global_parser.h"
#include "main.h"
#include "logger.h"
#include "parser.h"
#include "utils.h"
#include "ipwrapper.h"
#if defined _WITH_VRRP_
#include "vrrp_parser.h"
#endif
#if defined _WITH_BFD_
#include "bfd_parser.h"
#endif
#include "libipvs.h"

#define ESTABLISH_TIMEOUT_MAX 3600
#define ESTABLISH_TIMEOUT_MIN 1

/* List of valid schedulers */
static const char *lvs_schedulers[] =
	{"rr", "wrr", "lc", "wlc", "lblc", "sh", "mh", "dh", "fo", "ovf", "lblcr", "sed", "nq", "conhash", NULL};

/* SSL handlers */
static void
ssl_handler(const vector_t *strvec)
{
	if (!strvec)
		return;

	if (check_data->ssl) {
		free_ssl();
		report_config_error(CONFIG_GENERAL_ERROR, "SSL context already specified - replacing");
	}
	check_data->ssl = alloc_ssl();
}
static void
sslpass_handler(const vector_t *strvec)
{
	if (vector_size(strvec) < 2) {
		report_config_error(CONFIG_GENERAL_ERROR, "SSL password missing");
		return;
	}

	if (check_data->ssl->password) {
		report_config_error(CONFIG_GENERAL_ERROR, "SSL password already specified - replacing");
		FREE_CONST(check_data->ssl->password);
	}
	check_data->ssl->password = set_value(strvec);
}
static void
sslca_handler(const vector_t *strvec)
{
	if (vector_size(strvec) < 2) {
		report_config_error(CONFIG_GENERAL_ERROR, "SSL cafile missing");
		return;
	}

	if (check_data->ssl->cafile) {
		report_config_error(CONFIG_GENERAL_ERROR, "SSL cafile already specified - replacing");
		FREE_CONST(check_data->ssl->cafile);
	}
	check_data->ssl->cafile = set_value(strvec);
}
static void
sslcert_handler(const vector_t *strvec)
{
	if (vector_size(strvec) < 2) {
		report_config_error(CONFIG_GENERAL_ERROR, "SSL certfile missing");
		return;
	}

	if (check_data->ssl->certfile) {
		report_config_error(CONFIG_GENERAL_ERROR, "SSL certfile already specified - replacing");
		FREE_CONST(check_data->ssl->certfile);
	}
	check_data->ssl->certfile = set_value(strvec);
}
static void
sslkey_handler(const vector_t *strvec)
{
	if (vector_size(strvec) < 2) {
		report_config_error(CONFIG_GENERAL_ERROR, "SSL keyfile missing");
		return;
	}

	if (check_data->ssl->keyfile) {
		report_config_error(CONFIG_GENERAL_ERROR, "SSL keyfile already specified - replacing");
		FREE_CONST(check_data->ssl->keyfile);
	}
	check_data->ssl->keyfile = set_value(strvec);
}

/* Virtual Servers handlers */
static void
vsg_handler(const vector_t *strvec)
{
	virtual_server_group_t *vsg;

	if (!strvec)
		return;

	/* Fetch queued vsg */
	alloc_vsg(strvec_slot(strvec, 1));
	alloc_value_block(alloc_vsg_entry, strvec_slot(strvec, 0));

	/* Ensure the virtual server group has some configuration */
	vsg = LIST_TAIL_DATA(check_data->vs_group);
	if (LIST_ISEMPTY(vsg->vfwmark) && LIST_ISEMPTY(vsg->addr_range)) {
		report_config_error(CONFIG_GENERAL_ERROR, "virtual server group %s has no entries - removing", vsg->gname);
		free_list_element(check_data->vs_group, check_data->vs_group->tail);
	}
}

static void
laddr_group_handler(const vector_t *strvec)
{
	if (!strvec)
		return;
	alloc_laddr_group(vector_slot(strvec, 1));
	alloc_value_block(alloc_laddr_entry, strvec_slot(strvec, 0));
}

static void
vs_handler(const vector_t *strvec)
{
	global_data->have_checker_config = true;

	/* If we are not in the checker process, we don't want any more info */
	if (!strvec)
		return;

	alloc_vs(strvec_slot(strvec, 1), vector_size(strvec) >= 3 ? strvec_slot(strvec, 2) : NULL);
}
static void
vs_end_handler(void)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	real_server_t *rs;
	element e;
	uint16_t af = AF_UNSPEC;
	bool mixed_af;


	if (vs->af == AF_UNSPEC) {
		/* This only occurs if:
		 *
		 * 1. the virtual server uses a fwmark(not supported by DPVS), all the
		 * real/sorry servers are tunnelled, and the address family has not
		 * been specified.
		 *
		 * Maintain backward compatibility. Prior to the commit following 17fa4a3c
		 * the address family of the virtual server was set from any of its
		 * real or sorry servers, even if they were tunnelled. However, all the real
		 * and sorry servers had to be the same address family, even if tunnelled,
		 * so only set the address family from the tunnelled real/sorry servers
		 * if all the real/sorry servers are of the same address family.
		 *
		 * 2. the virtual server is configured with virtual_server_group and no "ip_family"
		 * is specified explicitly within it.
		 *
		 * Keep the vs->af to be AF_UNSPEC unchanged, and vs->af would be assgined with
		 * vsg->af in link_vsg_to_vs later.
		 *
		 * */
		mixed_af = false;

		if (vs->s_svr)
			af = vs->s_svr->addr.ss_family;

		LIST_FOREACH(vs->rs, rs, e) {
			if (af == AF_UNSPEC)
				af = rs->addr.ss_family;
			else if (af != rs->addr.ss_family) {
				mixed_af = true;
				break;
			}
		}

		if (mixed_af) {
			/* We have a mixture of IPv4 and IPv6 tunnelled real/sorry servers.
			 * Default to IPv4.*/
			report_config_error(CONFIG_GENERAL_ERROR, "Address family of real/sorry servers are"
					"not the same for vs %s.", FMT_VS(vs));
		}
	}
}
static void
ip_family_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	uint16_t af;

	if (!strcmp(strvec_slot(strvec, 1), "inet"))
		af = AF_INET;
	else if (!strcmp(strvec_slot(strvec, 1), "inet6")) {
#ifndef LIBIPVS_USE_NL
		report_config_error(CONFIG_GENERAL_ERROR, "IPVS with IPv6 is not supported by this build");
		skip_block(false);
		return;
#endif
		af = AF_INET6;
	}
	else {
		report_config_error(CONFIG_GENERAL_ERROR, "unknown address family %s", strvec_slot(strvec, 1));
		return;
	}

	if (vs->af != AF_UNSPEC &&
	    af != vs->af) {
		report_config_error(CONFIG_GENERAL_ERROR, "Virtual server specified family %s conflicts with server family", strvec_slot(strvec, 1));
		return;
	}

	vs->af = af;
}
static void
vs_co_timeout_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	unsigned long timer;

	if (!read_timer(strvec, 1, &timer, 1, UINT_MAX, true)) {
		report_config_error(CONFIG_GENERAL_ERROR, "virtual server connect_timeout %s invalid - ignoring", strvec_slot(strvec, 1));
		return;
	}
	vs->connection_to = timer;
}
static void
vs_delay_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	unsigned long delay;

	if (read_timer(strvec, 1, &delay, 1, 0, true))
		vs->delay_loop = delay;
	else
		report_config_error(CONFIG_GENERAL_ERROR, "virtual server delay loop '%s' invalid - ignoring", strvec_slot(strvec, 1));
}
static void
vs_delay_before_retry_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	unsigned long delay;

	if (read_timer(strvec, 1, &delay, 0, 0, true))
		vs->delay_before_retry = delay;
	else
		report_config_error(CONFIG_GENERAL_ERROR, "virtual server delay before retry '%s' invalid - ignoring", strvec_slot(strvec, 1));
}
static void
vs_retry_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	unsigned retry;

	if (!read_unsigned_strvec(strvec, 1, &retry, 1, UINT32_MAX, false)) {
		report_config_error(CONFIG_GENERAL_ERROR, "retry value invalid - %s", strvec_slot(strvec, 1));
		return;
	}
	vs->retry = retry;
}
static void
vs_warmup_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	unsigned long delay;

	if (read_timer(strvec, 1, &delay, 0, 0, true))
		vs->warmup = delay;
	else
		report_config_error(CONFIG_GENERAL_ERROR, "virtual server warmup '%s' invalid - ignoring", strvec_slot(strvec, 1));
}
static void
lbalgo_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	const char *str = strvec_slot(strvec, 1);
	int i;

	/* Check valid scheduler name */
	for (i = 0; lvs_schedulers[i] && strcmp(str, lvs_schedulers[i]); i++);

	if (!lvs_schedulers[i] || strlen(str) >= sizeof(vs->sched)) {
		report_config_error(CONFIG_GENERAL_ERROR, "Invalid lvs_scheduler '%s' - ignoring", strvec_slot(strvec, 1));
		return;
	}

	strcpy(vs->sched, str);
}

static void
lbflags_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	const char *str = strvec_slot(strvec, 0);

	if (!strcmp(str, "hashed"))
		vs->flags |= IP_VS_SVC_F_HASHED;
#ifdef IP_VS_SVC_F_ONEPACKET
	else if (!strcmp(str, "ops"))
		vs->flags |= IP_VS_SVC_F_ONEPACKET;
#endif
#ifdef IP_VS_SVC_F_SCHED1		/* From Linux 3.11 */
	else if (!strcmp(str, "flag-1"))
		vs->flags |= IP_VS_SVC_F_SCHED1;
	else if (!strcmp(str, "flag-2"))
		vs->flags |= IP_VS_SVC_F_SCHED2;
	else if (!strcmp(str, "flag-3"))
		vs->flags |= IP_VS_SVC_F_SCHED3;
	else if (!strcmp(vs->sched , "sh") )
	{
		/* sh-port and sh-fallback flags are relevant for sh scheduler only */
		if (!strcmp(str, "sh-port")  )
			vs->flags |= IP_VS_SVC_F_SCHED_SH_PORT;
		if (!strcmp(str, "sh-fallback"))
			vs->flags |= IP_VS_SVC_F_SCHED_SH_FALLBACK;
	}
	else if (!strcmp(vs->sched , "mh") )
	{
		/* mh-port and mh-fallback flags are relevant for mh scheduler only */
		if (!strcmp(str, "mh-port")  )
			vs->flags |= IP_VS_SVC_F_SCHED_MH_PORT;
		if (!strcmp(str, "mh-fallback"))
			vs->flags |= IP_VS_SVC_F_SCHED_MH_FALLBACK;
	}
	else
		report_config_error(CONFIG_GENERAL_ERROR, "%s only applies to sh scheduler - ignoring", str);
#endif
}

static void
svr_forwarding_handler(real_server_t *rs, const vector_t *strvec, const char *s_type)
{
	const char *str = strvec_slot(strvec, 1);
#ifdef _HAVE_IPVS_TUN_TYPE_
	size_t i;
	int tun_type = IP_VS_CONN_F_TUNNEL_TYPE_IPIP;
	unsigned port = 0;
#ifdef _HAVE_IPVS_TUN_CSUM_
	int csum = IP_VS_TUNNEL_ENCAP_FLAG_NOCSUM;
#endif
#endif

	if (!strcmp(str, "NAT"))
		rs->forwarding_method = IP_VS_CONN_F_MASQ;
	else if (!strcmp(str, "DR"))
		rs->forwarding_method = IP_VS_CONN_F_DROUTE;
	else if (!strcmp(str, "TUN"))
		rs->forwarding_method = IP_VS_CONN_F_TUNNEL;
	else if (!strcmp(str, "FNAT"))
		rs->forwarding_method = IP_VS_CONN_F_FULLNAT;
	else if (!strcmp(str, "SNAT"))
                rs->forwarding_method = IP_VS_CONN_F_SNAT;
	else {
		report_config_error(CONFIG_GENERAL_ERROR, "PARSER : unknown [%s] routing method for %s server.", str, s_type);
		return;
	}

#ifdef _HAVE_IPVS_TUN_TYPE_
	for (i = 2; i < vector_size(strvec); i++) {
		if (!strcmp(strvec_slot(strvec, i), "type")) {
			if (vector_size(strvec) == i + 1) {
				report_config_error(CONFIG_GENERAL_ERROR, "Missing tunnel type for %s server.", s_type);
				return;
			}
			if (!strcmp(strvec_slot(strvec, i + 1), "ipip"))
				tun_type = IP_VS_CONN_F_TUNNEL_TYPE_IPIP;
			else if (!strcmp(strvec_slot(strvec, i + 1), "gue"))
				tun_type = IP_VS_CONN_F_TUNNEL_TYPE_GUE;
#ifdef _HAVE_IPVS_TUN_GRE_
			else if (!strcmp(strvec_slot(strvec, i + 1), "gre"))
				tun_type = IP_VS_CONN_F_TUNNEL_TYPE_GRE;
#endif
			else {
				report_config_error(CONFIG_GENERAL_ERROR, "Unknown tunnel type %s for %s server.", strvec_slot(strvec, i + 1), s_type);
				return;
			}
			i++;
		} else if (!strcmp(strvec_slot(strvec, i), "port")) {
			if (vector_size(strvec) == i + 1) {
				report_config_error(CONFIG_GENERAL_ERROR, "Missing port for %s server gue tunnel.", s_type);
				return;
			}
			if (!read_unsigned_strvec(strvec, i + 1, &port, 1, 65535, false)) {
				report_config_error(CONFIG_GENERAL_ERROR, "Invalid gue tunnel port %s for %s server.", strvec_slot(strvec, i + 1), s_type);
				return;
			}
			i++;
		}
#ifdef _HAVE_IPVS_TUN_CSUM_
		else if (!strcmp(strvec_slot(strvec, i), "nocsum"))
			csum = IP_VS_TUNNEL_ENCAP_FLAG_NOCSUM;
		else if (!strcmp(strvec_slot(strvec, i), "csum"))
			csum = IP_VS_TUNNEL_ENCAP_FLAG_CSUM;
		else if (!strcmp(strvec_slot(strvec, i), "remcsum"))
			csum = IP_VS_TUNNEL_ENCAP_FLAG_REMCSUM;
#endif
		else {
			report_config_error(CONFIG_GENERAL_ERROR, "Invalid tunnel option %s for %s server.", strvec_slot(strvec, i), s_type);
			return;
		}
	}

	if ((tun_type == IP_VS_CONN_F_TUNNEL_TYPE_GUE) != (port != 0)) {
		report_config_error(CONFIG_GENERAL_ERROR, "gue tunnels require port, otherwise cannot have port.");
		return;
	}
#ifdef _HAVE_IPVS_TUN_CSUM_
	if (tun_type == IP_VS_CONN_F_TUNNEL_TYPE_IPIP && csum != IP_VS_TUNNEL_ENCAP_FLAG_NOCSUM) {
		report_config_error(CONFIG_GENERAL_ERROR, "ipip tunnels do not support checksum option.");
		return;
	}
#endif
#ifdef _HAVE_IPVS_TUN_GRE_
	if (tun_type == IP_VS_CONN_F_TUNNEL_TYPE_GRE && csum == IP_VS_TUNNEL_ENCAP_FLAG_REMCSUM) {
		report_config_error(CONFIG_GENERAL_ERROR, "gre tunnels do not support remote checksum option.");
		return;
	}
#endif

#ifdef _HAVE_IPVS_TUN_TYPE_
	rs->tun_type = tun_type;
	rs->tun_port = htons(port);
#ifdef _HAVE_IPVS_TUN_CSUM_
	rs->tun_flags = csum;
#endif
#endif
#endif
}

static void
forwarding_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	real_server_t rs;	// dummy for setting parameters

	svr_forwarding_handler(&rs, strvec, "virtual");
	vs->forwarding_method = rs.forwarding_method;
#ifdef _HAVE_IPVS_TUN_TYPE_
	vs->tun_type = rs.tun_type;
	vs->tun_port = rs.tun_port;
#ifdef _HAVE_IPVS_TUN_CSUM_
	vs->tun_flags = rs.tun_flags;
#endif
#endif
}

static void
pto_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	unsigned timeout;

	if (vector_size(strvec) < 2) {
		vs->persistence_timeout = IPVS_SVC_PERSISTENT_TIMEOUT;
		return;
	}

	if (!read_unsigned_strvec(strvec, 1, &timeout, 1, UINT32_MAX, false)) {
		report_config_error(CONFIG_GENERAL_ERROR, "persistence_timeout invalid");
		return;
	}

	vs->persistence_timeout = (uint32_t)timeout;
}
#ifdef _HAVE_PE_NAME_
static void
pengine_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	const char *str = strvec_slot(strvec, 1);
	size_t size = sizeof (vs->pe_name);

	strncpy(vs->pe_name, str, size - 1);
	vs->pe_name[size - 1] = '\0';
}
#endif
static void
pgr_handler(const vector_t *strvec)
{
	struct in_addr addr;
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	uint16_t af = vs->af;
	unsigned granularity;

	if (af == AF_UNSPEC)
		af = strchr(strvec_slot(strvec, 1), '.') ? AF_INET : AF_INET6;

	if (af == AF_INET6) {
		if (!read_unsigned_strvec(strvec, 1, &granularity, 1, 128, false)) {
			report_config_error(CONFIG_GENERAL_ERROR, "Invalid IPv6 persistence_granularity specified - %s", strvec_slot(strvec, 1));
			return;
		}
		vs->persistence_granularity = granularity;
	} else {
		if (!inet_aton(strvec_slot(strvec, 1), &addr)) {
			report_config_error(CONFIG_GENERAL_ERROR, "Invalid IPv4 persistence_granularity specified - %s", strvec_slot(strvec, 1));
			return;
		}

		/* Ensure the netmask is solid */
		uint32_t haddr = ntohl(addr.s_addr);
		while (!(haddr & 1))
			haddr = (haddr >> 1) | 0x80000000;
		if (haddr != 0xffffffff) {
			report_config_error(CONFIG_GENERAL_ERROR, "IPv4 persistence_granularity netmask is not solid - %s", strvec_slot(strvec, 1));
			return;
		}

		vs->persistence_granularity = addr.s_addr;
	}

	if (vs->af == AF_UNSPEC)
		vs->af = af;

	if (!vs->persistence_timeout)
		vs->persistence_timeout = IPVS_SVC_PERSISTENT_TIMEOUT;
}
static void
proto_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	const char *str = strvec_slot(strvec, 1);

	if (!strcasecmp(str, "TCP"))
		vs->service_type = IPPROTO_TCP;
	else if (!strcasecmp(str, "SCTP"))
		vs->service_type = IPPROTO_SCTP;
	else if (!strcasecmp(str, "UDP"))
		vs->service_type = IPPROTO_UDP;
	else if (!strcasecmp(str, "ICMP"))
                vs->service_type = IPPROTO_ICMP;
	else if (!strcasecmp(str, "ICMPV6"))
		vs->service_type = IPPROTO_ICMPV6;
	else
		report_config_error(CONFIG_GENERAL_ERROR, "Unknown protocol %s - ignoring", str);
}
static void
hasuspend_handler(__attribute__((unused)) const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	vs->ha_suspend = true;
}

static void
vs_smtp_alert_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	int res = true;

	if (vector_size(strvec) >= 2) {
		res = check_true_false(strvec_slot(strvec, 1));
		if (res == -1) {
			report_config_error(CONFIG_GENERAL_ERROR, "Invalid virtual_server smtp_alert parameter %s", strvec_slot(strvec, 1));
			return;
		}
	}
	vs->smtp_alert = res;
	check_data->num_smtp_alert++;
}

static void
vs_virtualhost_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);

	if (vector_size(strvec) < 2) {
		report_config_error(CONFIG_GENERAL_ERROR, "virtual server virtualhost missing");
		return;
	}

	vs->virtualhost = set_value(strvec);
}

/* Sorry Servers handlers */
static void
ssvr_handler(const vector_t *strvec)
{
	alloc_ssvr(strvec_slot(strvec, 1), vector_size(strvec) >= 3 ? strvec_slot(strvec, 2) : NULL);
}
static void
ssvri_handler(__attribute__((unused)) const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	if (vs->s_svr)
		vs->s_svr->inhibit = true;
	else
		report_config_error(CONFIG_GENERAL_ERROR, "Ignoring sorry_server inhibit used before or without sorry_server");
}
static void
ss_forwarding_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);

	if (vs->s_svr)
		svr_forwarding_handler(vs->s_svr, strvec, "sorry");
	else
		report_config_error(CONFIG_GENERAL_ERROR, "sorry_server forwarding used without sorry_server");
}

/* Real Servers handlers */
static void
rs_handler(const vector_t *strvec)
{
	alloc_rs(strvec_slot(strvec, 1), vector_size(strvec) >= 3 ? strvec_slot(strvec, 2) : NULL);
}
static void
rs_end_handler(void)
{
	virtual_server_t *vs;
	real_server_t *rs;
	virtual_server_group_t *vsg;
	virtual_server_group_entry_t *vsge;

	if (LIST_ISEMPTY(check_data->vs))
		return;

	vs = LIST_TAIL_DATA(check_data->vs);

	if (LIST_ISEMPTY(vs->rs))
		return;

	rs = LIST_TAIL_DATA(vs->rs);

	/* Do NOT assign vs->af with rs->addr.ss_family, even if vs->af == AF_UNSPEC,
	 * because vs->af and rs->addr.ss_family are not the same in NAT64.
	 */
}
static void
rs_weight_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	real_server_t *rs = LIST_TAIL_DATA(vs->rs);
	unsigned weight;

	if (!read_unsigned_strvec(strvec, 1, &weight, 0, 65535, true)) {
		report_config_error(CONFIG_GENERAL_ERROR, "Real server weight %s is outside range 0-65535", strvec_slot(strvec, 1));
		return;
	}
	rs->weight = weight;
	rs->iweight = weight;
}
static void
rs_forwarding_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	real_server_t *rs = LIST_TAIL_DATA(vs->rs);

	svr_forwarding_handler(rs, strvec, "real");
}
static void
uthreshold_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	real_server_t *rs = LIST_TAIL_DATA(vs->rs);
	unsigned threshold;

	if (!read_unsigned_strvec(strvec, 1, &threshold, 0, UINT_MAX, true)) {
		report_config_error(CONFIG_GENERAL_ERROR, "Invalid real_server uthreshold '%s' - ignoring", strvec_slot(strvec, 1));
		return;
	}
	rs->u_threshold = threshold;
}
static void
lthreshold_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	real_server_t *rs = LIST_TAIL_DATA(vs->rs);
	unsigned threshold;

	if (!read_unsigned_strvec(strvec, 1, &threshold, 0, UINT_MAX, true)) {
		report_config_error(CONFIG_GENERAL_ERROR, "Invalid real_server lthreshold '%s' - ignoring", strvec_slot(strvec, 1));
		return;
	}
	rs->l_threshold = threshold;
}
static void
vs_inhibit_handler(__attribute__((unused)) const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	vs->inhibit = true;
}
static inline notify_script_t*
set_check_notify_script(__attribute__((unused)) const vector_t *strvec, const char *type)
{
	return notify_script_init(0, type);
}
static void
notify_up_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	real_server_t *rs = LIST_TAIL_DATA(vs->rs);
	if (rs->notify_up) {
		report_config_error(CONFIG_GENERAL_ERROR, "(%s) notify_up script already specified - ignoring %s", vs->vsgname, strvec_slot(strvec,1));
		return;
	}
	rs->notify_up = set_check_notify_script(strvec, "notify");
}
static void
notify_down_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	real_server_t *rs = LIST_TAIL_DATA(vs->rs);
	if (rs->notify_down) {
		report_config_error(CONFIG_GENERAL_ERROR, "(%s) notify_down script already specified - ignoring %s", vs->vsgname, strvec_slot(strvec,1));
		return;
	}
	rs->notify_down = set_check_notify_script(strvec, "notify");
}
static void
rs_co_timeout_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	real_server_t *rs = LIST_TAIL_DATA(vs->rs);
	unsigned long timer;

	if (!read_timer(strvec, 1, &timer, 1, UINT_MAX, true)) {
		report_config_error(CONFIG_GENERAL_ERROR, "real server connect_timeout %s invalid - ignoring", strvec_slot(strvec, 1));
		return;
	}
	rs->connection_to = timer;
}
static void
rs_delay_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	real_server_t *rs = LIST_TAIL_DATA(vs->rs);
	unsigned long delay;

	if (read_timer(strvec, 1, &delay, 1, 0, true))
		rs->delay_loop = delay;
	else
		report_config_error(CONFIG_GENERAL_ERROR, "real server delay_loop '%s' invalid - ignoring", strvec_slot(strvec, 1));
}
static void
rs_delay_before_retry_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	real_server_t *rs = LIST_TAIL_DATA(vs->rs);
	unsigned long delay;

	if (read_timer(strvec, 1, &delay, 0, 0, true))
		rs->delay_before_retry = delay;
	else
		report_config_error(CONFIG_GENERAL_ERROR, "real server delay_before_retry '%s' invalid - ignoring", strvec_slot(strvec, 1));
}
static void
rs_retry_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	real_server_t *rs = LIST_TAIL_DATA(vs->rs);
	unsigned retry;

	if (!read_unsigned_strvec(strvec, 1, &retry, 1, UINT32_MAX, false)) {
		report_config_error(CONFIG_GENERAL_ERROR, "retry value invalid - %s", strvec_slot(strvec, 1));
		return;
	}
	rs->retry = (unsigned)retry;
}
static void
rs_warmup_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	real_server_t *rs = LIST_TAIL_DATA(vs->rs);
	unsigned long delay;

	if (read_timer(strvec, 1, &delay, 0, 0, true))
		rs->warmup = delay;
	else
		report_config_error(CONFIG_GENERAL_ERROR, "real server warmup '%s' invalid - ignoring", strvec_slot(strvec, 1));
}
static void
rs_inhibit_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	real_server_t *rs = LIST_TAIL_DATA(vs->rs);
	int res = true;

	if (vector_size(strvec) >= 2) {
		res = check_true_false(strvec_slot(strvec, 1));
		if (res == -1) {
			report_config_error(CONFIG_GENERAL_ERROR, "Invalid inhibit_on_failure parameter %s", strvec_slot(strvec, 1));
			return;
		}
	}
	rs->inhibit = res;
}
static void
rs_alpha_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	real_server_t *rs = LIST_TAIL_DATA(vs->rs);
	int res = true;

	if (vector_size(strvec) >= 2) {
		res = check_true_false(strvec_slot(strvec, 1));
		if (res == -1) {
			report_config_error(CONFIG_GENERAL_ERROR, "Invalid alpha parameter %s", strvec_slot(strvec, 1));
			return;
		}
	}
	rs->alpha = res;
}
static void
rs_smtp_alert_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	real_server_t *rs = LIST_TAIL_DATA(vs->rs);
	int res = true;

	if (vector_size(strvec) >= 2) {
		res = check_true_false(strvec_slot(strvec, 1));
		if (res == -1) {
			report_config_error(CONFIG_GENERAL_ERROR, "Invalid real_server smtp_alert parameter %s", strvec_slot(strvec, 1));
			return;
		}
	}
	rs->smtp_alert = res;
	check_data->num_smtp_alert++;
}
static void
rs_virtualhost_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	real_server_t *rs = LIST_TAIL_DATA(vs->rs);

	if (vector_size(strvec) < 2) {
		report_config_error(CONFIG_GENERAL_ERROR, "real server virtualhost missing");
		return;
	}

	rs->virtualhost = set_value(strvec);
}
static void
vs_alpha_handler(__attribute__((unused)) const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	vs->alpha = true;
}
static void
omega_handler(__attribute__((unused)) const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	vs->omega = true;
}
static void
quorum_up_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	if (vs->notify_quorum_up) {
		report_config_error(CONFIG_GENERAL_ERROR, "(%s) quorum_up script already specified - ignoring %s", vs->vsgname, strvec_slot(strvec,1));
		return;
	}
	vs->notify_quorum_up = set_check_notify_script(strvec, "quorum");
}
static void
quorum_down_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	if (vs->notify_quorum_down) {
		report_config_error(CONFIG_GENERAL_ERROR, "(%s) quorum_down script already specified - ignoring %s", vs->vsgname, strvec_slot(strvec,1));
		return;
	}
	vs->notify_quorum_down = set_check_notify_script(strvec, "quorum");
}
static void
quorum_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	unsigned quorum;

	if (!read_unsigned_strvec(strvec, 1, &quorum, 1, UINT_MAX, true)) {
		report_config_error(CONFIG_GENERAL_ERROR, "Quorum %s must be in [1, %u]. Setting to 1.", strvec_slot(strvec, 1), UINT_MAX);
		quorum = 1;
	}

	vs->quorum = quorum;
}
static void
hysteresis_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	unsigned hysteresis;

	if (!read_unsigned_strvec(strvec, 1, &hysteresis, 0, UINT_MAX, true)) {
		report_config_error(CONFIG_GENERAL_ERROR, "Hysteresis %s must be in [0, %u] - ignoring", strvec_slot(strvec, 1), UINT_MAX);
		return;
	}

	vs->hysteresis = hysteresis;
}
static void
vs_weight_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	unsigned weight;

	if (!read_unsigned_strvec(strvec, 1, &weight, 1, 65535, true)) {
		report_config_error(CONFIG_GENERAL_ERROR, "Virtual server weight %s is outside range 1-65535", strvec_slot(strvec, 1));
		return;
	}
	vs->weight = weight;
}

static void
laddr_gname_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	vs->local_addr_gname = set_value(strvec);
}

static void
syn_proxy_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	vs->syn_proxy = true;
}

static void
expire_quiescent_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	vs->expire_quiescent_conn = true;
}

static void
bind_dev_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	vs->vip_bind_dev = set_value(strvec);
}

static void
blklst_group_handler(const vector_t *strvec)
{
	if (!strvec)
		return;
	alloc_blklst_group(vector_slot(strvec, 1));
	alloc_value_block(alloc_blklst_entry, strvec_slot(strvec, 0));
}

static void
blklst_gname_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	vs->blklst_addr_gname = set_value(strvec);
}
static void
whtlst_group_handler(const vector_t *strvec)
{
	if (!strvec)
	    return;
    alloc_whtlst_group(vector_slot(strvec, 1));
    alloc_value_block(alloc_whtlst_entry, strvec_slot(strvec, 0));
}
static void
whtlst_gname_handler(const vector_t *strvec)
{
    virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
    vs->whtlst_addr_gname = set_value(strvec);
}

static void
tunnel_handler(const vector_t *strvec)
{
	if (!strvec)
		return;

	alloc_tunnel(vector_slot(strvec, 1));
}

static void
tunnel_entry_handler(const vector_t *strvec)
{
	alloc_tunnel_entry(vector_slot(strvec, 1));
}

static void
kind_handler(const vector_t *strvec)
{
	tunnel_group *gtunnel = LIST_TAIL_DATA(check_data->tunnel_group);
	tunnel_entry *entry = LIST_TAIL_DATA(gtunnel->tunnel_entry);

	strncpy(entry->kind, vector_slot(strvec, 1), sizeof(entry->kind) - 1);
}

static void
remote_handler(const vector_t *strvec)
{
	tunnel_group *gtunnel = LIST_TAIL_DATA(check_data->tunnel_group);
	tunnel_entry *entry = LIST_TAIL_DATA(gtunnel->tunnel_entry);

	inet_stosockaddr(vector_slot(strvec, 1), NULL, &entry->remote);
}

static void
local_handler(const vector_t *strvec)
{
	tunnel_group *gtunnel = LIST_TAIL_DATA(check_data->tunnel_group);
	tunnel_entry *entry = LIST_TAIL_DATA(gtunnel->tunnel_entry);

	inet_stosockaddr(vector_slot(strvec, 1), NULL, &entry->local);
}

static void
if_handler(const vector_t *strvec)
{
	tunnel_group *gtunnel = LIST_TAIL_DATA(check_data->tunnel_group);
	tunnel_entry *entry = LIST_TAIL_DATA(gtunnel->tunnel_entry);
	snprintf(entry->link, sizeof(entry->link), "%s", (char *)vector_slot(strvec, 1));
}


static void
bps_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	char *str = vector_slot(strvec, 1);
	vs->bps = atoi(str);
}

static void
limit_proportion_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
    char *str = vector_slot(strvec, 1);
    vs->limit_proportion = atoi(str);
}

static void
establish_timeout_handler(const vector_t *strvec)
{
	int conn_timeout;
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	conn_timeout = atoi(vector_slot(strvec, 1));
	if (conn_timeout > ESTABLISH_TIMEOUT_MAX)
		conn_timeout = ESTABLISH_TIMEOUT_MAX;
	if (conn_timeout < ESTABLISH_TIMEOUT_MIN)
		conn_timeout = ESTABLISH_TIMEOUT_MIN;
	vs->conn_timeout = conn_timeout;
}

static void
src_range_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	snprintf(vs->srange, sizeof(vs->srange), "%s", (char *)vector_slot(strvec, 1));
}

static void
dst_range_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	snprintf(vs->drange, sizeof(vs->drange), "%s", (char *)vector_slot(strvec, 1));
}

static void
oif_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	snprintf(vs->oifname, sizeof(vs->oifname), "%s", (char *)vector_slot(strvec, 1));
}

static void
iif_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	snprintf(vs->iifname, sizeof(vs->iifname), "%s", (char *)vector_slot(strvec, 1));
}

static void
hash_target_handler(const vector_t *strvec)
{
	virtual_server_t *vs = LIST_TAIL_DATA(check_data->vs);
	char *str = vector_slot(strvec, 1);

	if (!strcmp(str, "sip"))
		vs->hash_target = IP_VS_SVC_F_SIP_HASH;
	else if (!strcmp(str, "qid"))
		vs->hash_target = IP_VS_SVC_F_QID_HASH;
	else {
		vs->hash_target = IP_VS_SVC_F_SIP_HASH;
		log_message(LOG_INFO, "PARSER : unknown [%s] hash target, use source_ip", str);
	}
}

void
init_check_keywords(bool active)
{
	/* SSL mapping */
	install_keyword_root("SSL", &ssl_handler, active);
	install_keyword("password", &sslpass_handler);
	install_keyword("ca", &sslca_handler);
	install_keyword("certificate", &sslcert_handler);
	install_keyword("key", &sslkey_handler);

	/* tunnel process */
	install_keyword_root("tunnel_group", &tunnel_handler, active);
	install_keyword("tunnel_entry", &tunnel_entry_handler);
	install_sublevel();
	install_keyword("kind", &kind_handler);
	install_keyword("remote", &remote_handler);
	install_keyword("local", &local_handler);
	install_keyword("if", &if_handler);
	install_sublevel_end();

	/* local IP address mapping */
	install_keyword_root("local_address_group", &laddr_group_handler, active);
	/* blacklist IP */
	install_keyword_root("deny_address_group", &blklst_group_handler, active);
	/* whitelist IP */
	install_keyword_root("allow_address_group", &whtlst_group_handler, active);

	/* Virtual server mapping */
	install_keyword_root("virtual_server_group", &vsg_handler, active);
	install_keyword_root("virtual_server", &vs_handler, active);
	install_root_end_handler(&vs_end_handler);
	install_keyword("ip_family", &ip_family_handler);
	install_keyword("retry", &vs_retry_handler);
	install_keyword("delay_before_retry", &vs_delay_before_retry_handler);
	install_keyword("warmup", &vs_warmup_handler);
	install_keyword("connect_timeout", &vs_co_timeout_handler);
	install_keyword("delay_loop", &vs_delay_handler);
	install_keyword("inhibit_on_failure", &vs_inhibit_handler);
	install_keyword("lb_algo", &lbalgo_handler);
	install_keyword("lvs_sched", &lbalgo_handler);

	install_keyword("hashed", &lbflags_handler);
#ifdef IP_VS_SVC_F_ONEPACKET
	install_keyword("ops", &lbflags_handler);
#endif
#ifdef IP_VS_SVC_F_SCHED1
	install_keyword("flag-1", &lbflags_handler);
	install_keyword("flag-2", &lbflags_handler);
	install_keyword("flag-3", &lbflags_handler);
	install_keyword("sh-port", &lbflags_handler);
	install_keyword("sh-fallback", &lbflags_handler);
	install_keyword("mh-port", &lbflags_handler);
	install_keyword("mh-fallback", &lbflags_handler);
#endif
	install_keyword("lb_kind", &forwarding_handler);
	install_keyword("establish_timeout", &establish_timeout_handler);
	install_keyword("lvs_method", &forwarding_handler);
#ifdef _HAVE_PE_NAME_
	install_keyword("persistence_engine", &pengine_handler);
#endif
	install_keyword("persistence_timeout", &pto_handler);
	install_keyword("persistence_granularity", &pgr_handler);
	install_keyword("bps", &bps_handler);
	install_keyword("limit_proportion", &limit_proportion_handler);
	install_keyword("protocol", &proto_handler);
	install_keyword("ha_suspend", &hasuspend_handler);
	install_keyword("smtp_alert", &vs_smtp_alert_handler);
	install_keyword("virtualhost", &vs_virtualhost_handler);
	install_keyword("src-range", &src_range_handler);
	install_keyword("dst-range", &dst_range_handler);
	install_keyword("oif", &oif_handler);
	install_keyword("iif", &iif_handler);
	install_keyword("hash_target", &hash_target_handler);

	/* Pool regression detection and handling. */
	install_keyword("alpha", &vs_alpha_handler);
	install_keyword("omega", &omega_handler);
	install_keyword("quorum_up", &quorum_up_handler);
	install_keyword("quorum_down", &quorum_down_handler);
	install_keyword("quorum", &quorum_handler);
	install_keyword("hysteresis", &hysteresis_handler);
	install_keyword("weight", &vs_weight_handler);

	/* Real server mapping */
	install_keyword("sorry_server", &ssvr_handler);
	install_keyword("sorry_server_inhibit", &ssvri_handler);
	install_keyword("sorry_server_lvs_method", &ss_forwarding_handler);
	install_keyword("real_server", &rs_handler);
	install_sublevel();
	install_keyword("weight", &rs_weight_handler);
	install_keyword("lvs_method", &rs_forwarding_handler);
	install_keyword("uthreshold", &uthreshold_handler);
	install_keyword("lthreshold", &lthreshold_handler);
	install_keyword("inhibit_on_failure", &rs_inhibit_handler);
	install_keyword("notify_up", &notify_up_handler);
	install_keyword("notify_down", &notify_down_handler);
	install_keyword("alpha", &rs_alpha_handler);
	install_keyword("retry", &rs_retry_handler);
	install_keyword("delay_before_retry", &rs_delay_before_retry_handler);
	install_keyword("warmup", &rs_warmup_handler);
	install_keyword("connect_timeout", &rs_co_timeout_handler);
	install_keyword("delay_loop", &rs_delay_handler);
	install_keyword("smtp_alert", &rs_smtp_alert_handler);
	install_keyword("virtualhost", &rs_virtualhost_handler);

	install_sublevel_end_handler(&rs_end_handler);

	/* Checkers mapping */
	install_checkers_keyword();
	install_sublevel_end();
	install_keyword("laddr_group_name", &laddr_gname_handler);
	install_keyword("daddr_group_name", &blklst_gname_handler);
	install_keyword("waddr_group_name", &whtlst_gname_handler);
	install_keyword("syn_proxy", &syn_proxy_handler);
	install_keyword("expire_quiescent_conn", &expire_quiescent_handler);
	install_keyword("vip_bind_dev", &bind_dev_handler);
}

const vector_t *
check_init_keywords(void)
{
	/* global definitions mapping */
	init_global_keywords(reload);

	init_check_keywords(true);
#ifdef _WITH_VRRP_
	init_vrrp_keywords(false);
#endif
#ifdef _WITH_BFD_
	init_bfd_keywords(true);
#endif
	return keywords;
}
