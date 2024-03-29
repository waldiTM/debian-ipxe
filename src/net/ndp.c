/*
 * Copyright (C) 2013 Michael Brown <mbrown@fensystems.co.uk>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

FILE_LICENCE ( GPL2_OR_LATER );

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <byteswap.h>
#include <ipxe/in.h>
#include <ipxe/iobuf.h>
#include <ipxe/tcpip.h>
#include <ipxe/ipv6.h>
#include <ipxe/icmpv6.h>
#include <ipxe/neighbour.h>
#include <ipxe/ndp.h>

/** @file
 *
 * IPv6 neighbour discovery protocol
 *
 */

static int ipv6conf_rx_router_advertisement ( struct net_device *netdev,
					      unsigned int flags );

/**
 * Transmit NDP packet with link-layer address option
 *
 * @v netdev		Network device
 * @v sin6_src		Source socket address
 * @v sin6_dest		Destination socket address
 * @v data		NDP header
 * @v len		Size of NDP header
 * @v option_type	NDP option type
 * @ret rc		Return status code
 */
static int ndp_tx_ll_addr ( struct net_device *netdev,
			    struct sockaddr_in6 *sin6_src,
			    struct sockaddr_in6 *sin6_dest,
			    const void *data, size_t len,
			    unsigned int option_type ) {
	struct sockaddr_tcpip *st_src =
		( ( struct sockaddr_tcpip * ) sin6_src );
	struct sockaddr_tcpip *st_dest =
		( ( struct sockaddr_tcpip * ) sin6_dest );
	struct ll_protocol *ll_protocol = netdev->ll_protocol;
	struct io_buffer *iobuf;
	struct ndp_ll_addr_option *ll_addr_opt;
	union ndp_header *ndp;
	size_t option_len;
	int rc;

	/* Allocate and populate buffer */
	option_len = ( ( sizeof ( *ll_addr_opt ) +
			 ll_protocol->ll_addr_len + NDP_OPTION_BLKSZ - 1 ) &
		       ~( NDP_OPTION_BLKSZ - 1 ) );
	iobuf = alloc_iob ( MAX_LL_NET_HEADER_LEN + len + option_len );
	if ( ! iobuf )
		return -ENOMEM;
	iob_reserve ( iobuf, MAX_LL_NET_HEADER_LEN );
	memcpy ( iob_put ( iobuf, len ), data, len );
	ll_addr_opt = iob_put ( iobuf, option_len );
	ll_addr_opt->header.type = option_type;
	ll_addr_opt->header.blocks = ( option_len / NDP_OPTION_BLKSZ );
	memcpy ( ll_addr_opt->ll_addr, netdev->ll_addr,
		 ll_protocol->ll_addr_len );
	ndp = iobuf->data;
	ndp->icmp.chksum = tcpip_chksum ( ndp, ( len + option_len ) );

	/* Transmit packet */
	if ( ( rc = tcpip_tx ( iobuf, &icmpv6_protocol, st_src, st_dest,
			       netdev, &ndp->icmp.chksum ) ) != 0 ) {
		DBGC ( netdev, "NDP could not transmit packet: %s\n",
		       strerror ( rc ) );
		return rc;
	}

	return 0;
}

/**
 * Transmit NDP neighbour discovery request
 *
 * @v netdev		Network device
 * @v net_protocol	Network-layer protocol
 * @v net_dest		Destination network-layer address
 * @v net_source	Source network-layer address
 * @ret rc		Return status code
 */
static int ndp_tx_request ( struct net_device *netdev,
			    struct net_protocol *net_protocol __unused,
			    const void *net_dest, const void *net_source ) {
	struct sockaddr_in6 sin6_src;
	struct sockaddr_in6 sin6_dest;
	struct ndp_neighbour_header neigh;
	int rc;

	/* Construct source address */
	memset ( &sin6_src, 0, sizeof ( sin6_src ) );
	sin6_src.sin6_family = AF_INET6;
	memcpy ( &sin6_src.sin6_addr, net_source,
		 sizeof ( sin6_src.sin6_addr ) );
	sin6_src.sin6_scope_id = netdev->index;

	/* Construct multicast destination address */
	memset ( &sin6_dest, 0, sizeof ( sin6_dest ) );
	sin6_dest.sin6_family = AF_INET6;
	sin6_dest.sin6_scope_id = netdev->index;
	ipv6_solicited_node ( &sin6_dest.sin6_addr, net_dest );

	/* Construct neighbour header */
	memset ( &neigh, 0, sizeof ( neigh ) );
	neigh.icmp.type = ICMPV6_NEIGHBOUR_SOLICITATION;
	memcpy ( &neigh.target, net_dest, sizeof ( neigh.target ) );

	/* Transmit neighbour discovery packet */
	if ( ( rc = ndp_tx_ll_addr ( netdev, &sin6_src, &sin6_dest, &neigh,
				     sizeof ( neigh ),
				     NDP_OPT_LL_SOURCE ) ) != 0 )
		return rc;

	return 0;
}

