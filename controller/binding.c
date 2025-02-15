/* Copyright (c) 2015, 2016, 2017 Nicira, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>
#include "binding.h"
#include "ha-chassis.h"
#include "lflow.h"
#include "lport.h"

#include "lib/bitmap.h"
#include "openvswitch/poll-loop.h"
#include "lib/sset.h"
#include "lib/util.h"
#include "lib/netdev.h"
#include "lib/vswitch-idl.h"
#include "openvswitch/hmap.h"
#include "openvswitch/vlog.h"
#include "lib/chassis-index.h"
#include "lib/ovn-sb-idl.h"
#include "ovn-controller.h"

VLOG_DEFINE_THIS_MODULE(binding);

#define OVN_QOS_TYPE "linux-htb"

struct qos_queue {
    struct hmap_node node;
    uint32_t queue_id;
    uint32_t max_rate;
    uint32_t burst;
};

void
binding_register_ovs_idl(struct ovsdb_idl *ovs_idl)
{
    ovsdb_idl_add_table(ovs_idl, &ovsrec_table_open_vswitch);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_open_vswitch_col_bridges);

    ovsdb_idl_add_table(ovs_idl, &ovsrec_table_bridge);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_bridge_col_name);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_bridge_col_ports);

    ovsdb_idl_add_table(ovs_idl, &ovsrec_table_port);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_port_col_name);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_port_col_interfaces);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_port_col_qos);

    ovsdb_idl_add_table(ovs_idl, &ovsrec_table_interface);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_interface_col_name);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_interface_col_external_ids);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_interface_col_bfd);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_interface_col_bfd_status);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_interface_col_status);

    ovsdb_idl_add_table(ovs_idl, &ovsrec_table_qos);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_qos_col_type);
}

static void
get_local_iface_ids(const struct ovsrec_bridge *br_int,
                    struct shash *lport_to_iface,
                    struct sset *local_lports,
                    struct sset *egress_ifaces)
{
    int i;

    for (i = 0; i < br_int->n_ports; i++) {
        const struct ovsrec_port *port_rec = br_int->ports[i];
        const char *iface_id;
        int j;

        if (!strcmp(port_rec->name, br_int->name)) {
            continue;
        }

        for (j = 0; j < port_rec->n_interfaces; j++) {
            const struct ovsrec_interface *iface_rec;

            iface_rec = port_rec->interfaces[j];
            iface_id = smap_get(&iface_rec->external_ids, "iface-id");
            int64_t ofport = iface_rec->n_ofport ? *iface_rec->ofport : 0;

            if (iface_id && ofport > 0) {
                shash_add(lport_to_iface, iface_id, iface_rec);
                sset_add(local_lports, iface_id);
            }

            /* Check if this is a tunnel interface. */
            if (smap_get(&iface_rec->options, "remote_ip")) {
                const char *tunnel_iface
                    = smap_get(&iface_rec->status, "tunnel_egress_iface");
                if (tunnel_iface) {
                    sset_add(egress_ifaces, tunnel_iface);
                }
            }
        }
    }
}

static void
add_local_datapath__(struct ovsdb_idl_index *sbrec_datapath_binding_by_key,
                     struct ovsdb_idl_index *sbrec_port_binding_by_datapath,
                     struct ovsdb_idl_index *sbrec_port_binding_by_name,
                     const struct sbrec_datapath_binding *datapath,
                     bool has_local_l3gateway, int depth,
                     struct hmap *local_datapaths)
{
    uint32_t dp_key = datapath->tunnel_key;
    struct local_datapath *ld = get_local_datapath(local_datapaths, dp_key);
    if (ld) {
        if (has_local_l3gateway) {
            ld->has_local_l3gateway = true;
        }
        return;
    }

    ld = xzalloc(sizeof *ld);
    hmap_insert(local_datapaths, &ld->hmap_node, dp_key);
    ld->datapath = datapath;
    ld->localnet_port = NULL;
    ld->has_local_l3gateway = has_local_l3gateway;

