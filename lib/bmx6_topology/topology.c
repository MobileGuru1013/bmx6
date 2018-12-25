/*
 * Copyright (c) 2010  Axel Neumann
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/inotify.h>

#include "bmx.h"
#include "msg.h"
#include "plugin.h"
#include "schedule.h"
#include "metrics.h"
#include "ip.h"
#include "topology.h"


#define CODE_CATEGORY_NAME "topology"


AVL_TREE(local_topology_tree, struct local_topology_node, pkid);

int32_t my_topology_hysteresis = DEF_TOPOLOGY_HYSTERESIS;
int32_t my_topology_period = DEF_TOPOLOGY_PERIOD;

struct description_msg_topology *topology_msg;
uint32_t topology_msgs;


struct topology_status {
        char* name;
        GLOBAL_ID_T *id;
        IPX_T primaryIp;
        char* neighName;
        GLOBAL_ID_T *neighId;
        IPX_T neighIp;
        uint8_t blocked;
	uint32_t lastDesc;
        UMETRIC_T txBw;
        UMETRIC_T rxBw;
	uint8_t txRate;
	uint8_t rxRate;
};

static const struct field_format topology_status_format[] = {
        FIELD_FORMAT_INIT(FIELD_TYPE_POINTER_CHAR,      topology_status, name,          1, FIELD_RELEVANCE_HIGH),
        FIELD_FORMAT_INIT(FIELD_TYPE_POINTER_GLOBAL_ID, topology_status, id,            1, FIELD_RELEVANCE_MEDI),
        FIELD_FORMAT_INIT(FIELD_TYPE_IPX,               topology_status, primaryIp,     1, FIELD_RELEVANCE_HIGH),
        FIELD_FORMAT_INIT(FIELD_TYPE_POINTER_CHAR,      topology_status, neighName,     1, FIELD_RELEVANCE_HIGH),
        FIELD_FORMAT_INIT(FIELD_TYPE_POINTER_GLOBAL_ID, topology_status, neighId,       1, FIELD_RELEVANCE_MEDI),
        FIELD_FORMAT_INIT(FIELD_TYPE_IPX,               topology_status, neighIp,       1, FIELD_RELEVANCE_HIGH),
        FIELD_FORMAT_INIT(FIELD_TYPE_UINT,              topology_status, blocked,       1, FIELD_RELEVANCE_HIGH),
        FIELD_FORMAT_INIT(FIELD_TYPE_UINT,              topology_status, lastDesc,      1, FIELD_RELEVANCE_HIGH),
        FIELD_FORMAT_INIT(FIELD_TYPE_UMETRIC,           topology_status, txBw,          1, FIELD_RELEVANCE_HIGH),
        FIELD_FORMAT_INIT(FIELD_TYPE_UMETRIC,           topology_status, rxBw,          1, FIELD_RELEVANCE_HIGH),
        FIELD_FORMAT_INIT(FIELD_TYPE_UINT,              topology_status, txRate,        1, FIELD_RELEVANCE_HIGH),
        FIELD_FORMAT_INIT(FIELD_TYPE_UINT,              topology_status, rxRate,        1, FIELD_RELEVANCE_HIGH),
        FIELD_FORMAT_END
};



static int32_t topology_status_creator(struct status_handl *handl, void *data)
{
        struct avl_node *it = NULL;
        struct orig_node *on;
        uint32_t stsize = 0;
        uint32_t i = 0;
        struct topology_status *status = NULL;

        while (data ? (on = data) : (on = avl_iterate_item(&orig_tree, &it))) {

		topology_msgs = 0;
		topology_msg = NULL;
		uint32_t m = 0;
		process_description_tlvs(NULL, on, on->desc, TLV_OP_CUSTOM_TOPOLOGY, BMX_DSC_TLV_TOPOLOGY, NULL, NULL);

		for (m=0; topology_msg && m < topology_msgs; m++) {

			struct orig_node *non;
			struct avl_node *nan = NULL;
			while ((non = avl_iterate_item(&orig_tree, &nan)) && memcmp(&non->global_id.pkid, &topology_msg[m].pkid, sizeof(PKID_T)));

			if (non) {
				stsize += sizeof(struct topology_status);
				status = ((struct topology_status*) (handl->data = debugRealloc(handl->data, stsize, -300366)));

				memset(&status[i], 0, sizeof(struct topology_status));
				status[i].name = on->global_id.name;
				status[i].id = &on->global_id;
				status[i].primaryIp = on->primary_ip;
				status[i].blocked = on->blocked || non->blocked;
				status[i].lastDesc = (bmx_time - on->updated_timestamp) / 1000;
				status[i].txBw = fmetric_u8_to_umetric(topology_msg[m].txBw);
				status[i].rxBw = fmetric_u8_to_umetric(topology_msg[m].rxBw);
				status[i].txRate = topology_msg[m].txRate;
				status[i].rxRate = topology_msg[m].rxRate;
				status[i].neighName = non->global_id.name;
				status[i].neighId = &non->global_id;
				status[i].neighIp = non->primary_ip;

				i++;
			}
		}

                if(data)
                        break;

        }
        return stsize;
}



STATIC_FUNC
int process_description_topology(struct rx_frame_iterator *it)
{

	if (it->op == TLV_OP_CUSTOM_TOPOLOGY) {
		topology_msgs = it->frame_msgs_length / it->handl->min_msg_size;
		topology_msg = (struct description_msg_topology*)it->frame_data;
	}

	return it->frame_msgs_length;
}

STATIC_INLINE_FUNC
UMETRIC_T umetric_multiply_normalized_topology(UMETRIC_T a, UMETRIC_T b)
{
        if (b < UMETRIC_MULTIPLY_MAX)
                return (a * b) / UMETRIC_MAX;
        else
                return (a * ((b << UMETRIC_SHIFT_MAX) / UMETRIC_MAX)) >> UMETRIC_SHIFT_MAX;
}

STATIC_FUNC
int check_value_deviation(UMETRIC_T a, UMETRIC_T b, UMETRIC_T percent)
{
	if ((((a * (100+percent)) / 100) < b) || (((b * (100+percent)) / 100) < a))
		return YES;

	return NO;
}

STATIC_FUNC
void set_local_topology_node(struct local_topology_node *ltn, struct local_node *local)
{
	assertion(-500000, (ltn && local && local->neigh && local->neigh->dhn->on && local->best_tp_lndev));

	ltn->txBw = umetric_multiply_normalized_topology(local->best_tp_lndev->key.dev->umetric_max, local->best_tp_lndev->timeaware_tx_probe);
	ltn->rxBw = umetric_multiply_normalized_topology(local->best_tp_lndev->key.dev->umetric_max, local->best_tp_lndev->timeaware_rx_probe);
	ltn->txRate = ((local->best_tp_lndev->timeaware_tx_probe * 100) / UMETRIC_MAX);
	ltn->rxRate = ((local->best_tp_lndev->timeaware_rx_probe * 100) / UMETRIC_MAX);
}

STATIC_FUNC
void check_local_topology_cache(void *nothing)
{
	assertion(-500000, (my_topology_period < MAX_TOPOLOGY_PERIOD));

        struct avl_node *local_it;
        struct local_node *local;
	uint32_t m = 0;

	for (local_it = NULL; (local = avl_iterate_item(&local_tree, &local_it));) {

		if (local->neigh && local->neigh->dhn->on && local->best_tp_lndev) {

			struct local_topology_node *ltn = avl_find_item(&local_topology_tree, &local->neigh->dhn->on->global_id.pkid);
			struct local_topology_node tmp;

			if (!ltn) {
				my_description_changed = YES;
				return;
			}

			set_local_topology_node(&tmp, local);

			if ( (bmx_time - self->updated_timestamp) > ((uint32_t)my_topology_period * 10) && (
				check_value_deviation(ltn->txBw, tmp.txBw, 0) ||
				check_value_deviation(ltn->rxBw, tmp.rxBw, 0) ||
				check_value_deviation(ltn->txRate, tmp.txRate, 0) ||
				check_value_deviation(ltn->rxRate, tmp.rxRate, 0) )
				) {

				my_description_changed = YES;
				return;

			} else if (
				check_value_deviation(ltn->txBw, tmp.txBw, my_topology_hysteresis) ||
				check_value_deviation(ltn->rxBw, tmp.rxBw, my_topology_hysteresis) ||
				check_value_deviation(ltn->txRate, tmp.txRate, my_topology_hysteresis) ||
				check_value_deviation(ltn->rxRate, tmp.rxRate, my_topology_hysteresis)
				) {
				my_description_changed = YES;
				return;
			}

			m++;
		}
        }

	if (local_topology_tree.items != m) {
		my_description_changed = YES;
		return;
	}

	task_register(my_topology_period/10, check_local_topology_cache, NULL, -300000);
}

STATIC_FUNC
void destroy_local_topology_cache(void)
{

	struct local_topology_node *ltn;

	while ((ltn = avl_remove_first_item(&local_topology_tree, -300000)))
		debugFree(ltn, -300000);

}

STATIC_FUNC
int create_description_topology(struct tx_frame_iterator *it)
{
        struct avl_node *local_it = NULL;
        struct local_node *local;
        int32_t m = 0;

	struct description_msg_topology *msg = (struct description_msg_topology *) tx_iterator_cache_msg_ptr(it);

	destroy_local_topology_cache();

	if (my_topology_period >= MAX_TOPOLOGY_PERIOD)
		return TLV_TX_DATA_IGNORED;

	task_remove(check_local_topology_cache, NULL);
	task_register(my_topology_period, check_local_topology_cache, NULL, -300000);


        while ((local = avl_iterate_item(&local_tree, &local_it)) && m < tx_iterator_cache_msg_space_max(it)) {

		if (local->neigh && local->neigh->dhn->on && local->best_tp_lndev) {

			struct local_topology_node *ltn = debugMallocReset(sizeof(struct local_topology_node), -300000);

			set_local_topology_node(ltn, local);
			ltn->pkid = local->neigh->dhn->on->global_id.pkid;
			avl_insert(&local_topology_tree, ltn, -300000);

			msg[m].pkid = ltn->pkid;
			msg[m].txBw = umetric_to_fmu8( &ltn->txBw);
			msg[m].rxBw = umetric_to_fmu8(&ltn->rxBw);
			msg[m].txRate = ltn->txRate;
			msg[m].rxRate = ltn->rxRate;

			msg[m].type = 0;
			msg[m].reserved = 0;


			m++;
		}
        }

	if (m)
		return m * sizeof(struct description_msg_topology);

        return TLV_TX_DATA_IGNORED;
}

STATIC_FUNC
int32_t opt_topology(uint8_t cmd, uint8_t _save, struct opt_type *opt, struct opt_parent *patch, struct ctrl_node *cn)
{
        TRACE_FUNCTION_CALL;

	static int32_t scheduled_period = MAX_TOPOLOGY_PERIOD;

        if (!terminating && cmd == OPT_POST && my_topology_period != scheduled_period) {

		task_remove(check_local_topology_cache, NULL);

		if (my_topology_period < MAX_TOPOLOGY_PERIOD)
			task_register(my_topology_period, check_local_topology_cache, NULL, -300000);

		scheduled_period = my_topology_period;
	}

	if (cmd == OPT_UNREGISTER && scheduled_period < MAX_TOPOLOGY_PERIOD) {
		task_remove(check_local_topology_cache, NULL);
		scheduled_period = MAX_TOPOLOGY_PERIOD;
	}

	return SUCCESS;
}

static struct opt_type topology_options[]=
{
//        ord parent long_name          shrt Attributes				*ival		min		max		default		*func,*syntax,*help

	{ODI,0,ARG_TOPOLOGY,	        0, 9,2,A_PS0N,A_USR,A_DYN,A_ARG,A_ANY,	0,		0, 		0,		0,0, 		opt_status,
			0,		"show topology\n"}
	,
	{ODI,0,ARG_TOPOLOGY_HYSTERESIS, 0, 9,2, A_PS1, A_ADM, A_DYI, A_CFA, A_ANY, &my_topology_hysteresis, MIN_TOPOLOGY_HYSTERESIS, MAX_TOPOLOGY_HYSTERESIS, DEF_TOPOLOGY_HYSTERESIS,0, NULL,
			ARG_VALUE_FORM,	"set hysteresis for creating topology (description) updates due to changed local topology statistics"}
	,
	{ODI,0,ARG_TOPOLOGY_PERIOD,     0, 9,2, A_PS1, A_ADM, A_DYI, A_CFA, A_ANY, &my_topology_period, MIN_TOPOLOGY_PERIOD, MAX_TOPOLOGY_PERIOD, DEF_TOPOLOGY_PERIOD,0, opt_topology,
			ARG_VALUE_FORM,	"set min periodicity for creating topology (description) updates due to changed local topology statistics"}
};


static void topology_cleanup( void )
{

	destroy_local_topology_cache();
}



static int32_t topology_init( void ) {

        static const struct field_format topology_format[] = DESCRIPTION_MSG_TOPOLOGY_FORMAT;
        struct frame_handl tlv_handl;

        memset( &tlv_handl, 0, sizeof(tlv_handl));
        tlv_handl.min_msg_size = sizeof (struct description_msg_topology);
        tlv_handl.fixed_msg_size = 1;
        tlv_handl.is_relevant = 0;
        tlv_handl.name = "TOPOLOGY_EXTENSION";
        tlv_handl.tx_frame_handler = create_description_topology;
        tlv_handl.rx_frame_handler = process_description_topology;
        tlv_handl.msg_format = topology_format;
        register_frame_handler(description_tlv_handl, BMX_DSC_TLV_TOPOLOGY, &tlv_handl);
	register_status_handl(sizeof (struct topology_status), 1, topology_status_format, ARG_TOPOLOGY, topology_status_creator);
        register_options_array(topology_options, sizeof ( topology_options), CODE_CATEGORY_NAME);

	return SUCCESS;
}


struct plugin* get_plugin( void ) {
	
	static struct plugin topology_plugin;
	
	memset( &topology_plugin, 0, sizeof ( struct plugin ) );

	topology_plugin.plugin_name = CODE_CATEGORY_NAME;
	topology_plugin.plugin_size = sizeof ( struct plugin );
	topology_plugin.cb_init = topology_init;
	topology_plugin.cb_cleanup = topology_cleanup;

	return &topology_plugin;
}


