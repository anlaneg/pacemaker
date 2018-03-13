/* 
 * Copyright (C) 2004 Andrew Beekhof <andrew@beekhof.net>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <crm_internal.h>

#include <unistd.h>  /* pid_t, sleep, ssize_t */

#include <crm/cib.h>
#include <crm/cluster.h>
#include <crm/common/xml.h>
#include <crm/crm.h>
#include <crm/msg_xml.h>

#include <crmd.h>
#include <crmd_fsa.h>
#include <crmd_messages.h>  /* register_fsa_error_adv */

static mainloop_io_t *pe_subsystem = NULL;

/*!
 * \internal
 * \brief Close any PE connection and free associated memory
 */
void
pe_subsystem_free(void)
{
    if (pe_subsystem) {
        mainloop_del_ipc_client(pe_subsystem);
        pe_subsystem = NULL;
    }
}

/*!
 * \internal
 * \brief Save CIB query result to file, raising FSA error
 *
 * \param[in] msg        Ignored
 * \param[in] call_id    Call ID of CIB query
 * \param[in] rc         Return code of CIB query
 * \param[in] output     Result of CIB query
 * \param[in] user_data  Unique identifier for filename (will be freed)
 *
 * \note This is intended to be called after a PE connection fails.
 */
static void
save_cib_contents(xmlNode *msg, int call_id, int rc, xmlNode *output,
                  void *user_data)
{
    char *id = user_data;

    register_fsa_error_adv(C_FSA_INTERNAL, I_ERROR, NULL, NULL, __FUNCTION__);
    CRM_CHECK(id != NULL, return);

    if (rc == pcmk_ok) {
        char *filename = crm_strdup_printf(PE_STATE_DIR "/pe-core-%s.bz2", id);

        if (write_xml_file(output, filename, TRUE) < 0) {
            crm_err("Could not save Cluster Information Base to %s after Policy Engine crash",
                    filename);
        } else {
            crm_notice("Saved Cluster Information Base to %s after Policy Engine crash",
                       filename);
        }
        free(filename);
    }
}

/*!
 * \internal
 * \brief Respond to PE connection failure
 *
 * \param[in] user_data  Ignored
 */
static void
pe_ipc_destroy(gpointer user_data)
{
    if (is_set(fsa_input_register, R_PE_REQUIRED)) {
        int rc = pcmk_ok;
        char *uuid_str = crm_generate_uuid();

        crm_crit("Connection to the Policy Engine failed "
                 CRM_XS " uuid=%s", uuid_str);

        /*
         * The PE died...
         *
         * Save the current CIB so that we have a chance of
         * figuring out what killed it.
         *
         * Delay raising the I_ERROR until the query below completes or
         * 5s is up, whichever comes first.
         *
         */
        rc = fsa_cib_conn->cmds->query(fsa_cib_conn, NULL, NULL, cib_scope_local);
        fsa_register_cib_callback(rc, FALSE, uuid_str, save_cib_contents);

    } else {
        crm_info("Connection to the Policy Engine released");
    }

    clear_bit(fsa_input_register, R_PE_CONNECTED);
    pe_subsystem = NULL;
    mainloop_set_trigger(fsa_source);
    return;
}

/*!
 * \internal
 * \brief Handle message from PE connection
 *
 * \param[in] buffer    XML message (will be freed)
 * \param[in] length    Ignored
 * \param[in] userdata  Ignored
 *
 * \return 0
 */
static int
pe_ipc_dispatch(const char *buffer, ssize_t length, gpointer userdata)
{
    xmlNode *msg = string2xml(buffer);

    if (msg) {
        route_message(C_IPC_MESSAGE, msg);
    }
    free_xml(msg);
    return 0;
}

/*!
 * \internal
 * \brief Make new connection to PE
 *
 * \return TRUE on success, FALSE otherwise
 */
static bool
pe_subsystem_new()
{
    static struct ipc_client_callbacks pe_callbacks = {
        .dispatch = pe_ipc_dispatch,
        .destroy = pe_ipc_destroy
    };

    pe_subsystem = mainloop_add_ipc_client(CRM_SYSTEM_PENGINE,
                                           G_PRIORITY_DEFAULT,
                                           5 * 1024 * 1024 /* 5MB */,
                                           NULL, &pe_callbacks);
    return (pe_subsystem != NULL);
}

/*!
 * \internal
 * \brief Send an XML message to the PE
 *
 * \param[in] cmd  XML message to send
 *
 * \return pcmk_ok on success, -errno otherwise
 */
