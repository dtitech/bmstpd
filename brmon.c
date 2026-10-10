/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * brmon.c      RTnetlink listener.
 *
 * Authors: Stephen Hemminger <shemminger@osdl.org>
 * Modified by Srinivas Aji <Aji_Srinivas@emc.com>
 *    for use in RSTP daemon. - 2006-09-01
 * Modified by Vitalii Demianets <dvitasgs@gmail.com>
 *    for use in MSTP daemon. - 2011-07-18
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <linux/if_bridge.h>

#include "log.h"
#include "libnetlink.h"
#include "bridge_ctl.h"
#include "netif_utils.h"
#include "epoll_loop.h"

/* RFC 2863 operational status */
enum
{
    IF_OPER_UNKNOWN,
    IF_OPER_NOTPRESENT,
    IF_OPER_DOWN,
    IF_OPER_LOWERLAYERDOWN,
    IF_OPER_TESTING,
    IF_OPER_DORMANT,
    IF_OPER_UP,
};

/* link modes */
enum
{
    IF_LINK_MODE_DEFAULT,
    IF_LINK_MODE_DORMANT, /* limit upward transition to dormant */
};

static struct rtnl_handle rth;
static struct epoll_event_handler br_handler;

static struct rtnl_handle rth_state;

bool handle_all_bridges = 1;
bool have_per_vlan_state = 1;

int br_set_vlan_msti(unsigned ifindex, __u16 vid, __u16 msti)
{
    struct
    {
        struct nlmsghdr n;
        struct br_vlan_msg bvm;
        char buf[256];
    } req;
    struct rtattr *gopts;

    memset(&req, 0, sizeof(req));

    req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct br_vlan_msg));
    req.n.nlmsg_flags = NLM_F_REQUEST;
    req.n.nlmsg_type = RTM_NEWVLAN;
    req.bvm.family = PF_BRIDGE;
    req.bvm.ifindex = ifindex;

    gopts = addattr_nest(&req.n, sizeof(req), BRIDGE_VLANDB_GLOBAL_OPTIONS | NLA_F_NESTED);

    addattr16(&req.n, sizeof(req), BRIDGE_VLANDB_GOPTS_ID, vid);
    addattr16(&req.n, sizeof(req), BRIDGE_VLANDB_GOPTS_MSTI, msti);

    addattr_nest_end(&req.n, gopts);

    return rtnl_talk(&rth_state, &req.n, NULL);
}

int br_set_msti_state(unsigned ifindex, __u16 msti, __u8 state)
{
    struct
    {
        struct nlmsghdr n;
        struct ifinfomsg ifi;
        char buf[256];
    } req;
    struct rtattr *af_spec, *mst, *entry;

    memset(&req, 0, sizeof(req));

    req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req.n.nlmsg_flags = NLM_F_REQUEST;
    req.n.nlmsg_type = RTM_SETLINK;
    req.ifi.ifi_family = PF_BRIDGE;
    req.ifi.ifi_index = ifindex;

    af_spec = addattr_nest(&req.n, sizeof(req), IFLA_AF_SPEC);
    mst = addattr_nest(&req.n, sizeof(req), IFLA_BRIDGE_MST);

    entry = addattr_nest(&req.n, sizeof(req), IFLA_BRIDGE_MST_ENTRY | NLA_F_NESTED);

    addattr16(&req.n, sizeof(req), IFLA_BRIDGE_MST_ENTRY_MSTI, msti);
    addattr8(&req.n, sizeof(req), IFLA_BRIDGE_MST_ENTRY_STATE, state);

    addattr_nest_end(&req.n, entry);
    addattr_nest_end(&req.n, mst);
    addattr_nest_end(&req.n, af_spec);

    return rtnl_talk(&rth_state, &req.n, NULL);
}

