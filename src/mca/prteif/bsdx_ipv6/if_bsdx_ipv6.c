/*
 * Copyright (c) 2010-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2010      Oracle and/or its affiliates.  All rights reserved.
 * Copyright (c) 2018      Amazon.com, Inc. or its affiliates.  All Rights
 *                         reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"
#include "src/util/output.h"
#include "src/util/pmix_string_copy.h"

#include <string.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#include <errno.h>
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#    include <sys/socket.h>
#endif
#ifdef HAVE_SYS_SOCKIO_H
#    include <sys/sockio.h>
#endif
#ifdef HAVE_SYS_IOCTL_H
#    include <sys/ioctl.h>
#endif
#ifdef HAVE_NETINET_IN_H
#    include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#    include <arpa/inet.h>
#endif
#ifdef HAVE_NET_IF_H
#    include <net/if.h>
#endif
#ifdef HAVE_NETDB_H
#    include <netdb.h>
#endif
#ifdef HAVE_IFADDRS_H
#    include <ifaddrs.h>
#endif

#include "src/mca/prteif/base/base.h"
#include "src/mca/prteif/prteif.h"

static int if_bsdx_ipv6_open(void);

/* Discovers IPv6 interfaces for:
 *
 * NetBSD
 * OpenBSD
 * FreeBSD
 * 386BSD
 * bsdi
 * Apple
 */
prte_if_base_component_t prte_prteif_bsdx_ipv6_component = {
    /* First, the mca_component_t struct containing meta information
       about the component itself */
    {PRTE_IF_BASE_VERSION_2_0_0,

     /* Component name and version */
     "bsdx_ipv6", PRTE_MAJOR_VERSION, PRTE_MINOR_VERSION, PMIX_RELEASE_VERSION,

     /* Component open and close functions */
     if_bsdx_ipv6_open, NULL},
    {/* This component is checkpointable */
     PRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT},
};

/* configure using getifaddrs(3) */
static int if_bsdx_ipv6_open(void)
{
#if PRTE_ENABLE_IPV6
    struct ifaddrs **ifadd_list;
    struct ifaddrs *cur_ifaddrs;
    struct sockaddr_in6 *sin_addr;

    prte_output_verbose(1, prte_prteif_base_framework.framework_output,
                        "searching for IPv6 interfaces");

    /*
     * the manpage claims that getifaddrs() allocates the memory,
     * and freeifaddrs() is later used to release the allocated memory.
     * however, without this malloc the call to getifaddrs() segfaults
     */
    ifadd_list = (struct ifaddrs **) malloc(sizeof(struct ifaddrs *));

    /* create the linked list of ifaddrs structs */
    if (getifaddrs(ifadd_list) < 0) {
        prte_output(0, "prte_ifinit: getifaddrs() failed with error=%d\n", errno);
        free(ifadd_list);
        return PRTE_ERROR;
    }

    for (cur_ifaddrs = *ifadd_list; NULL != cur_ifaddrs; cur_ifaddrs = cur_ifaddrs->ifa_next) {
        prte_if_t *intf;
        struct in6_addr a6;

        /* skip non-ipv6 interface addresses */
        if (AF_INET6 != cur_ifaddrs->ifa_addr->sa_family) {
            prte_output_verbose(1, prte_prteif_base_framework.framework_output,
                                "skipping non-ipv6 interface %s[%d].\n", cur_ifaddrs->ifa_name,
                                (int) cur_ifaddrs->ifa_addr->sa_family);
            continue;
        }

        /* skip interface if it is down (IFF_UP not set) */
        if (0 == (cur_ifaddrs->ifa_flags & IFF_UP)) {
            prte_output_verbose(1, prte_prteif_base_framework.framework_output,
                                "skipping non-up interface %s.\n", cur_ifaddrs->ifa_name);
            continue;
        }

        /* skip interface if it is a loopback device (IFF_LOOPBACK set) */
        if (!prte_if_retain_loopback && 0 != (cur_ifaddrs->ifa_flags & IFF_LOOPBACK)) {
            prte_output_verbose(1, prte_prteif_base_framework.framework_output,
                                "skipping loopback interface %s.\n", cur_ifaddrs->ifa_name);
            continue;
        }

        /* or if it is a point-to-point interface */
        /* TODO: do we really skip p2p? */
        if (0 != (cur_ifaddrs->ifa_flags & IFF_POINTOPOINT)) {
            prte_output_verbose(1, prte_prteif_base_framework.framework_output,
                                "skipping p2p interface %s.\n", cur_ifaddrs->ifa_name);
            continue;
        }

        sin_addr = (struct sockaddr_in6 *) cur_ifaddrs->ifa_addr;

        /*
         * skip IPv6 address starting with fe80:, as this is supposed to be
         * link-local scope. sockaddr_in6->sin6_scope_id doesn't always work
         * TODO: test whether scope id is set to a sensible value on
         * linux and/or bsd (including osx)
         *
         * MacOSX: fe80::... has a scope of 0, but ifconfig -a shows
         * a scope of 4 on that particular machine,
         * so the scope returned by getifaddrs() isn't working properly
         */

        if ((IN6_IS_ADDR_LINKLOCAL(&sin_addr->sin6_addr))) {
            prte_output_verbose(1, prte_prteif_base_framework.framework_output,
                                "skipping link-local ipv6 address on interface "
                                "%s with scope %d.\n",
                                cur_ifaddrs->ifa_name, sin_addr->sin6_scope_id);
            continue;
        }

        if (0 < prte_output_get_verbosity(prte_prteif_base_framework.framework_output)) {
            char *addr_name = (char *) malloc(48 * sizeof(char));
            inet_ntop(AF_INET6, &sin_addr->sin6_addr, addr_name, 48 * sizeof(char));
            prte_output(0, "ipv6 capable interface %s discovered, address %s.\n",
                        cur_ifaddrs->ifa_name, addr_name);
            free(addr_name);
        }

        /* fill values into the prte_if_t */
        memcpy(&a6, &(sin_addr->sin6_addr), sizeof(struct in6_addr));

        intf = PMIX_NEW(prte_if_t);
        if (NULL == intf) {
            prte_output(0, "prte_ifinit: unable to allocate %lu bytes\n", sizeof(prte_if_t));
            free(ifadd_list);
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
        intf->af_family = AF_INET6;
        pmix_string_copy(intf->if_name, cur_ifaddrs->ifa_name, PMIX_IF_NAMESIZE);
        intf->if_index = pmix_list_get_size(&prte_if_list) + 1;
        ((struct sockaddr_in6 *) &intf->if_addr)->sin6_addr = a6;
        ((struct sockaddr_in6 *) &intf->if_addr)->sin6_family = AF_INET6;

        /* since every scope != 0 is ignored, we just set the scope to 0 */
        ((struct sockaddr_in6 *) &intf->if_addr)->sin6_scope_id = 0;

        /*
         * hardcoded netmask, adrian says that's ok
         */
        intf->if_mask = 64;
        intf->if_flags = cur_ifaddrs->ifa_flags;

        /*
         * FIXME: figure out how to gain access to the kernel index
         * (or create our own), getifaddrs() does not contain such
         * data
         */
        intf->if_kernel_index = (uint16_t) if_nametoindex(cur_ifaddrs->ifa_name);
        pmix_list_append(&prte_if_list, &(intf->super));
    } /*  of for loop over ifaddrs list */

    free(ifadd_list);
#endif /* PRTE_ENABLE_IPV6 */

    return PRTE_SUCCESS;
}