    if (depth >= 100) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);
        VLOG_WARN_RL(&rl, "datapaths nested too deep");
        return;
    }

    struct sbrec_port_binding *target =
        sbrec_port_binding_index_init_row(sbrec_port_binding_by_datapath);
    sbrec_port_binding_index_set_datapath(target, datapath);

    const struct sbrec_port_binding *pb;
    SBREC_PORT_BINDING_FOR_EACH_EQUAL (pb, target,
                                       sbrec_port_binding_by_datapath) {
        if (!strcmp(pb->type, "patch")) {
            const char *peer_name = smap_get(&pb->options, "peer");
            if (peer_name) {
                const struct sbrec_port_binding *peer;

                peer = lport_lookup_by_name(sbrec_port_binding_by_name,
                                            peer_name);

                if (peer && peer->datapath) {
                    add_local_datapath__(sbrec_datapath_binding_by_key,
                                         sbrec_port_binding_by_datapath,
                                         sbrec_port_binding_by_name,
                                         peer->datapath, false,
                                         depth + 1, local_datapaths);
                    ld->n_peer_ports++;
                    ld->peer_ports = xrealloc(ld->peer_ports,
                                              ld->n_peer_ports *
                                              sizeof *ld->peer_ports);
                    ld->peer_ports[ld->n_peer_ports - 1] = peer;
                }
            }
        }
    }
    sbrec_port_binding_index_destroy_row(target);
}

static void
add_local_datapath(struct ovsdb_idl_index *sbrec_datapath_binding_by_key,
                   struct ovsdb_idl_index *sbrec_port_binding_by_datapath,
                   struct ovsdb_idl_index *sbrec_port_binding_by_name,
                   const struct sbrec_datapath_binding *datapath,
                   bool has_local_l3gateway, struct hmap *local_datapaths)
{
    add_local_datapath__(sbrec_datapath_binding_by_key,
                         sbrec_port_binding_by_datapath,
                         sbrec_port_binding_by_name,
                         datapath, has_local_l3gateway, 0, local_datapaths);
}

static void
get_qos_params(const struct sbrec_port_binding *pb, struct hmap *queue_map)
{
    uint32_t max_rate = smap_get_int(&pb->options, "qos_max_rate", 0);
    uint32_t burst = smap_get_int(&pb->options, "qos_burst", 0);
    uint32_t queue_id = smap_get_int(&pb->options, "qdisc_queue_id", 0);

    if ((!max_rate && !burst) || !queue_id) {
        /* Qos is not configured for this port. */
        return;
    }

    struct qos_queue *node = xzalloc(sizeof *node);
    hmap_insert(queue_map, &node->node, hash_int(queue_id, 0));
    node->max_rate = max_rate;
    node->burst = burst;
    node->queue_id = queue_id;
}

static const struct ovsrec_qos *
get_noop_qos(struct ovsdb_idl_txn *ovs_idl_txn,
             const struct ovsrec_qos_table *qos_table)
{
    const struct ovsrec_qos *qos;
    OVSREC_QOS_TABLE_FOR_EACH (qos, qos_table) {
        if (!strcmp(qos->type, "linux-noop")) {
            return qos;
        }
    }

    if (!ovs_idl_txn) {
        return NULL;
    }
    qos = ovsrec_qos_insert(ovs_idl_txn);
    ovsrec_qos_set_type(qos, "linux-noop");
    return qos;
}

static bool
set_noop_qos(struct ovsdb_idl_txn *ovs_idl_txn,
             const struct ovsrec_port_table *port_table,
             const struct ovsrec_qos_table *qos_table,
             struct sset *egress_ifaces)
{
    if (!ovs_idl_txn) {
        return false;
    }

    const struct ovsrec_qos *noop_qos = get_noop_qos(ovs_idl_txn, qos_table);
    if (!noop_qos) {
        return false;
    }

    const struct ovsrec_port *port;
    size_t count = 0;

