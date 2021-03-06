/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2011  Nokia Corporation
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <glib.h>

#include <bluetooth/uuid.h>

#include <readline/readline.h>
#include <readline/history.h>

#include "att.h"
#include "btio.h"
#include "gattrib.h"
#include "gatt.h"
#include "gatttool.h"

static GIOChannel *iochannel = NULL;
static GAttrib *attrib = NULL;
static GMainLoop *event_loop;
static GString *prompt;

static gchar *opt_src = NULL;
static gchar *opt_dst = NULL;
static gchar *opt_dst_type = NULL;
static gchar *opt_sec_level = NULL;
static int opt_psm = 0;
static int opt_mtu = 0;
static uint16_t conn_handle = 0;
static int start;
static int end;


struct characteristic_data {
    uint16_t orig_start;
    uint16_t start;
    uint16_t end;
    bt_uuid_t uuid;
};

static void cmd_help(int argcp, char **argvp);

enum state {
    STATE_DISCONNECTED,
    STATE_CONNECTING,
    STATE_CONNECTED
} conn_state;

static char *get_prompt(void)
{
    if (conn_state == STATE_CONNECTING) {
        g_string_assign(prompt, "\nConnecting...\n");
        return prompt->str;
    }

    if (conn_state == STATE_CONNECTED)
        g_string_assign(prompt, "\n[CON]");
    else
        g_string_assign(prompt, "\n[   ]");

    if (opt_dst)
        g_string_append_printf(prompt, "[%17s]", opt_dst);
    else
        g_string_append_printf(prompt, "[%17s]", "");

    if (opt_psm)
        g_string_append(prompt, "[BR]\n");
    else
        g_string_append(prompt, "[LE]\n");

    g_string_append(prompt, "> ");

    return prompt->str;
}


static void set_state(enum state st)
{
    conn_state = st;
    if (conn_state != STATE_CONNECTED){
        conn_handle = 0;
    }
    rl_on_new_line();
    rl_set_prompt(get_prompt());
    rl_redisplay();
}

static void events_handler(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
    uint8_t *opdu;
    uint16_t handle, i, olen;
    size_t plen;

    handle = att_get_u16(&pdu[1]);

    printf("\n");
    switch (pdu[0]) {
    case ATT_OP_HANDLE_NOTIFY:
        printf("NOTIFICATION(%04x): %04x ", conn_handle , handle);
        break;
    case ATT_OP_HANDLE_IND:
        printf("INDICATION(%04x): %04x ", conn_handle, handle);
        break;
    default:
        printf("ERROR(%04x): (16,256) Invalid opcode\n", conn_handle);
        rl_forced_update_display();
        return;
    }

    for (i = 3; i < len; i++)
        printf("%02x ", pdu[i]);

    printf("\n");
    rl_forced_update_display();

    if (pdu[0] == ATT_OP_HANDLE_NOTIFY)
        return;

    opdu = g_attrib_get_buffer(attrib, &plen);
    olen = enc_confirmation(opdu, plen);

    if (olen > 0)
        g_attrib_send(attrib, 0, opdu[0], opdu, olen, NULL, NULL, NULL);
}

static void connect_cb(GIOChannel *io, GError *err, gpointer user_data)
{
    if (err) {
        set_state(STATE_DISCONNECTED);
        printf("\nCONNECTED(%04x): %s %i %s\n", conn_handle, opt_dst,
               err->code, err->message);
        rl_forced_update_display();
        return;
    }

    attrib = g_attrib_new(iochannel);
    g_attrib_register(attrib, ATT_OP_HANDLE_NOTIFY, events_handler,
                            attrib, NULL);
    g_attrib_register(attrib, ATT_OP_HANDLE_IND, events_handler,
                            attrib, NULL);
    
    GError *gerr = NULL;

    // get connection handle
    bt_io_get(iochannel, &gerr, BT_IO_OPT_HANDLE, &conn_handle,
              BT_IO_OPT_INVALID);
    if (gerr){
        printf("CONNECTED(%04x): %s %i %s\n", conn_handle, opt_dst, 
               gerr->code, gerr->message);
        conn_handle = 0;
        rl_forced_update_display();
        return;
    }

    printf("\nCONNECTED(%04x): %s 0\n", conn_handle, opt_dst);
    set_state(STATE_CONNECTED);
}

