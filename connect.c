#include "vconsole.h"

/* ------------------------------------------------------------------ */

static int connect_domain_event(virConnectPtr c, virDomainPtr d,
                                int event, int detail, void *opaque)
{
    struct vconsole_connect *conn = opaque;

    if (debug)
        fprintf(stderr, "%s: %s, event %d\n", __func__,
                virDomainGetName(d), event);
    domain_update(conn, d, event);
    return 0;
}

static void connect_error(void *opaque, virErrorPtr err)
{
    struct vconsole_connect *conn = opaque;
    GtkMessageType type;
    GtkWidget **dialog;

    switch (err->domain) {
    case VIR_FROM_STREAMS:  /* get one on guest shutdown, ignore */
        return;
    default:
        break;
    }

    switch (err->code) {
    case VIR_ERR_NO_DOMAIN:  /* domain is gone, ignore */
        return;
    default:
        break;
    }

    switch (err->level) {
    case VIR_ERR_WARNING:
        type = GTK_MESSAGE_WARNING;
        dialog = &conn->warn;
        break;
    case VIR_ERR_ERROR:
        type = GTK_MESSAGE_ERROR;
        dialog = &conn->err;
        break;
    default:
        type = GTK_MESSAGE_INFO;
        dialog = &conn->info;
        break;
    }
    gtk_message(conn->win->toplevel, dialog, type,
                "%s [ %d / %d ]\n",
                err->message,
                err->code, err->domain);
}

void connect_close(virConnectPtr c, int reason, void *opaque)
{
    struct vconsole_connect *conn = opaque;
    GtkTreeModel *model = GTK_TREE_MODEL(conn->win->store);
    struct vconsole_domain *dom = NULL;
    GtkTreeIter host, guest;
    gboolean rc;
    void *ptr;

    if (debug)
        fprintf(stderr, "%s: reason %d\n", __func__, reason);

    /* find host */
    rc = gtk_tree_model_get_iter_first(model, &host);
    while (rc) {
        gtk_tree_model_get(model, &host,
                           CPTR_COL, &ptr,
                           -1);
        if (ptr == conn)
            break;
        rc = gtk_tree_model_iter_next(model, &host);
    }
    assert(ptr == conn);

    /* free all guests */
    while ((rc = gtk_tree_model_iter_nth_child(model, &guest, &host, 0))) {
        gtk_tree_model_get(model, &guest,
                           DPTR_COL, &dom,
                           -1);
        gtk_tree_store_remove(conn->win->store, &guest);
        domain_free(dom);
    }

    /* free host */
    gtk_tree_store_remove(conn->win->store, &host);
    g_free(conn);
}

static void connect_list(struct vconsole_connect *conn)
{
    virDomainPtr d;
    int i, n;
    char **inactive;
    int *active;

    n = virConnectNumOfDomains(conn->ptr);
    active = malloc(sizeof(int) * n);
    n = virConnectListDomains(conn->ptr, active, n);
    for (i = 0; i < n; i++) {
        d = virDomainLookupByID(conn->ptr, active[i]);
        domain_update(conn, d, -1);
        virDomainFree(d);
    }
    free(active);

    n = virConnectNumOfDefinedDomains(conn->ptr);
    inactive = malloc(sizeof(char *) * n);
    n = virConnectListDefinedDomains(conn->ptr, inactive, n);
    for (i = 0; i < n; i++) {
        d = virDomainLookupByName(conn->ptr, inactive[i]);
        domain_update(conn, d, -1);
        virDomainFree(d);
        free(inactive[i]);
    }
    free(inactive);
}

struct vconsole_connect *connect_init(struct vconsole_window *win,
                                      const char *uri)
{
    struct vconsole_connect *conn;
    GtkTreeIter iter;
    const char *type;
    char *name, *key, *caps;

    conn = g_new0(struct vconsole_connect, 1);
    conn->ptr = virConnectOpen(uri);
    if (conn->ptr == NULL) {
        gtk_message(win->toplevel, NULL, GTK_MESSAGE_ERROR,
                    "Failed to open connection to %s\n", uri);
        g_free(conn);
        return NULL;
    }
    conn->win = win;
    type = virConnectGetType(conn->ptr);
    name = virConnectGetHostname(conn->ptr);
    caps = virConnectGetCapabilities(conn->ptr);
    key = g_strdup_printf("%s:%s", type, name);
    virConnectDomainEventRegister(conn->ptr, connect_domain_event,
                                  conn, NULL);
    virConnSetErrorFunc(conn->ptr, conn, connect_error);
#if LIBVIR_VERSION_NUMBER >= 10000 /* 0.10.0 */
    virConnectRegisterCloseCallback(conn->ptr, connect_close,
                                    conn, NULL);
#endif

    gtk_tree_store_append(win->store, &iter, NULL);
    gtk_tree_store_set(win->store, &iter,
                       CPTR_COL,       conn,
                       NAME_COL,       name,
                       TYPE_COL,       type,
                       URI_COL,        uri,
                       FOREGROUND_COL, win->darkmode ? "white" : "black",
                       WEIGHT_COL,     PANGO_WEIGHT_NORMAL,
                       -1);

    if (debug)
        fprintf(stderr, "%s: %s\n", __func__, uri);
    g_key_file_set_string(config, "hosts", name, uri);
    g_key_file_set_string(config, "connections", key, uri);
    config_write();
    connect_list(conn);

    if (strstr(caps, "<migration_features>")) {
        if (debug)
            fprintf(stderr, "%s: migration supported\n", __func__);
        conn->cap_migration = TRUE;
    }
    if (strcmp(type, "QEMU") == 0) {
        conn->cap_start_paused = TRUE;
        conn->cap_console_force = TRUE;
    }

    free(key);
    free(caps);
    free(name);

    return conn;
}