    OVSREC_PORT_TABLE_FOR_EACH (port, port_table) {
        if (sset_contains(egress_ifaces, port->name)) {
            ovsrec_port_set_qos(port, noop_qos);
            count++;
        }
        if (sset_count(egress_ifaces) == count) {
            break;
        }
    }
    return true;
}

static void
set_qos_type(struct netdev *netdev, const char *type)
{
    int error = netdev_set_qos(netdev, type, NULL);
    if (error) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);
        VLOG_WARN_RL(&rl, "%s: could not set qdisc type \"%s\" (%s)",
                     netdev_get_name(netdev), type, ovs_strerror(error));
    }
}

static void
setup_qos(const char *egress_iface, struct hmap *queue_map)
{
    static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 5);
    struct netdev *netdev_phy;

    if (!egress_iface) {
        /* Queues cannot be configured. */
        return;
    }

    int error = netdev_open(egress_iface, NULL, &netdev_phy);
    if (error) {
        VLOG_WARN_RL(&rl, "%s: could not open netdev (%s)",
                     egress_iface, ovs_strerror(error));
        return;
    }

    /* Check current qdisc. */
    const char *qdisc_type;
    struct smap qdisc_details;

    smap_init(&qdisc_details);
    if (netdev_get_qos(netdev_phy, &qdisc_type, &qdisc_details) != 0 ||
        qdisc_type[0] == '\0') {
        smap_destroy(&qdisc_details);
        netdev_close(netdev_phy);
        /* Qos is not supported. */
        return;
    }
    smap_destroy(&qdisc_details);

    /* If we're not actually being requested to do any QoS:
     *
     *     - If the current qdisc type is OVN_QOS_TYPE, then we clear the qdisc
     *       type to "".  Otherwise, it's possible that our own leftover qdisc
     *       settings could cause strange behavior on egress.  Also, QoS is
     *       expensive and may waste CPU time even if it's not really in use.
     *
     *       OVN isn't the only software that can configure qdiscs, and
     *       physical interfaces are shared resources, so there is some risk in
     *       this strategy: we could disrupt some other program's QoS.
     *       Probably, to entirely avoid this possibility we would need to add
     *       a configuration setting.
     *
     *     - Otherwise leave the qdisc alone. */
    if (hmap_is_empty(queue_map)) {
        if (!strcmp(qdisc_type, OVN_QOS_TYPE)) {
            set_qos_type(netdev_phy, "");
        }
        netdev_close(netdev_phy);
        return;
    }

    /* Configure qdisc. */
    if (strcmp(qdisc_type, OVN_QOS_TYPE)) {
        set_qos_type(netdev_phy, OVN_QOS_TYPE);
    }

    /* Check and delete if needed. */
    struct netdev_queue_dump dump;
    unsigned int queue_id;
    struct smap queue_details;
    struct qos_queue *sb_info;
    struct hmap consistent_queues;

    smap_init(&queue_details);
    hmap_init(&consistent_queues);
    NETDEV_QUEUE_FOR_EACH (&queue_id, &queue_details, &dump, netdev_phy) {
        bool is_queue_needed = false;

        HMAP_FOR_EACH_WITH_HASH (sb_info, node, hash_int(queue_id, 0),
                                 queue_map) {
            is_queue_needed = true;
            if (sb_info->max_rate ==
                smap_get_int(&queue_details, "max-rate", 0)
                && sb_info->burst == smap_get_int(&queue_details, "burst", 0)) {
                /* This queue is consistent. */
                hmap_insert(&consistent_queues, &sb_info->node,
                            hash_int(queue_id, 0));
                break;
            }
        }

        if (!is_queue_needed) {
            error = netdev_delete_queue(netdev_phy, queue_id);
            if (error) {
                VLOG_WARN_RL(&rl, "%s: could not delete queue %u (%s)",
                             egress_iface, queue_id, ovs_strerror(error));
            }
        }
    }

    /* Create/Update queues. */
    HMAP_FOR_EACH (sb_info, node, queue_map) {
        if (hmap_contains(&consistent_queues, &sb_info->node)) {
            hmap_remove(&consistent_queues, &sb_info->node);
            continue;
        }

        smap_clear(&queue_details);
        smap_add_format(&queue_details, "max-rate", "%d", sb_info->max_rate);
        smap_add_format(&queue_details, "burst", "%d", sb_info->burst);
        error = netdev_set_queue(netdev_phy, sb_info->queue_id,
                                 &queue_details);
        if (error) {
            VLOG_WARN_RL(&rl, "%s: could not configure queue %u (%s)",
                         egress_iface, sb_info->queue_id, ovs_strerror(error));
        }
    }
    smap_destroy(&queue_details);
    hmap_destroy(&consistent_queues);
    netdev_close(netdev_phy);
}