static void disconnect_io()
{
    if (conn_state == STATE_DISCONNECTED)
        return;

    g_attrib_unref(attrib);
    attrib = NULL;
    opt_mtu = 0;

    g_io_channel_shutdown(iochannel, FALSE, NULL);
    g_io_channel_unref(iochannel);
    iochannel = NULL;

    printf("\nDISCONNECTED(%04X): %s\n", conn_handle, opt_dst);

    set_state(STATE_DISCONNECTED);
}

static void primary_all_cb(GSList *services, guint8 status, gpointer user_data)
{
    GSList *l;

    if (status) {
        printf("\nPRIMARY-ALL-END(%04x): %i %s\n", conn_handle, status, 
               att_ecode2str(status));
        rl_forced_update_display();
        return;
    }

    printf("\n");
    for (l = services; l; l = l->next) {
        struct gatt_primary *prim = l->data;
        printf("PRIMARY-ALL(%04x): %04x %04x %s\n", conn_handle, 
               prim->range.start, prim->range.end, prim->uuid);
    }
    printf("PRIMARY-ALL-END(%04x): 0\n", conn_handle);

    rl_forced_update_display();
}

static void primary_by_uuid_cb(GSList *ranges, guint8 status,
                            gpointer user_data)
{
    GSList *l;

    if (status) {
        printf("PRIMARY-UUID-END(%04x): %i %s\n", conn_handle, status, 
               att_ecode2str(status));
        rl_forced_update_display();
        return;
    }

    printf("\n");
    for (l = ranges; l; l = l->next) {
        struct att_range *range = l->data;
        printf("PRIMARY-UUID(%04x): %04x %04x\n", conn_handle, range->start, 
               range->end);
    }
    printf("PRIMARY-UUID-END(%04x): 0\n", conn_handle);

    rl_forced_update_display();
}

static void char_cb(GSList *characteristics, guint8 status, gpointer user_data)
{
    GSList *l;

    if (status) {
        printf("CHAR-END(%04x): %i %s\n", conn_handle, status, att_ecode2str(status));
        rl_forced_update_display();
        return;
    }

    printf("\n");
    for (l = characteristics; l; l = l->next) {
        struct gatt_char *chars = l->data;

        printf("CHAR(%04x): %04x %02x %04x %s\n", conn_handle, chars->handle,
               chars->properties, chars->value_handle, chars->uuid);
    }
    printf("CHAR-END(%04x): 0\n", conn_handle, opt_dst);

    rl_forced_update_display();
}

static void char_desc_cb(guint8 status, const guint8 *pdu, guint16 plen,
                            gpointer user_data)
{
    struct att_data_list *list;
    guint8 format;
    uint16_t handle = 0xffff;
    int i;

    if (status != 0) {
        printf("CHAR-DESC-END(%04x): %i %s\n", conn_handle, status, 
               att_ecode2str(status));
        rl_forced_update_display();
        return;
    }

    list = dec_find_info_resp(pdu, plen, &format);
    if (list != NULL) {

        printf("\n");
        for (i = 0; i < list->num; i++) {
            char uuidstr[MAX_LEN_UUID_STR];
            uint8_t *value;
            bt_uuid_t uuid;
            
            value = list->data[i];
            handle = att_get_u16(value);

            if (format == 0x01)
                uuid = att_get_uuid16(&value[2]);
            else
                uuid = att_get_uuid128(&value[2]);

            bt_uuid_to_string(&uuid, uuidstr, MAX_LEN_UUID_STR);
            printf("CHAR-DESC(%04x): %04x %s\n", conn_handle, handle, uuidstr);
        }
    }
    printf("CHAR-DESC-END(%04x): 0\n", conn_handle);

    att_data_list_free(list);

    if (handle != 0xffff && handle < end)
        gatt_find_info(attrib, handle + 1, end, char_desc_cb, NULL);
    else
        rl_forced_update_display();
}

static void char_read_cb(guint8 status, const guint8 *pdu, guint16 plen,
                            gpointer user_data)
{
    uint8_t value[ATT_MAX_MTU];
    ssize_t vlen;
    int i;

    if (status != 0) {
        printf("\nCHAR-VAL-DESC(%04x): %i %s\n", conn_handle, status, 
               att_ecode2str(status));
        rl_forced_update_display();
        return;
    }

    vlen = dec_read_resp(pdu, plen, value, sizeof(value));
    if (vlen < 0) {
        status = ATT_ECODE_INVALID_PDU;
        printf("\nCHAR-VAL-DESC(%04x): %i %s\n", conn_handle, status,
               att_ecode2str(status));
        rl_forced_update_display();
        return;
    }

    printf("\nCHAR-VAL-DESC(%04x): 0 ", conn_handle);
    for (i = 0; i < vlen; i++)
        printf("%02x ", value[i]);
    printf("\n");

    rl_forced_update_display();
}