/** NDP neighbour discovery protocol */
struct neighbour_discovery ndp_discovery = {
	.name = "NDP",
	.tx_request = ndp_tx_request,
};

/**
 * Transmit NDP router solicitation
 *
 * @v netdev		Network device
 * @ret rc		Return status code
 */
static int ndp_tx_router_solicitation ( struct net_device *netdev ) {
	struct ndp_router_solicitation_header rsol;
	struct sockaddr_in6 sin6_dest;
	int rc;

	/* Construct multicast destination address */
	memset ( &sin6_dest, 0, sizeof ( sin6_dest ) );
	sin6_dest.sin6_family = AF_INET6;
	sin6_dest.sin6_scope_id = netdev->index;
	ipv6_all_routers ( &sin6_dest.sin6_addr );

	/* Construct router solicitation */
	memset ( &rsol, 0, sizeof ( rsol ) );
	rsol.icmp.type = ICMPV6_ROUTER_SOLICITATION;

	/* Transmit packet */
	if ( ( rc = ndp_tx_ll_addr ( netdev, NULL, &sin6_dest, &rsol,
				     sizeof ( rsol ), NDP_OPT_LL_SOURCE ) ) !=0)
		return rc;

	return 0;
}

/**
 * Process NDP neighbour solicitation source link-layer address option
 *
 * @v netdev		Network device
 * @v sin6_src		Source socket address
 * @v ndp		NDP packet
 * @v option		NDP option
 * @v len		NDP option length
 * @ret rc		Return status code
 */
static int
ndp_rx_neighbour_solicitation_ll_source ( struct net_device *netdev,
					  struct sockaddr_in6 *sin6_src,
					  union ndp_header *ndp,
					  union ndp_option *option,
					  size_t len ) {
	struct ndp_neighbour_header *neigh = &ndp->neigh;
	struct ndp_ll_addr_option *ll_addr_opt = &option->ll_addr;
	struct ll_protocol *ll_protocol = netdev->ll_protocol;
	int rc;

	/* Silently ignore neighbour solicitations for addresses we do
	 * not own.
	 */
	if ( ! ipv6_has_addr ( netdev, &neigh->target ) )
		return 0;

	/* Sanity check */
	if ( offsetof ( typeof ( *ll_addr_opt ),
			ll_addr[ll_protocol->ll_addr_len] ) > len ) {
		DBGC ( netdev, "NDP neighbour solicitation link-layer address "
		       "option too short at %zd bytes\n", len );
		return -EINVAL;
	}

	/* Create or update neighbour cache entry */
	if ( ( rc = neighbour_define ( netdev, &ipv6_protocol,
				       &sin6_src->sin6_addr,
				       ll_addr_opt->ll_addr ) ) != 0 ) {
		DBGC ( netdev, "NDP could not define %s => %s: %s\n",
		       inet6_ntoa ( &sin6_src->sin6_addr ),
		       ll_protocol->ntoa ( ll_addr_opt->ll_addr ),
		       strerror ( rc ) );
		return rc;
	}

	/* Convert neighbour header to advertisement */
	memset ( neigh, 0, offsetof ( typeof ( *neigh ), target ) );
	neigh->icmp.type = ICMPV6_NEIGHBOUR_ADVERTISEMENT;
	neigh->flags = ( NDP_NEIGHBOUR_SOLICITED | NDP_NEIGHBOUR_OVERRIDE );

	/* Send neighbour advertisement */
	if ( ( rc = ndp_tx_ll_addr ( netdev, NULL, sin6_src, neigh,
				     sizeof ( *neigh ),
				     NDP_OPT_LL_TARGET ) ) != 0 )
		return rc;

	return 0;
}