static void
update_local_lport_ids(struct sset *local_lport_ids,
                       const struct sbrec_port_binding *binding_rec)
{
        char buf[16];
        snprintf(buf, sizeof(buf), "%"PRId64"_%"PRId64,
                 binding_rec->datapath->tunnel_key,
                 binding_rec->tunnel_key);
        sset_add(local_lport_ids, buf);
}

/*
 * Get the encap from the chassis for this port. The interface
 * may have an external_ids:encap-ip=<encap-ip> set; if so we
 * get the corresponding encap from the chassis.
 * If "encap-ip" external-ids is not set, we'll not bind the port
 * to any specific encap rec. and we'll pick up a tunnel port based on
 * the chassis name alone for the port.
 */
static struct sbrec_encap *
sbrec_get_port_encap(const struct sbrec_chassis *chassis_rec,
                     const struct ovsrec_interface *iface_rec)
{

    if (!iface_rec) {
        return NULL;
    }

    const char *encap_ip = smap_get(&iface_rec->external_ids, "encap-ip");
    if (!encap_ip) {
        return NULL;
    }

    struct sbrec_encap *best_encap = NULL;
    uint32_t best_type = 0;
    for (int i = 0; i < chassis_rec->n_encaps; i++) {
        if (!strcmp(chassis_rec->encaps[i]->ip, encap_ip)) {
            uint32_t tun_type = get_tunnel_type(chassis_rec->encaps[i]->type);
            if (tun_type > best_type) {
                best_type = tun_type;
                best_encap = chassis_rec->encaps[i];
            }
        }
    }
    return best_encap;
}

static bool
is_our_chassis(const struct sbrec_chassis *chassis_rec,
               const struct sbrec_port_binding *binding_rec,
               const struct sset *active_tunnels,
               const struct shash *lport_to_iface,
               const struct sset *local_lports)
{
    const struct ovsrec_interface *iface_rec
        = shash_find_data(lport_to_iface, binding_rec->logical_port);

    bool our_chassis = false;
    if (iface_rec
        || (binding_rec->parent_port && binding_rec->parent_port[0] &&
            sset_contains(local_lports, binding_rec->parent_port))) {
        /* This port is in our chassis unless it is a localport. */
        our_chassis = strcmp(binding_rec->type, "localport");
    } else if (!strcmp(binding_rec->type, "l2gateway")) {
        const char *chassis_id = smap_get(&binding_rec->options,
                                          "l2gateway-chassis");
        our_chassis = chassis_id && !strcmp(chassis_id, chassis_rec->name);
    } else if (!strcmp(binding_rec->type, "chassisredirect") ||
               !strcmp(binding_rec->type, "external")) {
        our_chassis = ha_chassis_group_contains(binding_rec->ha_chassis_group,
                                                chassis_rec) &&
                      ha_chassis_group_is_active(binding_rec->ha_chassis_group,
                                                 active_tunnels, chassis_rec);
    } else if (!strcmp(binding_rec->type, "l3gateway")) {
        const char *chassis_id = smap_get(&binding_rec->options,
                                          "l3gateway-chassis");
        our_chassis = chassis_id && !strcmp(chassis_id, chassis_rec->name);
    }

    return our_chassis;
}