static void char_read_by_uuid_cb(guint8 status, const guint8 *pdu,
                    guint16 plen, gpointer user_data)
{
    struct characteristic_data *char_data = user_data;
    struct att_data_list *list;
    int i;

    if (status == ATT_ECODE_ATTR_NOT_FOUND &&
                char_data->start != char_data->orig_start)
        goto done;

    if (status != 0) {
        printf("CHAR-READ-UUID-END(%04x): %i %s\n", conn_handle, status, 
               att_ecode2str(status));
        goto done;
    }

    list = dec_read_by_type_resp(pdu, plen);
    if (list == NULL)
        goto done;

    for (i = 0; i < list->num; i++) {
        uint8_t *value = list->data[i];
        int j;

        char_data->start = att_get_u16(value) + 1;

        printf("\nCHAR-READ-UUID(%04x): %04x ", conn_handle, att_get_u16(value));
        value += 2;
        for (j = 0; j < list->len - 2; j++, value++)
            printf("%02x ", *value);
        printf("\n");
    }
    printf("CHAR-READ-UUID-END(%04x): 0\n", conn_handle);

    att_data_list_free(list);

done:
    rl_forced_update_display();
    g_free(char_data);
}

static void cmd_exit(int argcp, char **argvp)
{
    rl_callback_handler_remove();
    g_main_loop_quit(event_loop);
}

static gboolean channel_watcher(GIOChannel *chan, GIOCondition cond,
                gpointer user_data)
{
    printf("\nDISCONNECTED(%04x): %s\n", conn_handle, opt_dst);
    disconnect_io();
    rl_forced_update_display();
    return FALSE;
}

static void cmd_connect(int argcp, char **argvp)
{
    if (conn_state != STATE_DISCONNECTED)
        return;

    if (argcp > 1) {
        g_free(opt_dst);
        opt_dst = g_strdup(argvp[1]);

        g_free(opt_dst_type);
        if (argcp > 2)
            opt_dst_type = g_strdup(argvp[2]);
        else
            opt_dst_type = g_strdup("public");
    }

    if (opt_dst == NULL) {
        printf("\nCONNECT(0000): 1 00:00:00:00:00:00 "
               "Remote Bluetooth address required\n");
        rl_forced_update_display();
        return;
    }

    set_state(STATE_CONNECTING);
    iochannel = gatt_connect(opt_src, opt_dst, opt_dst_type, opt_sec_level,
                        opt_psm, opt_mtu, connect_cb);
    if (iochannel == NULL)
        set_state(STATE_DISCONNECTED);
    else
        g_io_add_watch(iochannel, G_IO_HUP, channel_watcher, NULL);
}

static void cmd_disconnect(int argcp, char **argvp)
{
    disconnect_io();
}

static void cmd_primary(int argcp, char **argvp)
{
    bt_uuid_t uuid;

    if (conn_state != STATE_CONNECTED) {
        if (argcp)
            printf("\nPRIMARY-ALL(0000): 256 Command failed: disconnected\n");
        else
            printf("\nPRIMARY-UUID(0000): 256 Command failed: disconnected\n");
        rl_forced_update_display();
        return;
    }

    if (argcp == 1) {
        gatt_discover_primary(attrib, NULL, primary_all_cb, NULL);
        rl_forced_update_display();
        return;
    }

    if (bt_string_to_uuid(&uuid, argvp[1]) < 0) {
        printf("\nPRIMARY-UUID(%04x): 1 Invalid UUID\n", conn_handle);
        rl_forced_update_display();
        return;
    }

    gatt_discover_primary(attrib, &uuid, primary_by_uuid_cb, NULL);
}

static int strtohandle(const char *src)
{
    char *e;
    int dst;

    errno = 0;
    dst = strtoll(src, &e, 16);
    if (errno != 0 || *e != '\0')
        return -EINVAL;

    return dst;
}