int br_set_vlan_state(unsigned ifindex, __u16 vid, __u8 state)
{
    struct
    {
        struct nlmsghdr n;
        struct br_vlan_msg bvm;
        char buf[256];
    } req;
    char entry_buf[256];
    struct rtattr *rta = (void *)entry_buf;
    struct bridge_vlan_info vlan_info;
    struct rtattr *nest;

    LOG("ifindex %d vid %hu state %s", ifindex, vid, stp_state_name(state));

    memset(&req, 0, sizeof(req));

    req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct br_vlan_msg));
    req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_REPLACE;
    req.n.nlmsg_type = RTM_NEWVLAN;
    req.bvm.family = AF_BRIDGE;
    req.bvm.ifindex = ifindex;

    rta->rta_type = BRIDGE_VLANDB_ENTRY;
    rta->rta_len = RTA_LENGTH(0);

    vlan_info.vid = vid;
    vlan_info.flags = BRIDGE_VLAN_INFO_ONLY_OPTS;

    nest = rta_nest(rta, sizeof(entry_buf), BRIDGE_VLANDB_ENTRY);
    rta_addattr_l(rta, sizeof(entry_buf), BRIDGE_VLANDB_ENTRY_INFO, &vlan_info, sizeof(vlan_info));
    rta_addattr8(rta, sizeof(entry_buf), BRIDGE_VLANDB_ENTRY_STATE, state);

    rta_nest_end(rta, nest);

    addraw_l(&req.n, sizeof(req.buf), RTA_DATA(rta), RTA_PAYLOAD(rta));

    return rtnl_talk(&rth_state, &req.n, NULL);
}

int br_set_state(unsigned ifindex, __u8 state)
{
    struct
    {
        struct nlmsghdr n;
        struct ifinfomsg ifi;
        char buf[256];
    } req;

    LOG("ifindex %d state %s", ifindex, stp_state_name(state));

    memset(&req, 0, sizeof(req));

    req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_REPLACE;
    req.n.nlmsg_type = RTM_SETLINK;
    req.ifi.ifi_family = AF_BRIDGE;
    req.ifi.ifi_index = ifindex;

    addattr8(&req.n, sizeof(req.buf), IFLA_PROTINFO, state);

    return rtnl_talk(&rth_state, &req.n, NULL);
}

int br_flush_port(unsigned br_ifindex, unsigned port_ifindex, int vid)
{
    struct
    {
        struct nlmsghdr n;
        struct ndmsg ndm;
        char buf[256];
    } req;

    memset(&req, 0, sizeof(req));

    req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ndmsg));
    req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_BULK;
    req.n.nlmsg_type = RTM_DELNEIGH;
    req.ndm.ndm_family = PF_BRIDGE;
    req.ndm.ndm_ifindex = br_ifindex;

    req.ndm.ndm_flags = NTF_SELF | NTF_MASTER;
    /* only flush dynamic entries */
    req.ndm.ndm_state = 0;

    addattr16(&req.n, sizeof(req.buf), NDA_NDM_STATE_MASK,
              NUD_NOARP | NUD_PERMANENT);
    addattr32(&req.n, sizeof(req.buf), NDA_IFINDEX, port_ifindex);
    if (vid > -1)
        addattr16(&req.n, sizeof(req), NDA_VLAN, vid);

    return rtnl_talk(&rth_state, &req.n, NULL);
}