static void
consider_local_datapath(struct ovsdb_idl_txn *ovnsb_idl_txn,
                        struct ovsdb_idl_txn *ovs_idl_txn,
                        struct ovsdb_idl_index *sbrec_datapath_binding_by_key,
                        struct ovsdb_idl_index *sbrec_port_binding_by_datapath,
                        struct ovsdb_idl_index *sbrec_port_binding_by_name,
                        const struct sset *active_tunnels,
                        const struct sbrec_chassis *chassis_rec,
                        const struct sbrec_port_binding *binding_rec,
                        struct hmap *qos_map,
                        struct hmap *local_datapaths,
                        struct shash *lport_to_iface,
                        struct sset *local_lports,
                        struct sset *local_lport_ids)
{
    const struct ovsrec_interface *iface_rec
        = shash_find_data(lport_to_iface, binding_rec->logical_port);

    bool our_chassis = is_our_chassis(chassis_rec, binding_rec, active_tunnels,
                                      lport_to_iface, local_lports);
    if (iface_rec
        || (binding_rec->parent_port && binding_rec->parent_port[0] &&
            sset_contains(local_lports, binding_rec->parent_port))) {
        if (binding_rec->parent_port && binding_rec->parent_port[0]) {
            /* Add child logical port to the set of all local ports. */
            sset_add(local_lports, binding_rec->logical_port);
        }
        add_local_datapath(sbrec_datapath_binding_by_key,
                           sbrec_port_binding_by_datapath,
                           sbrec_port_binding_by_name,
                           binding_rec->datapath, false, local_datapaths);
        if (iface_rec && qos_map && ovs_idl_txn) {
            get_qos_params(binding_rec, qos_map);
        }
    } else if (!strcmp(binding_rec->type, "l2gateway")) {
        if (our_chassis) {
            sset_add(local_lports, binding_rec->logical_port);
            add_local_datapath(sbrec_datapath_binding_by_key,
                               sbrec_port_binding_by_datapath,
                               sbrec_port_binding_by_name,
                               binding_rec->datapath, false, local_datapaths);
        }
    } else if (!strcmp(binding_rec->type, "chassisredirect")) {
        if (ha_chassis_group_contains(binding_rec->ha_chassis_group,
                                      chassis_rec)) {
            add_local_datapath(sbrec_datapath_binding_by_key,
                               sbrec_port_binding_by_datapath,
                               sbrec_port_binding_by_name,
                               binding_rec->datapath, false, local_datapaths);
        }
    } else if (!strcmp(binding_rec->type, "l3gateway")) {
        if (our_chassis) {
            add_local_datapath(sbrec_datapath_binding_by_key,
                               sbrec_port_binding_by_datapath,
                               sbrec_port_binding_by_name,
                               binding_rec->datapath, true, local_datapaths);
        }
    } else if (!strcmp(binding_rec->type, "localnet")) {
        /* Add all localnet ports to local_lports so that we allocate ct zones
         * for them. */
        sset_add(local_lports, binding_rec->logical_port);
    } else if (!strcmp(binding_rec->type, "external")) {
        if (ha_chassis_group_contains(binding_rec->ha_chassis_group,
                                      chassis_rec)) {
            add_local_datapath(sbrec_datapath_binding_by_key,
                               sbrec_port_binding_by_datapath,
                               sbrec_port_binding_by_name,
                               binding_rec->datapath, false, local_datapaths);
        }
    }

    if (our_chassis
        || !strcmp(binding_rec->type, "patch")
        || !strcmp(binding_rec->type, "localport")
        || !strcmp(binding_rec->type, "vtep")
        || !strcmp(binding_rec->type, "localnet")) {
        update_local_lport_ids(local_lport_ids, binding_rec);
    }