static void cmd_char(int argcp, char **argvp)
{
    int start = 0x0001;
    int end = 0xffff;

    if (conn_state != STATE_CONNECTED) {
        printf("\nCHAR-DESC-END(0000): 256 disconnected\n");
        rl_forced_update_display();
        return;
    }

    if (argcp > 1) {
        start = strtohandle(argvp[1]);
        if (start < 0) {
            printf("\nCHAR-DESC-END(%04x): %i Invalid start handle: %s\n", 
                   conn_handle, ATT_ECODE_INVALID_HANDLE, argvp[1]);
            rl_forced_update_display();
            return;
        }
    }

    if (argcp > 2) {
        end = strtohandle(argvp[2]);
        if (end < 0) {
            printf("\nCHAR-DESC-END(%04x): %i Invalid end handle: %s\n", 
                   conn_handle, ATT_ECODE_INVALID_HANDLE, argvp[2]);
            rl_forced_update_display();
            return;
        }
    }

    if (argcp > 3) {
        bt_uuid_t uuid;

        if (bt_string_to_uuid(&uuid, argvp[3]) < 0) {
            printf("\nCHAR-DESC-END(%04x): %i Invalid UUID\n", 
                   conn_handle, ATT_ECODE_UNLIKELY);
            rl_forced_update_display();
            return;
        }

        gatt_discover_char(attrib, start, end, &uuid, char_cb, NULL);
        return;
    }

    gatt_discover_char(attrib, start, end, NULL, char_cb, NULL);
}

static void cmd_char_desc(int argcp, char **argvp)
{
    if (conn_state != STATE_CONNECTED) {
        printf("\nCHAR-DESC-END(0000): 256 Command failed: disconnected\n");
        rl_forced_update_display();
        return;
    }

    if (argcp > 1) {
        start = strtohandle(argvp[1]);
        if (start < 0) {
            printf("\nCHAR-DESC-END(%04x): %i Invalid start handle: %s\n", 
                   conn_handle, ATT_ECODE_INVALID_HANDLE, argvp[1]);
            rl_forced_update_display();
            return;
        }
    } else
        start = 0x0001;

    if (argcp > 2) {
        end = strtohandle(argvp[2]);
        if (end < start) {
            printf("\nCHAR-DESC-END(%04x): %i Invalid end handle: %s\n\n", 
                   conn_handle, ATT_ECODE_INVALID_HANDLE, argvp[2]);
            rl_forced_update_display();
            return;
        }
    } else
        end = 0xffff;

    gatt_find_info(attrib, start, end, char_desc_cb, NULL);
}

static void cmd_read_hnd(int argcp, char **argvp)
{
    int handle;
    int offset = 0;

    if (conn_state != STATE_CONNECTED) {
        printf("\nCHAR-READ-HND(0000): 256 Command failed: disconnected\n");
        rl_forced_update_display();
        return;
    }

    if (argcp < 2) {
        printf("\nCHAR-READ-HND(%04x): 1 Missing argument: handle\n", 
               conn_handle);
        rl_forced_update_display();
        return;
    }

    handle = strtohandle(argvp[1]);
    if (handle < 0) {
        printf("\nCHAR-READ-HND(%04x): 1 Invalid handle: %s\n", 
               conn_handle, argvp[1]);
        rl_forced_update_display();
        return;
    }

    if (argcp > 2) {
        char *e;

        errno = 0;
        offset = strtol(argvp[2], &e, 0);
        if (errno != 0 || *e != '\0') {
            printf("\nCHAR-READ-HND(%04x): %i Invalid offset: %s\n", 
                   conn_handle, ATT_ECODE_INVALID_OFFSET, argvp[2]);
            rl_forced_update_display();
            return;
        }
    }

    gatt_read_char(attrib, handle, offset, char_read_cb, attrib);
}