/**
 * Process NDP neighbour advertisement target link-layer address option
 *
 * @v netdev		Network device
 * @v sin6_src		Source socket address
 * @v ndp		NDP packet
 * @v option		NDP option
 * @v len		NDP option length
 * @ret rc		Return status code
 */
static int
ndp_rx_neighbour_advertisement_ll_target ( struct net_device *netdev,
					   struct sockaddr_in6 *sin6_src
						   __unused,
					   union ndp_header *ndp,
					   union ndp_option *option,
					   size_t len ) {
	struct ndp_neighbour_header *neigh = &ndp->neigh;
	struct ndp_ll_addr_option *ll_addr_opt = &option->ll_addr;
	struct ll_protocol *ll_protocol = netdev->ll_protocol;
	int rc;

	/* Sanity check */
	if ( offsetof ( typeof ( *ll_addr_opt ),
			ll_addr[ll_protocol->ll_addr_len] ) > len ) {
		DBGC ( netdev, "NDP neighbour advertisement link-layer address "
		       "option too short at %zd bytes\n", len );
		return -EINVAL;
	}

	/* Update neighbour cache entry, if any */
	if ( ( rc = neighbour_update ( netdev, &ipv6_protocol, &neigh->target,
				       ll_addr_opt->ll_addr ) ) != 0 ) {
		DBGC ( netdev, "NDP could not update %s => %s: %s\n",
		       inet6_ntoa ( &neigh->target ),
		       ll_protocol->ntoa ( ll_addr_opt->ll_addr ),
		       strerror ( rc ) );
		return rc;
	}

	return 0;
}

/**
 * Process NDP router advertisement source link-layer address option
 *
 * @v netdev		Network device
 * @v sin6_src		Source socket address
 * @v ndp		NDP packet
 * @v option		NDP option
 * @v len		NDP option length
 * @ret rc		Return status code
 */
static int
ndp_rx_router_advertisement_ll_source ( struct net_device *netdev,
					struct sockaddr_in6 *sin6_src,
					union ndp_header *ndp __unused,
					union ndp_option *option, size_t len ) {
	struct ndp_ll_addr_option *ll_addr_opt = &option->ll_addr;
	struct ll_protocol *ll_protocol = netdev->ll_protocol;
	int rc;

	/* Sanity check */
	if ( offsetof ( typeof ( *ll_addr_opt ),
			ll_addr[ll_protocol->ll_addr_len] ) > len ) {
		DBGC ( netdev, "NDP router advertisement link-layer address "
		       "option too short at %zd bytes\n", len );
		return -EINVAL;
	}

	/* Define neighbour cache entry */
	if ( ( rc = neighbour_define ( netdev, &ipv6_protocol,
				       &sin6_src->sin6_addr,
				       ll_addr_opt->ll_addr ) ) != 0 ) {
		DBGC ( netdev, "NDP could not define %s => %s: %s\n",
		       inet6_ntoa ( &sin6_src->sin6_addr ),
		       ll_protocol->ntoa ( ll_addr_opt->ll_addr ),
		       strerror ( rc ) );
		return rc;
	}

	return 0;
}

/**
 * Process NDP router advertisement prefix information option
 *
 * @v netdev		Network device
 * @v sin6_src		Source socket address
 * @v ndp		NDP packet
 * @v option		NDP option
 * @v len		NDP option length
 * @ret rc		Return status code
 */
static int
ndp_rx_router_advertisement_prefix ( struct net_device *netdev,
				     struct sockaddr_in6 *sin6_src,
				     union ndp_header *ndp,
				     union ndp_option *option, size_t len ) {
	struct ndp_router_advertisement_header *radv = &ndp->radv;
	struct ndp_prefix_information_option *prefix_opt = &option->prefix;
	struct in6_addr *router = &sin6_src->sin6_addr;
	int rc;

	/* Sanity check */
	if ( sizeof ( *prefix_opt ) > len ) {
		DBGC ( netdev, "NDP router advertisement prefix option too "
		       "short at %zd bytes\n", len );
		return -EINVAL;
	}
	DBGC ( netdev, "NDP found %sdefault router %s ",
	       ( radv->lifetime ? "" : "non-" ),
	       inet6_ntoa ( &sin6_src->sin6_addr ) );
	DBGC ( netdev, "for %s-link %sautonomous prefix %s/%d\n",
	       ( ( prefix_opt->flags & NDP_PREFIX_ON_LINK ) ? "on" : "off" ),
	       ( ( prefix_opt->flags & NDP_PREFIX_AUTONOMOUS ) ? "" : "non-" ),
	       inet6_ntoa ( &prefix_opt->prefix ),
	       prefix_opt->prefix_len );

	/* Perform stateless address autoconfiguration, if applicable */
	if ( ( prefix_opt->flags &
	       ( NDP_PREFIX_ON_LINK | NDP_PREFIX_AUTONOMOUS ) ) ==
	     ( NDP_PREFIX_ON_LINK | NDP_PREFIX_AUTONOMOUS ) ) {
		if ( ( rc = ipv6_slaac ( netdev, &prefix_opt->prefix,
					 prefix_opt->prefix_len,
					 ( radv->lifetime ?
					   router : NULL ) ) ) != 0 ) {
			DBGC ( netdev, "NDP could not autoconfigure prefix %s/"
			       "%d: %s\n", inet6_ntoa ( &prefix_opt->prefix ),
			       prefix_opt->prefix_len, strerror ( rc ) );
			return rc;
		}
	}

	return 0;
}