    ovs_assert(ovnsb_idl_txn);
    if (ovnsb_idl_txn) {
        const char *vif_chassis = smap_get(&binding_rec->options,
                                           "requested-chassis");
        bool can_bind = !vif_chassis || !vif_chassis[0]
                        || !strcmp(vif_chassis, chassis_rec->name)
                        || !strcmp(vif_chassis, chassis_rec->hostname);

        if (can_bind && our_chassis) {
            if (binding_rec->chassis != chassis_rec) {
                if (binding_rec->chassis) {
                    VLOG_INFO("Changing chassis for lport %s from %s to %s.",
                              binding_rec->logical_port,
                              binding_rec->chassis->name,
                              chassis_rec->name);
                } else {
                    VLOG_INFO("Claiming lport %s for this chassis.",
                              binding_rec->logical_port);
                }
                for (int i = 0; i < binding_rec->n_mac; i++) {
                    VLOG_INFO("%s: Claiming %s",
                              binding_rec->logical_port, binding_rec->mac[i]);
                }
                sbrec_port_binding_set_chassis(binding_rec, chassis_rec);
            }
            /* Check if the port encap binding, if any, has changed */
            struct sbrec_encap *encap_rec = sbrec_get_port_encap(
                                            chassis_rec, iface_rec);
            if (encap_rec && binding_rec->encap != encap_rec) {
                sbrec_port_binding_set_encap(binding_rec, encap_rec);
            }
        } else if (binding_rec->chassis == chassis_rec) {
            VLOG_INFO("Releasing lport %s from this chassis.",
                      binding_rec->logical_port);
            if (binding_rec->encap)
                sbrec_port_binding_set_encap(binding_rec, NULL);
            sbrec_port_binding_set_chassis(binding_rec, NULL);
        } else if (our_chassis) {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
            VLOG_INFO_RL(&rl,
                         "Not claiming lport %s, chassis %s "
                         "requested-chassis %s",
                         binding_rec->logical_port,
                         chassis_rec->name,
                         vif_chassis);
        }
    }
}

static void
consider_localnet_port(const struct sbrec_port_binding *binding_rec,
                       struct hmap *local_datapaths)
{
    struct local_datapath *ld
        = get_local_datapath(local_datapaths,
                             binding_rec->datapath->tunnel_key);
    if (!ld) {
        return;
    }

    if (ld->localnet_port && strcmp(ld->localnet_port->logical_port,
                                    binding_rec->logical_port)) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
        VLOG_WARN_RL(&rl, "localnet port '%s' already set for datapath "
                     "'%"PRId64"', skipping the new port '%s'.",
                     ld->localnet_port->logical_port,
                     binding_rec->datapath->tunnel_key,
                     binding_rec->logical_port);
        return;
    }
    ld->localnet_port = binding_rec;
}

void
binding_run(struct ovsdb_idl_txn *ovnsb_idl_txn,
            struct ovsdb_idl_txn *ovs_idl_txn,
            struct ovsdb_idl_index *sbrec_datapath_binding_by_key,
            struct ovsdb_idl_index *sbrec_port_binding_by_datapath,
            struct ovsdb_idl_index *sbrec_port_binding_by_name,
            const struct ovsrec_port_table *port_table,
            const struct ovsrec_qos_table *qos_table,
            const struct sbrec_port_binding_table *port_binding_table,
            const struct ovsrec_bridge *br_int,
            const struct sbrec_chassis *chassis_rec,
            const struct sset *active_tunnels,
            struct hmap *local_datapaths, struct sset *local_lports,
            struct sset *local_lport_ids)
{
    if (!chassis_rec) {
        return;
    }

    const struct sbrec_port_binding *binding_rec;
    struct shash lport_to_iface = SHASH_INITIALIZER(&lport_to_iface);
    struct sset egress_ifaces = SSET_INITIALIZER(&egress_ifaces);
    struct hmap qos_map;

    hmap_init(&qos_map);
    if (br_int) {
        get_local_iface_ids(br_int, &lport_to_iface, local_lports,
                            &egress_ifaces);
    }

    /* Run through each binding record to see if it is resident on this
     * chassis and update the binding accordingly.  This includes both
     * directly connected logical ports and children of those ports. */
    SBREC_PORT_BINDING_TABLE_FOR_EACH (binding_rec, port_binding_table) {
        consider_local_datapath(ovnsb_idl_txn, ovs_idl_txn,
                                sbrec_datapath_binding_by_key,
                                sbrec_port_binding_by_datapath,
                                sbrec_port_binding_by_name,
                                active_tunnels, chassis_rec, binding_rec,
                                sset_is_empty(&egress_ifaces) ? NULL :
                                &qos_map, local_datapaths, &lport_to_iface,
                                local_lports, local_lport_ids);

    }