static void cmd_read_uuid(int argcp, char **argvp)
{
    struct characteristic_data *char_data;
    int start = 0x0001;
    int end = 0xffff;
    bt_uuid_t uuid;

    if (conn_state != STATE_CONNECTED) {
        printf("\nCHAR-READ-UUID(0000): 256 Command failed: disconnected\n");
        rl_forced_update_display();
        return;
    }

    if (argcp < 2) {
        printf("\nCHAR-READ-UUID(%04x): 1 Missing argument: UUID\n", 
               conn_handle);
        rl_forced_update_display();
        return;
    }

    if (bt_string_to_uuid(&uuid, argvp[1]) < 0) {
        printf("\nCHAR-READ-UUID(%04x): 1 Invalid UUID\n", 
               conn_handle);
        rl_forced_update_display();
        return;
    }

    if (argcp > 2) {
        start = strtohandle(argvp[2]);
        if (start < 0) {
            printf("\nCHAR-READ-UUID(%04x): %i Invalid start handle: %s\n", 
                   conn_handle, ATT_ECODE_INVALID_HANDLE, argvp[1]);
            rl_forced_update_display();
            return;
        }
    }

    if (argcp > 3) {
        end = strtohandle(argvp[3]);
        if (end < start) {
            printf("\nCHAR-READ-UUID(%04x): %i Invalid end handle: %s\n", 
                   conn_handle, ATT_ECODE_INVALID_HANDLE, argvp[2]);
            rl_forced_update_display();
            return;
        }
    }

    char_data = g_new(struct characteristic_data, 1);
    char_data->orig_start = start;
    char_data->start = start;
    char_data->end = end;
    char_data->uuid = uuid;

    gatt_read_char_by_uuid(attrib, start, end, &char_data->uuid,
                    char_read_by_uuid_cb, char_data);
}

static void char_write_req_cb(guint8 status, const guint8 *pdu, guint16 plen,
                            gpointer user_data)
{
    if (status != 0) {
        printf("\nCHAR-WRITE-REQ(%04x): %i %s\n", conn_handle, status,
               att_ecode2str(status));
        rl_forced_update_display();
        return;
    }

    if (!dec_write_resp(pdu, plen) && !dec_exec_write_resp(pdu, plen))
        printf("\nCHAR-WRITE-REQ(%04x): 1\n", conn_handle);
    else 
        printf("\nCHAR-WRITE-REQ(%04x): 0\n", conn_handle);
    rl_forced_update_display();
}

static void cmd_char_write(int argcp, char **argvp)
{
    uint8_t *value;
    size_t plen;
    int handle;
    
    if (argcp < 3) {
        printf("\nCHAR-WRITE-(%04x): 257 Usage: %s <handle> <new value>\n", 
               conn_handle, argvp[0]);
        rl_forced_update_display();
        return;
    }

    int mode = g_strcmp0("char-write-req", argvp[0]) == 0;

    if (conn_state != STATE_CONNECTED) {
        if (mode > 0)
            printf("\nCHAR-WRITE-REQ");
        else
            printf("\nCHAR-WRITE-CMD");
        printf("(000): 256 Command failed: disconnected\n");
        rl_forced_update_display();
        return;
    }


    handle = strtohandle(argvp[1]);
    if (handle <= 0) {
        if (mode > 0)
            printf("\nCHAR-WRITE-REQ");
        else
            printf("\nCHAR-WRITE-CMD");

        printf("(%04x): %i A valid handle is required\n", conn_handle,
               ATT_ECODE_INVALID_HANDLE);
        rl_forced_update_display();
        return;
    }

    plen = gatt_attr_data_from_string(argvp[2], &value);
    if (plen == 0) {
        if (mode > 0)
            printf("\nCHAR-WRITE-REQ");
        else
            printf("\nCHAR-WRITE-CMD");

        printf("(%04x): %i invalid value\n", conn_handle,
               ATT_ECODE_INVALID_HANDLE);
        rl_forced_update_display();
        return;
    }

    if (g_strcmp0("char-write-req", argvp[0]) == 0)
        gatt_write_char(attrib, handle, value, plen,
                    char_write_req_cb, NULL);
    else {
        gatt_write_char(attrib, handle, value, plen, NULL, NULL);
        printf("\nCHAR-WRITE-CMD(%04x): 0\n", conn_handle); 
        // let other end know we sent the request
        rl_forced_update_display();
        return;
    }

    g_free(value);
}