static int dump_br_msg(struct nlmsghdr *n, void *arg)
{
    struct ifinfomsg *ifi = NLMSG_DATA(n);
    struct rtattr * tb[IFLA_MAX + 1];
    int len = n->nlmsg_len;
    char b1[IFNAMSIZ];
    int af_family;
    bool newlink;
    int br_index;

    if(n->nlmsg_type == NLMSG_DONE)
        return 0;

    len -= NLMSG_LENGTH(sizeof(*ifi));
    if(len < 0)
    {
        return -1;
    }

    af_family = ifi->ifi_family;

    if(af_family != AF_BRIDGE && af_family != AF_UNSPEC)
        return 0;

    if(n->nlmsg_type != RTM_NEWLINK && n->nlmsg_type != RTM_DELLINK)
        return 0;

    parse_rtattr(tb, IFLA_MAX, IFLA_RTA(ifi), len);

    /* Check if we got this from bonding */
    if(tb[IFLA_MASTER] && af_family != AF_BRIDGE)
        return 0;

    if(tb[IFLA_IFNAME] == NULL)
    {
        ERROR("BUG: nil ifname");
        return -1;
    }

    if(n->nlmsg_type == RTM_DELLINK)
        LOG("Deleted ");

    LOG("%d: %s ", ifi->ifi_index, (char*)RTA_DATA(tb[IFLA_IFNAME]));

    if(tb[IFLA_OPERSTATE])
    {
        __u8 state = *(__u8*)RTA_DATA(tb[IFLA_OPERSTATE]);
        switch (state)
        {
            case IF_OPER_UNKNOWN:
                LOG("Unknown ");
                break;
            case IF_OPER_NOTPRESENT:
                LOG("Not Present ");
                break;
            case IF_OPER_DOWN:
                LOG("Down ");
                break;
            case IF_OPER_LOWERLAYERDOWN:
                LOG("Lowerlayerdown ");
                break;
            case IF_OPER_TESTING:
                LOG("Testing ");
                break;
            case IF_OPER_DORMANT:
                LOG("Dormant ");
                break;
            case IF_OPER_UP:
                LOG("Up ");
                break;
            default:
                LOG("State(%d) ", state);
        }
    }

    if(tb[IFLA_MTU])
        LOG("mtu %u ", *(int*)RTA_DATA(tb[IFLA_MTU]));

    if(tb[IFLA_MASTER])
    {
        LOG("master %s ",
                if_indextoname(*(int*)RTA_DATA(tb[IFLA_MASTER]), b1));
    }

    if(tb[IFLA_PROTINFO])
    {
        uint8_t state = *(uint8_t *)RTA_DATA(tb[IFLA_PROTINFO]);
        if(state <= BR_STATE_BLOCKING)
            LOG("state %s", stp_state_name(state));
        else
            LOG("state (%d)", state);
    }

    newlink = (n->nlmsg_type == RTM_NEWLINK);

    if(tb[IFLA_MASTER])
        br_index = *(int*)RTA_DATA(tb[IFLA_MASTER]);
    else if(is_bridge((char*)RTA_DATA(tb[IFLA_IFNAME])))
        br_index = ifi->ifi_index;
    else
        br_index = -1;

    if(br_index >= 0 && tb[IFLA_LINKINFO])
    {
        struct rtattr *tbli[__IFLA_INFO_MAX + 1];
        char *kind = NULL;

        parse_rtattr_nested(tbli, IFLA_INFO_MAX, tb[IFLA_LINKINFO]);
        if (tbli[IFLA_INFO_KIND])
        {
            kind = (char *)RTA_DATA(tbli[IFLA_INFO_KIND]);
        }

        if (kind && !strcmp("bridge", kind) && tbli[IFLA_INFO_DATA])
        {
            struct rtattr *tbbr[__IFLA_BR_MAX + 1];

            parse_rtattr_nested(tbbr, __IFLA_BR_MAX, tbli[IFLA_INFO_DATA]);

            if (tbbr[IFLA_BR_MULTI_BOOLOPT])
            {
                struct br_boolopt_multi *bm;
                bool mst_en;

                bm = (struct br_boolopt_multi *)RTA_DATA(tbbr[IFLA_BR_MULTI_BOOLOPT]);
                mst_en = !!(bm->optval & (1u << BR_BOOLOPT_MST_ENABLE));

                bridge_mst_notify(br_index, mst_en);
            }

        }
    }

    bridge_notify(br_index, ifi->ifi_index, (char*)RTA_DATA(tb[IFLA_IFNAME]), newlink, ifi->ifi_flags);

    return 0;
}

static int dump_vlan_msg(struct nlmsghdr *n, void *arg)
{
    struct br_vlan_msg *bvm = NLMSG_DATA(n);
    struct rtattr *pos;
    int len = n->nlmsg_len - NLMSG_LENGTH(sizeof(*bvm));
    bool newvlan = n->nlmsg_type == RTM_NEWVLAN;

    for (pos = NLMSG_DATA(n) + NLMSG_ALIGN(sizeof(*bvm)); RTA_OK(pos, len); pos = RTA_NEXT(pos, len))
    {
        struct rtattr *tb[BRIDGE_VLANDB_ENTRY_MAX +1];
        struct bridge_vlan_info *info = NULL;
        uint8_t state = VLAN_STATE_UNASSIGNED;
        uint16_t range = 0;

        if ((pos->rta_type & NLA_TYPE_MASK) != BRIDGE_VLANDB_ENTRY)
            continue;

        parse_rtattr_nested(tb, BRIDGE_VLANDB_ENTRY_MAX, pos);

        if (tb[BRIDGE_VLANDB_ENTRY_INFO])
            info = RTA_DATA(tb[BRIDGE_VLANDB_ENTRY_INFO]);
        if (tb[BRIDGE_VLANDB_ENTRY_STATE])
            state = *(uint8_t *)RTA_DATA(tb[BRIDGE_VLANDB_ENTRY_STATE]);
        if (tb[BRIDGE_VLANDB_ENTRY_RANGE])
            range = *(uint16_t*)RTA_DATA(tb[BRIDGE_VLANDB_ENTRY_RANGE]);

        if (!info)
            continue;

        if (!range)
            range = info->vid;

        for (uint16_t vid = info->vid; vid <= range; vid++)
            bridge_vlan_notify(bvm->ifindex, newvlan, vid, state);
    }

    return 0;
}