    /* Run through each binding record to see if it is a localnet port
     * on local datapaths discovered from above loop, and update the
     * corresponding local datapath accordingly. */
    SBREC_PORT_BINDING_TABLE_FOR_EACH (binding_rec, port_binding_table) {
        if (!strcmp(binding_rec->type, "localnet")) {
            consider_localnet_port(binding_rec, local_datapaths);
        }
    }

    if (!sset_is_empty(&egress_ifaces)
        && set_noop_qos(ovs_idl_txn, port_table, qos_table, &egress_ifaces)) {
        const char *entry;
        SSET_FOR_EACH (entry, &egress_ifaces) {
            setup_qos(entry, &qos_map);
        }
    }

    shash_destroy(&lport_to_iface);
    sset_destroy(&egress_ifaces);
    hmap_destroy(&qos_map);
}

/* Returns true if port-binding changes potentially require flow changes on
 * the current chassis. Returns false if we are sure there is no impact. */
bool
binding_evaluate_port_binding_changes(
        const struct sbrec_port_binding_table *pb_table,
        const struct ovsrec_bridge *br_int,
        const struct sbrec_chassis *chassis_rec,
        struct sset *active_tunnels,
        struct sset *local_lports)
{
    if (!chassis_rec) {
        return true;
    }

    bool changed = false;

    const struct sbrec_port_binding *binding_rec;
    struct shash lport_to_iface = SHASH_INITIALIZER(&lport_to_iface);
    struct sset egress_ifaces = SSET_INITIALIZER(&egress_ifaces);
    if (br_int) {
        get_local_iface_ids(br_int, &lport_to_iface, local_lports,
                            &egress_ifaces);
    }
    SBREC_PORT_BINDING_TABLE_FOR_EACH_TRACKED (binding_rec, pb_table) {
        /* XXX: currently OVSDB change tracking doesn't support getting old
         * data when the operation is update, so if a port-binding moved from
         * this chassis to another, there is no easy way to find out the
         * change. To workaround this problem, we just makes sure if
         * any port *related to* this chassis has any change, then trigger
         * recompute.
         *
         * - If a regular VIF is unbound from this chassis, the local ovsdb
         *   interface table will be updated, which will trigger recompute.
         *
         * - If the port is not a regular VIF, always trigger recompute. */
        if (binding_rec->chassis == chassis_rec
            || is_our_chassis(chassis_rec, binding_rec,
                              active_tunnels, &lport_to_iface, local_lports)
            || strcmp(binding_rec->type, "")) {
            changed = true;
            break;
        }
    }

    shash_destroy(&lport_to_iface);
    sset_destroy(&egress_ifaces);
    return changed;
}

/* Returns true if the database is all cleaned up, false if more work is
 * required. */
bool
binding_cleanup(struct ovsdb_idl_txn *ovnsb_idl_txn,
                const struct sbrec_port_binding_table *port_binding_table,
                const struct sbrec_chassis *chassis_rec)
{
    if (!ovnsb_idl_txn) {
        return false;
    }
    if (!chassis_rec) {
        return true;
    }

    const struct sbrec_port_binding *binding_rec;
    bool any_changes = false;
    SBREC_PORT_BINDING_TABLE_FOR_EACH (binding_rec, port_binding_table) {
        if (binding_rec->chassis == chassis_rec) {
            if (binding_rec->encap)
                sbrec_port_binding_set_encap(binding_rec, NULL);
            sbrec_port_binding_set_chassis(binding_rec, NULL);
            any_changes = true;
        }
    }

    if (any_changes) {
        ovsdb_idl_txn_add_comment(
            ovnsb_idl_txn,
            "ovn-controller: removing all port bindings for '%s'",
            chassis_rec->name);
    }

    return !any_changes;
}