/** An NDP option handler */
struct ndp_option_handler {
	/** ICMPv6 type */
	uint8_t icmp_type;
	/** Option type */
	uint8_t option_type;
	/**
	 * Handle received option
	 *
	 * @v netdev		Network device
	 * @v sin6_src		Source socket address
	 * @v ndp		NDP packet
	 * @v option		NDP option
	 * @ret rc		Return status code
	 */
	int ( * rx ) ( struct net_device *netdev, struct sockaddr_in6 *sin6_src,
		       union ndp_header *ndp, union ndp_option *option,
		       size_t len );
};

/** NDP option handlers */
static struct ndp_option_handler ndp_option_handlers[] = {
	{
		.icmp_type = ICMPV6_NEIGHBOUR_SOLICITATION,
		.option_type = NDP_OPT_LL_SOURCE,
		.rx = ndp_rx_neighbour_solicitation_ll_source,
	},
	{
		.icmp_type = ICMPV6_NEIGHBOUR_ADVERTISEMENT,
		.option_type = NDP_OPT_LL_TARGET,
		.rx = ndp_rx_neighbour_advertisement_ll_target,
	},
	{
		.icmp_type = ICMPV6_ROUTER_ADVERTISEMENT,
		.option_type = NDP_OPT_LL_SOURCE,
		.rx = ndp_rx_router_advertisement_ll_source,
	},
	{
		.icmp_type = ICMPV6_ROUTER_ADVERTISEMENT,
		.option_type = NDP_OPT_PREFIX,
		.rx = ndp_rx_router_advertisement_prefix,
	},
};

/**
 * Process received NDP option
 *
 * @v netdev		Network device
 * @v sin6_src		Source socket address
 * @v ndp		NDP packet
 * @v option		NDP option
 * @v len		Option length
 * @ret rc		Return status code
 */
static int ndp_rx_option ( struct net_device *netdev,
			   struct sockaddr_in6 *sin6_src, union ndp_header *ndp,
			   union ndp_option *option, size_t len ) {
	struct ndp_option_handler *handler;
	unsigned int i;

	/* Locate a suitable option handler, if any */
	for ( i = 0 ; i < ( sizeof ( ndp_option_handlers ) /
			    sizeof ( ndp_option_handlers[0] ) ) ; i++ ) {
		handler = &ndp_option_handlers[i];
		if ( ( handler->icmp_type == ndp->icmp.type ) &&
		     ( handler->option_type == option->header.type ) ) {
			return handler->rx ( netdev, sin6_src, ndp,
					     option, len );
		}
	}

	/* Silently ignore unknown options as per RFC 4861 */
	return 0;
}

/**
 * Process received NDP packet options
 *
 * @v netdev		Network device
 * @v sin6_src		Source socket address
 * @v ndp		NDP header
 * @v offset		Offset to NDP options
 * @v len		Length of NDP packet
 * @ret rc		Return status code
 */