static int
pe_subsystem_send(xmlNode *cmd)
{
    if (pe_subsystem) {
        int sent = crm_ipc_send(mainloop_get_ipc_client(pe_subsystem), cmd,
                                0, 0, NULL);

        if (sent == 0) {
            sent = -ENODATA;
        } else if (sent > 0) {
            sent = pcmk_ok;
        }
        return sent;
    }
    return -ENOTCONN;
}

static void do_pe_invoke_callback(xmlNode *msg, int call_id, int rc,
                                  xmlNode *output, void *user_data);

/*	 A_PE_START, A_PE_STOP, O_PE_RESTART	*/
void
do_pe_control(long long action,
              enum crmd_fsa_cause cause,
              enum crmd_fsa_state cur_state,
              enum crmd_fsa_input current_input, fsa_data_t * msg_data)
{
    if (action & A_PE_STOP) {
        clear_bit(fsa_input_register, R_PE_REQUIRED);
        pe_subsystem_free();
        clear_bit(fsa_input_register, R_PE_CONNECTED);
    }

    if ((action & A_PE_START) && (is_set(fsa_input_register, R_PE_CONNECTED) == FALSE)) {
        if (cur_state != S_STOPPING) {
            set_bit(fsa_input_register, R_PE_REQUIRED);
            if (pe_subsystem_new()) {
                set_bit(fsa_input_register, R_PE_CONNECTED);
            } else {
                crm_warn("Could not connect to Policy Engine");
                register_fsa_error(C_FSA_INTERNAL, I_FAIL, NULL);
            }
        } else {
            crm_info("Ignoring request to connect to PE while shutting down");
        }
    }
}

int fsa_pe_query = 0;
char *fsa_pe_ref = NULL;

/*	 A_PE_INVOKE	*/
void
do_pe_invoke(long long action,
             enum crmd_fsa_cause cause,
             enum crmd_fsa_state cur_state,
             enum crmd_fsa_input current_input, fsa_data_t * msg_data)
{
    if (AM_I_DC == FALSE) {
        crm_err("Not invoking Policy Engine because not DC: %s",
                fsa_action2string(action));
        return;
    }

    if (is_set(fsa_input_register, R_PE_CONNECTED) == FALSE) {
        if (is_set(fsa_input_register, R_SHUTDOWN)) {
            crm_err("Cannot shut down gracefully without the Policy Engine");
            register_fsa_input_before(C_FSA_INTERNAL, I_TERMINATE, NULL);

        } else {
            crm_info("Waiting for the Policy Engine to connect");
            crmd_fsa_stall(FALSE);
            register_fsa_action(A_PE_START);
        }
        return;
    }

    if (cur_state != S_POLICY_ENGINE) {
        crm_notice("Not invoking Policy Engine because in state %s",
                   fsa_state2string(cur_state));
        return;
    }
    if (is_set(fsa_input_register, R_HAVE_CIB) == FALSE) {
        crm_err("Attempted to invoke Policy Engine without consistent Cluster Information Base!");

        /* start the join from scratch */
        register_fsa_input_before(C_FSA_INTERNAL, I_ELECTION, NULL);
        return;
    }

    fsa_pe_query = fsa_cib_conn->cmds->query(fsa_cib_conn, NULL, NULL, cib_scope_local);

    crm_debug("Query %d: Requesting the current CIB: %s", fsa_pe_query,
              fsa_state2string(fsa_state));

    /* Make sure any queued calculations are discarded */
    free(fsa_pe_ref);
    fsa_pe_ref = NULL;

    fsa_register_cib_callback(fsa_pe_query, FALSE, NULL, do_pe_invoke_callback);
}