static void cmd_sec_level(int argcp, char **argvp)
{
    GError *gerr = NULL;
    BtIOSecLevel sec_level;

    if (argcp < 2) {
        printf("\nSEC-LEVEL(%04x): 0 %s\n", conn_handle, opt_sec_level);
        rl_forced_update_display();
        return;
    }

    if (strcasecmp(argvp[1], "medium") == 0)
        sec_level = BT_IO_SEC_MEDIUM;
    else if (strcasecmp(argvp[1], "high") == 0)
        sec_level = BT_IO_SEC_HIGH;
    else if (strcasecmp(argvp[1], "low") == 0)
        sec_level = BT_IO_SEC_LOW;
    else {
        printf("\nSEC-LEVEL(%04x): 257 Allowed values: low | medium | high\n", 
               conn_handle);
        rl_forced_update_display();
        return;
    }

    g_free(opt_sec_level);
    opt_sec_level = g_strdup(argvp[1]);

    if (!opt_psm && conn_state != STATE_CONNECTED){
        printf("\nSEC-LEVEL(0000): 256 It can only be done when connected"
               " for LE connections\n");
        rl_forced_update_display();
        return;
    }

    if (opt_psm && conn_state != STATE_DISCONNECTED) {
        printf("\nSEC-LEVEL(%04x): 256 It must be disconnected to this "
               "change take effect\n", conn_handle);
        rl_forced_update_display();
    }
    
    if ( iochannel != NULL ){
        bt_io_set(iochannel, &gerr,
              BT_IO_OPT_SEC_LEVEL, sec_level,
              BT_IO_OPT_INVALID);
        if (gerr) {
            printf("\nSEC-LEVEL(%04x): %i %s\n", conn_handle, gerr->code, 
                   gerr->message);
            g_error_free(gerr);
            rl_forced_update_display();
            return;
        }
    }

    printf("\nSEC-LEVEL(%04x): 0 %s\n", conn_handle, opt_sec_level);
    rl_forced_update_display();
}

static void exchange_mtu_cb(guint8 status, const guint8 *pdu, guint16 plen,
                            gpointer user_data)
{
    uint16_t mtu;

    if (status != 0) {
        printf("\nMTU(%04x): %i %s\n", conn_handle, status, 
               att_ecode2str(status));
        rl_forced_update_display();
        return;
    }

    if (!dec_mtu_resp(pdu, plen, &mtu)) {
        printf("\nMTU(%04x): %i Protocol error\n", conn_handle,
               ATT_ECODE_INVALID_PDU);
        rl_forced_update_display();
        return;
    }

    mtu = MIN(mtu, opt_mtu);
    /* Set new value for MTU in client */
    if (g_attrib_set_mtu(attrib, mtu))
        printf("\nMTU(%04x): 0\n", conn_handle);
    else
        printf("\nMTU(%04x): 129 Error exchanging MTU\n", conn_handle);
    rl_forced_update_display();
}

static void cmd_mtu(int argcp, char **argvp)
{
    if (conn_state != STATE_CONNECTED) {
        printf("\nMTU(0000): 256 Command failed: not connected.\n");
        rl_forced_update_display();
        return;
    }

    if (opt_psm) {
        printf("\nMTU(%04x): 256 Command failed: operation is only available"
               " for LE transport.\n", conn_handle);
        rl_forced_update_display();
        return;
    }

    if (argcp < 2) {
        printf("\nMTU(%04x): 257 Usage: mtu <value>\n", conn_handle);
        rl_forced_update_display();
        return;
    }

    if (opt_mtu) {
        printf("\nMTU(%04x): %i Command failed: MTU exchange can only occur"
               " once per connection.\n", conn_handle, ATT_ECODE_UNLIKELY);
        rl_forced_update_display();
        return;
    }

    errno = 0;
    opt_mtu = strtoll(argvp[1], NULL, 0);
    if (errno != 0 || opt_mtu < ATT_DEFAULT_LE_MTU) {
        printf("\nMTU(%04x): %i Invalid value. Minimum MTU size is %d\n",
               conn_handle, ATT_ECODE_UNLIKELY, ATT_DEFAULT_LE_MTU);
        rl_forced_update_display();
        return;
    }

    gatt_exchange_mtu(attrib, opt_mtu, exchange_mtu_cb, NULL);
}

static void cmd_psm(int argcp, char **argvp)
{
    if (conn_state == STATE_CONNECTED) {
        printf("\nPSM(%04x): 256 Command failed: connected.\n", conn_handle);
        rl_forced_update_display();
        return;
    }

    if (argcp < 2) {
        printf("\nPSM(0000): 257 Usage: psm <value>\n");
        rl_forced_update_display();
        return;
    }

    errno = 0;
    opt_psm = strtoll(argvp[1], NULL, 0);
    
    printf("\nPSM(0000): %i\n", opt_psm);
    rl_forced_update_display();
}