static int ndp_rx_options ( struct net_device *netdev,
			    struct sockaddr_in6 *sin6_src,
			    union ndp_header *ndp, size_t offset, size_t len ) {
	union ndp_option *option;
	size_t remaining;
	size_t option_len;
	int rc;

	/* Sanity check */
	if ( len < offset ) {
		DBGC ( netdev, "NDP packet too short at %zd bytes (min %zd "
		       "bytes)\n", len, offset );
		return -EINVAL;
	}

	/* Search for option */
	option = ( ( ( void * ) ndp ) + offset );
	remaining = ( len - offset );
	while ( remaining ) {

		/* Sanity check */
		if ( ( remaining < sizeof ( option->header ) ) ||
		     ( option->header.blocks == 0 ) ||
		     ( remaining < ( option->header.blocks *
				     NDP_OPTION_BLKSZ ) ) ) {
			DBGC ( netdev, "NDP bad option length:\n" );
			DBGC_HDA ( netdev, 0, option, remaining );
			return -EINVAL;
		}
		option_len = ( option->header.blocks * NDP_OPTION_BLKSZ );

		/* Handle option */
		if ( ( rc = ndp_rx_option ( netdev, sin6_src, ndp, option,
					    option_len ) ) != 0 )
			return rc;

		/* Move to next option */
		option = ( ( ( void * ) option ) + option_len );
		remaining -= option_len;
	}

	return 0;
}

/**
 * Process received NDP neighbour solicitation or advertisement
 *
 * @v iobuf		I/O buffer
 * @v netdev		Network device
 * @v sin6_src		Source socket address
 * @v sin6_dest		Destination socket address
 * @ret rc		Return status code
 */
static int ndp_rx_neighbour ( struct io_buffer *iobuf,
			      struct net_device *netdev,
			      struct sockaddr_in6 *sin6_src,
			      struct sockaddr_in6 *sin6_dest __unused ) {
	union ndp_header *ndp = iobuf->data;
	struct ndp_neighbour_header *neigh = &ndp->neigh;
	size_t len = iob_len ( iobuf );
	int rc;

	/* Process options */
	if ( ( rc = ndp_rx_options ( netdev, sin6_src, ndp,
				     offsetof ( typeof ( *neigh ), option ),
				     len ) ) != 0 )
		goto err_options;

 err_options:
	free_iob ( iobuf );
	return rc;
}

/**
 * Process received NDP router advertisement
 *
 * @v iobuf		I/O buffer
 * @v netdev		Network device
 * @v sin6_src		Source socket address
 * @v sin6_dest		Destination socket address
 * @ret rc		Return status code
 */
static int
ndp_rx_router_advertisement ( struct io_buffer *iobuf,
			      struct net_device *netdev,
			      struct sockaddr_in6 *sin6_src,
			      struct sockaddr_in6 *sin6_dest __unused ) {
	union ndp_header *ndp = iobuf->data;
	struct ndp_router_advertisement_header *radv = &ndp->radv;
	size_t len = iob_len ( iobuf );
	int rc;

	/* Process options */
	if ( ( rc = ndp_rx_options ( netdev, sin6_src, ndp,
				     offsetof ( typeof ( *radv ), option ),
				     len ) ) != 0 )
		goto err_options;

	/* Pass to IPv6 autoconfiguration */
	if ( ( rc = ipv6conf_rx_router_advertisement ( netdev,
						       radv->flags ) ) != 0 )
		goto err_ipv6conf;

 err_ipv6conf:
 err_options:
	free_iob ( iobuf );
	return rc;
}

/** NDP ICMPv6 handlers */
struct icmpv6_handler ndp_handlers[] __icmpv6_handler = {
	{
		.type = ICMPV6_NEIGHBOUR_SOLICITATION,
		.rx = ndp_rx_neighbour,
	},
	{
		.type = ICMPV6_NEIGHBOUR_ADVERTISEMENT,
		.rx = ndp_rx_neighbour,
	},
	{
		.type = ICMPV6_ROUTER_ADVERTISEMENT,
		.rx = ndp_rx_router_advertisement,
	},
};

/****************************************************************************
 *
 * IPv6 autoconfiguration
 *
 */

/** An IPv6 configurator */
struct ipv6conf {
	/** Reference count */
	struct refcnt refcnt;
	/** List of configurators */
	struct list_head list;

	/** Job control interface */
	struct interface job;

	/** Network device being configured */
	struct net_device *netdev;

	/** Retransmission timer */
	struct retry_timer timer;
};

/** List of IPv6 configurators */
static LIST_HEAD ( ipv6confs );

/**
 * Free IPv6 configurator
 *
 * @v refcnt		Reference count
 */
static void ipv6conf_free ( struct refcnt *refcnt ) {
	struct ipv6conf *ipv6conf =
		container_of ( refcnt, struct ipv6conf, refcnt );

	netdev_put ( ipv6conf->netdev );
	free ( ipv6conf );
}