static int fill_vlan_table_msg(struct nlmsghdr *n, void *arg)
{
    struct br_vlan_msg *bvm = NLMSG_DATA(n);
    struct rtattr *pos;
    int len = n->nlmsg_len - NLMSG_LENGTH(sizeof(*bvm));
    sysdep_uni_data_t *uni_data = arg;

    if (bvm->ifindex != uni_data->if_index)
            return 0;

    for (pos = NLMSG_DATA(n) + NLMSG_ALIGN(sizeof(*bvm)); RTA_OK(pos, len); pos = RTA_NEXT(pos, len))
    {
        struct rtattr *tb[BRIDGE_VLANDB_ENTRY_MAX +1];
        struct bridge_vlan_info *info = NULL;
        uint8_t state = VLAN_STATE_UNASSIGNED;
        uint16_t range = 0;

        if ((pos->rta_type & NLA_TYPE_MASK) != BRIDGE_VLANDB_ENTRY)
            continue;

        parse_rtattr_nested(tb, BRIDGE_VLANDB_ENTRY_MAX, pos);

        if (tb[BRIDGE_VLANDB_ENTRY_INFO])
            info = RTA_DATA(tb[BRIDGE_VLANDB_ENTRY_INFO]);
        if (tb[BRIDGE_VLANDB_ENTRY_STATE])
            state = *(uint8_t *)RTA_DATA(tb[BRIDGE_VLANDB_ENTRY_STATE]);
        if (tb[BRIDGE_VLANDB_ENTRY_RANGE])
            range = *(uint16_t*)RTA_DATA(tb[BRIDGE_VLANDB_ENTRY_RANGE]);

        if (!info)
            continue;

        if (!range)
            range = info->vid;


        for (uint16_t vid = info->vid; vid <= range; vid++)
            uni_data->vlan_state[vid] = state;
    }

    return 0;
}

static int dump_msg(struct nlmsghdr *n, void *arg)
{
    switch (n->nlmsg_type)
    {
        case RTM_NEWLINK:
        case RTM_DELLINK:
            return dump_br_msg(n, arg);
        case RTM_NEWVLAN:
        case RTM_DELVLAN:
            return dump_vlan_msg(n, arg);
        default:
            return 0;
    }
}

static int dump_listen_msg(struct rtnl_ctrl_data *, struct nlmsghdr *n,
                           void *arg)
{
    return dump_msg(n, arg);
}

int fill_vlan_table(sysdep_uni_data_t *uni_data)
{
    struct br_vlan_msg bvm;

    memset(&bvm, 0, sizeof(bvm));
    bvm.family = PF_BRIDGE;
    bvm.ifindex = uni_data->if_index;

    if(!have_per_vlan_state)
        return 0;

    if(rtnl_dump_request(&rth_state, RTM_GETVLAN, &bvm, sizeof(bvm)) < 0)
    {
        ERROR("Cannot send dump request: %m");
        return -1;
    }

    if(rtnl_dump_filter(&rth_state, fill_vlan_table_msg, uni_data) < 0)
    {
        ERROR("Dump terminated");
        return -1;
    }

    return 0;
}

static inline void br_ev_handler(uint32_t events, struct epoll_event_handler *h)
{
    if(rtnl_listen(&rth, dump_listen_msg, stdout) < 0)
    {
        ERROR("Error on bridge monitoring socket");
    }
}

int init_bridge_ops(void)
{
    if(rtnl_open(&rth, RTMGRP_LINK) < 0)
    {
        ERROR("Couldn't open rtnl socket for monitoring");
        return -1;
    }

    if(rtnl_add_nl_group(&rth, RTNLGRP_BRVLAN) < 0)
    {
        ERROR("Couldn't join RTNLGRP_BRVLAN, per vlan STP state not available");
        have_per_vlan_state = 0;
    }

    if(rtnl_open(&rth_state, 0) < 0)
    {
        ERROR("Couldn't open rtnl socket for setting state");
        return -1;
    }

    if(rtnl_linkdump_req(&rth, PF_PACKET) < 0)
    {
        ERROR("Cannot send dump request: %m");
        return -1;
    }

    if(rtnl_dump_filter(&rth, dump_msg, stdout) < 0)
    {
        ERROR("Dump terminated");
        return -1;
    }

    if(fcntl(rth.fd, F_SETFL, O_NONBLOCK) < 0)
    {
        ERROR("Error setting O_NONBLOCK: %m");
        return -1;
    }

    br_handler.fd = rth.fd;
    br_handler.arg = NULL;
    br_handler.handler = br_ev_handler;

    if(add_epoll(&br_handler) < 0)
        return -1;

    return 0;
}
