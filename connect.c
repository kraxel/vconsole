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

#if LIBVIR_VERSION_NUMBER >= 10000 /* 0.10.0 */

static void connect_close(virConnectPtr c, int reason, void *opaque)
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

#endif

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
    char *name;

    conn = g_new0(struct vconsole_connect, 1);
    conn->ptr = virConnectOpen(uri);
    if (conn->ptr == NULL) {
        fprintf(stderr, "Failed to open connection to %s\n", uri);
        g_free(conn);
        return NULL;
    }
    conn->win = win;
    name = virConnectGetHostname(conn->ptr);
    virConnectDomainEventRegister(conn->ptr, connect_domain_event,
                                  conn, NULL);
#if LIBVIR_VERSION_NUMBER >= 10000 /* 0.10.0 */
    virConnectRegisterCloseCallback(conn->ptr, connect_close,
                                    conn, NULL);
#endif

    gtk_tree_store_append(win->store, &iter, NULL);
    gtk_tree_store_set(win->store, &iter,
                       CPTR_COL,       conn,
                       NAME_COL,       name,
                       URI_COL,        uri,
                       FOREGROUND_COL, "black",
                       WEIGHT_COL,     PANGO_WEIGHT_NORMAL,
                       -1);

    if (debug)
        fprintf(stderr, "%s: %s\n", __func__, uri);
    g_key_file_set_string(config, "hosts", name, uri);
    config_write();
    connect_list(conn);

    free(name);

    return conn;
}