/**
 * Identify IPv6 configurator by network device
 *
 * @v netdev		Network device
 * @ret ipv6		IPv6 configurator, or NULL
 */
static struct ipv6conf * ipv6conf_demux ( struct net_device *netdev ) {
	struct ipv6conf *ipv6conf;

	list_for_each_entry ( ipv6conf, &ipv6confs, list ) {
		if ( ipv6conf->netdev == netdev )
			return ipv6conf;
	}
	return NULL;
}

/**
 * Finish IPv6 autoconfiguration
 *
 * @v ipv6		IPv6 configurator
 * @v rc		Reason for finishing
 */
static void ipv6conf_done ( struct ipv6conf *ipv6conf, int rc ) {

	/* Shut down interfaces */
	intf_shutdown ( &ipv6conf->job, rc );

	/* Stop timer */
	stop_timer ( &ipv6conf->timer );

	/* Remove from list and drop list's reference */
	list_del ( &ipv6conf->list );
	ref_put ( &ipv6conf->refcnt );
}

/**
 * Handle IPv6 configurator timer expiry
 *
 * @v timer		Retry timer
 * @v fail		Failure indicator
 */
static void ipv6conf_expired ( struct retry_timer *timer, int fail ) {
	struct ipv6conf *ipv6conf =
		container_of ( timer, struct ipv6conf, timer );

	/* If we have failed, terminate autoconfiguration */
	if ( fail ) {
		ipv6conf_done ( ipv6conf, -ETIMEDOUT );
		return;
	}

	/* Otherwise, transmit router solicitation and restart timer */
	start_timer ( &ipv6conf->timer );
	ndp_tx_router_solicitation ( ipv6conf->netdev );
}

/**
 * Handle router advertisement during IPv6 autoconfiguration
 *
 * @v netdev		Network device
 * @v flags		Router flags
 * @ret rc		Return status code
 */
static int ipv6conf_rx_router_advertisement ( struct net_device *netdev,
					      unsigned int flags ) {
	struct ipv6conf *ipv6conf;

	/* Identify IPv6 configurator, if any */
	ipv6conf = ipv6conf_demux ( netdev );
	if ( ! ipv6conf ) {
		/* Not an error; router advertisements are processed
		 * as a background activity even when no explicit
		 * autoconfiguration is taking place.
		 */
		return 0;
	}

	/* Fail if stateful address autoconfiguration is required */
	if ( flags & NDP_ROUTER_MANAGED ) {
		ipv6conf_done ( ipv6conf, -ENOTSUP );
		return -ENOTSUP;
	}

	/* Mark autoconfiguration as complete */
	ipv6conf_done ( ipv6conf, 0 );

	return 0;
}

/** IPv6 configurator job interface operations */
static struct interface_operation ipv6conf_job_op[] = {
	INTF_OP ( intf_close, struct ipv6conf *, ipv6conf_done ),
};

/** IPv6 configurator job interface descriptor */
static struct interface_descriptor ipv6conf_job_desc =
	INTF_DESC ( struct ipv6conf, job, ipv6conf_job_op );

/**
 * Start IPv6 autoconfiguration
 *
 * @v job		Job control interface
 * @v netdev		Network device
 * @ret rc		Return status code
 */
int start_ipv6conf ( struct interface *job, struct net_device *netdev ) {
	struct ipv6conf *ipv6conf;

	/* Allocate and initialise structure */
	ipv6conf = zalloc ( sizeof ( *ipv6conf ) );
	if ( ! ipv6conf )
		return -ENOMEM;
	ref_init ( &ipv6conf->refcnt, ipv6conf_free );
	intf_init ( &ipv6conf->job, &ipv6conf_job_desc, &ipv6conf->refcnt );
	timer_init ( &ipv6conf->timer, ipv6conf_expired, &ipv6conf->refcnt );
	ipv6conf->netdev = netdev_get ( netdev );

	/* Start timer to initiate router solicitation */
	start_timer_nodelay ( &ipv6conf->timer );

	/* Attach parent interface, transfer reference to list, and return */
	intf_plug_plug ( &ipv6conf->job, job );
	list_add ( &ipv6conf->list, &ipv6confs );
	return 0;
}

/** IPv6 network device configurator */
struct net_device_configurator ipv6_configurator __net_device_configurator = {
	.name = "ipv6",
	.start = start_ipv6conf,
};
