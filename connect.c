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

static void connect_list(struct vconsole_connect *conn)
{
    int i, n;
    char **inactive;
    int *active;

    n = virConnectNumOfDomains(conn->ptr);
    active = malloc(sizeof(int) * n);
    n = virConnectListDomains(conn->ptr, active, n);
    for (i = 0; i < n; i++) {
        domain_update(conn, virDomainLookupByID(conn->ptr, active[i]), -1);
    }
    free(active);

    n = virConnectNumOfDefinedDomains(conn->ptr);
    inactive = malloc(sizeof(char *) * n);
    n = virConnectListDefinedDomains(conn->ptr, inactive, n);
    for (i = 0; i < n; i++) {
        domain_update(conn, virDomainLookupByName(conn->ptr, inactive[i]), -1);
        free(inactive[i]);
    }
    free(inactive);
}

struct vconsole_connect *connect_init(struct vconsole_window *win,
                                      const char *uri)
{
    struct vconsole_connect *conn;
    GtkTreeIter iter;
    char *name;

    conn = g_new0(struct vconsole_connect, 1);
    conn->ptr = virConnectOpen(uri);
    name = virConnectGetHostname(conn->ptr);
    if (conn->ptr == NULL) {
        fprintf(stderr, "Failed to open connection to %s\n", uri);
        g_free(conn);
        return NULL;
    }
    conn->win = win;
    virConnectDomainEventRegister(conn->ptr, connect_domain_event,
                                  conn, NULL);

    gtk_tree_store_append(win->store, &iter, NULL);
    gtk_tree_store_set(win->store, &iter,
                       CPTR_COL, conn,
                       NAME_COL, name,
                       URI_COL,  uri,
                       -1);

    if (debug)
        fprintf(stderr, "%s: %s\n", __func__, uri);
    g_key_file_set_string(config, "hosts", name, uri);
    config_write();
    connect_list(conn);

    return conn;
}