static void
force_local_option(xmlNode *xml, const char *attr_name, const char *attr_value)
{
    int max = 0;
    int lpc = 0;
    char *xpath_string = NULL;
    xmlXPathObjectPtr xpathObj = NULL;

    xpath_string = crm_strdup_printf("%.128s//%s//nvpair[@name='%.128s']",
                                     get_object_path(XML_CIB_TAG_CRMCONFIG),
                                     XML_CIB_TAG_PROPSET, attr_name);
    xpathObj = xpath_search(xml, xpath_string);
    max = numXpathResults(xpathObj);
    free(xpath_string);

    for (lpc = 0; lpc < max; lpc++) {
        xmlNode *match = getXpathResult(xpathObj, lpc);
        crm_trace("Forcing %s/%s = %s", ID(match), attr_name, attr_value);
        crm_xml_add(match, XML_NVPAIR_ATTR_VALUE, attr_value);
    }

    if(max == 0) {
        xmlNode *configuration = NULL;
        xmlNode *crm_config = NULL;
        xmlNode *cluster_property_set = NULL;

        crm_trace("Creating %s-%s for %s=%s",
                  CIB_OPTIONS_FIRST, attr_name, attr_name, attr_value);

        configuration = find_entity(xml, XML_CIB_TAG_CONFIGURATION, NULL);
        if (configuration == NULL) {
            configuration = create_xml_node(xml, XML_CIB_TAG_CONFIGURATION);
        }

        crm_config = find_entity(configuration, XML_CIB_TAG_CRMCONFIG, NULL);
        if (crm_config == NULL) {
            crm_config = create_xml_node(configuration, XML_CIB_TAG_CRMCONFIG);
        }

        cluster_property_set = find_entity(crm_config, XML_CIB_TAG_PROPSET, NULL);
        if (cluster_property_set == NULL) {
            cluster_property_set = create_xml_node(crm_config, XML_CIB_TAG_PROPSET);
            crm_xml_add(cluster_property_set, XML_ATTR_ID, CIB_OPTIONS_FIRST);
        }

        xml = create_xml_node(cluster_property_set, XML_CIB_TAG_NVPAIR);

        crm_xml_set_id(xml, "%s-%s", CIB_OPTIONS_FIRST, attr_name);
        crm_xml_add(xml, XML_NVPAIR_ATTR_NAME, attr_name);
        crm_xml_add(xml, XML_NVPAIR_ATTR_VALUE, attr_value);
    }
    freeXpathObject(xpathObj);
}

static void
do_pe_invoke_callback(xmlNode * msg, int call_id, int rc, xmlNode * output, void *user_data)
{
    xmlNode *cmd = NULL;
    pid_t watchdog = pcmk_locate_sbd();

    if (rc != pcmk_ok) {
        crm_err("Could not retrieve the Cluster Information Base: %s "
                CRM_XS " call=%d", pcmk_strerror(rc), call_id);
        register_fsa_error_adv(C_FSA_INTERNAL, I_ERROR, NULL, NULL, __FUNCTION__);
        return;

    } else if (call_id != fsa_pe_query) {
        crm_trace("Skipping superseded CIB query: %d (current=%d)", call_id, fsa_pe_query);
        return;

    } else if (AM_I_DC == FALSE || is_set(fsa_input_register, R_PE_CONNECTED) == FALSE) {
        crm_debug("No need to invoke the PE anymore");
        return;

    } else if (fsa_state != S_POLICY_ENGINE) {
        crm_debug("Discarding PE request in state: %s", fsa_state2string(fsa_state));
        return;

    /* this callback counts as 1 */
    } else if (num_cib_op_callbacks() > 1) {
        crm_debug("Re-asking for the CIB: %d other peer updates still pending",
                  (num_cib_op_callbacks() - 1));
        sleep(1);
        register_fsa_action(A_PE_INVOKE);
        return;

    } else if (fsa_state != S_POLICY_ENGINE) {
        crm_err("Invoking PE in state: %s", fsa_state2string(fsa_state));
        return;
    }

    CRM_LOG_ASSERT(output != NULL);

    /* refresh our remote-node cache when the pengine is invoked */
    crm_remote_peer_cache_refresh(output);

    crm_xml_add(output, XML_ATTR_DC_UUID, fsa_our_uuid);
    crm_xml_add_int(output, XML_ATTR_HAVE_QUORUM, fsa_has_quorum);

    force_local_option(output, XML_ATTR_HAVE_WATCHDOG, watchdog?"true":"false");

    if (ever_had_quorum && crm_have_quorum == FALSE) {
        crm_xml_add_int(output, XML_ATTR_QUORUM_PANIC, 1);
    }

    cmd = create_request(CRM_OP_PECALC, output, NULL, CRM_SYSTEM_PENGINE, CRM_SYSTEM_DC, NULL);

    free(fsa_pe_ref);
    fsa_pe_ref = crm_element_value_copy(cmd, XML_ATTR_REFERENCE);

    rc = pe_subsystem_send(cmd);
    if (rc < 0) {
        crm_err("Could not contact the Policy Engine: %s " CRM_XS " rc=%d",
                pcmk_strerror(rc), rc);
        register_fsa_error_adv(C_FSA_INTERNAL, I_ERROR, NULL, NULL, __FUNCTION__);
    }
    crm_debug("Invoking the PE: query=%d, ref=%s, seq=%llu, quorate=%d",
              fsa_pe_query, fsa_pe_ref, crm_peer_seq, fsa_has_quorum);
    free_xml(cmd);
}