static struct {
    const char *cmd;
    void (*func)(int argcp, char **argvp);
    const char *params;
    const char *desc;
} commands[] = {
    { "help",       cmd_help,   "",
        "Show this help"},
    { "exit",       cmd_exit,   "",
        "Exit interactive mode" },
    { "quit",       cmd_exit,   "",
        "Exit interactive mode" },
    { "connect",        cmd_connect,    "[address [address type]]",
        "Connect to a remote device" },
    { "disconnect",     cmd_disconnect, "",
        "Disconnect from a remote device" },
    { "primary",        cmd_primary,    "[UUID]",
        "Primary Service Discovery" },
    { "characteristics",    cmd_char,   "[start hnd [end hnd [UUID]]]",
        "Characteristics Discovery" },
    { "char-desc",      cmd_char_desc,  "[start hnd] [end hnd]",
        "Characteristics Descriptor Discovery" },
    { "char-read-hnd",  cmd_read_hnd,   "<handle> [offset]",
        "Characteristics Value/Descriptor Read by handle" },
    { "char-read-uuid", cmd_read_uuid,  "<UUID> [start hnd] [end hnd]",
        "Characteristics Value/Descriptor Read by UUID" },
    { "char-write-req", cmd_char_write, "<handle> <new value>",
        "Characteristic Value Write (Write Request)" },
    { "char-write-cmd", cmd_char_write, "<handle> <new value>",
        "Characteristic Value Write (No response)" },
    { "sec-level",      cmd_sec_level,  "[low | medium | high]",
        "Set security level. Default: low" },
    { "mtu",        cmd_mtu,    "<value>",
        "Exchange MTU for GATT/ATT" },
    { "psm",        cmd_psm,    "<value>",
      "Set PSM for GATT/ATT over BR"},
    { NULL, NULL, NULL}
};

static void cmd_help(int argcp, char **argvp)
{
    int i;

    for (i = 0; commands[i].cmd; i++)
        printf("%-15s %-30s %s\n", commands[i].cmd,
                commands[i].params, commands[i].desc);
}

static void parse_line(char *line_read)
{
    gchar **argvp;
    int argcp;
    int i;

    if (line_read == NULL) {
        printf("\n");
        cmd_exit(0, NULL);
        return;
    }

    line_read = g_strstrip(line_read);

    if (*line_read == '\0')
        return;

    add_history(line_read);

    g_shell_parse_argv(line_read, &argcp, &argvp, NULL);

    for (i = 0; commands[i].cmd; i++)
        if (strcasecmp(commands[i].cmd, argvp[0]) == 0)
            break;

    if (commands[i].cmd)
        commands[i].func(argcp, argvp);
    else
        printf("\nERROR(15,256): %s: command not found\n", argvp[0]);

    g_strfreev(argvp);
}

static gboolean prompt_read(GIOChannel *chan, GIOCondition cond,
                            gpointer user_data)
{
    if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
        g_io_channel_unref(chan);
        return FALSE;
    }

    rl_callback_read_char();

    return TRUE;
}

static char *completion_generator(const char *text, int state)
{
    static int index = 0, len = 0;
    const char *cmd = NULL;

    if (state == 0) {
        index = 0;
        len = strlen(text);
    }

    while ((cmd = commands[index].cmd) != NULL) {
        index++;
        if (strncmp(cmd, text, len) == 0)
            return strdup(cmd);
    }

    return NULL;
}

static char **commands_completion(const char *text, int start, int end)
{
    if (start == 0)
        return rl_completion_matches(text, &completion_generator);
    else
        return NULL;
}

int interactive(const gchar *src, const gchar *dst,
        const gchar *dst_type, int psm)
{
    GIOChannel *pchan;
    gint events;

    opt_sec_level = g_strdup("low");

    opt_src = g_strdup(src);
    opt_dst = g_strdup(dst);
    opt_dst_type = g_strdup(dst_type);
    opt_psm = psm;

    prompt = g_string_new(NULL);

    event_loop = g_main_loop_new(NULL, FALSE);

    pchan = g_io_channel_unix_new(fileno(stdin));
    g_io_channel_set_close_on_unref(pchan, TRUE);
    events = G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL;
    g_io_add_watch(pchan, events, prompt_read, NULL);

    rl_attempted_completion_function = commands_completion;
    rl_callback_handler_install(get_prompt(), parse_line);

    g_main_loop_run(event_loop);

    rl_callback_handler_remove();
    cmd_disconnect(0, NULL);
    g_io_channel_unref(pchan);
    g_main_loop_unref(event_loop);
    g_string_free(prompt, TRUE);

    g_free(opt_src);
    g_free(opt_dst);
    g_free(opt_sec_level);

    return 0;
}
