#include "vconsole.h"

/* ------------------------------------------------------------------ */

static const char *state_name[] = {
    [ VIR_DOMAIN_NOSTATE ]     = "-",
    [ VIR_DOMAIN_RUNNING ]     = "running",
    [ VIR_DOMAIN_BLOCKED ]     = "blocked",
    [ VIR_DOMAIN_PAUSED ]      = "paused",
    [ VIR_DOMAIN_SHUTDOWN ]    = "shutdown",
    [ VIR_DOMAIN_SHUTOFF ]     = "shutoff",
    [ VIR_DOMAIN_CRASHED ]     = "crashed",
#ifdef VIR_DOMAIN_PMSUSPENDED
    [ VIR_DOMAIN_PMSUSPENDED ] = "suspended",
#endif
};

static const char *domain_state_name(struct vconsole_domain *dom)
{
    if (dom->info.state < sizeof(state_name)/sizeof(state_name[0]))
        return state_name[dom->info.state];
    return "-?-";
}

static void domain_update_status(struct vconsole_domain *dom)
{
    char *line;

    if (!dom->status)
        return;
    line = g_strdup_printf("%s%s%s%s", domain_state_name(dom),
                           dom->stream  ? ", connected" : "",
                           dom->logname ? ", log "      : "",
                           dom->logname ? dom->logname  : "");
    gtk_label_set_text(GTK_LABEL(dom->status), line);
    g_free(line);
}

/* ------------------------------------------------------------------ */

static void domain_foreach(struct vconsole_window *win,
                           void (*func)(struct vconsole_domain *dom))
{
    GtkTreeModel *model = GTK_TREE_MODEL(win->store);
    GtkTreeIter host, guest;
    struct vconsole_domain *dom;
    int rc;

    if (model == NULL)
        return;

    rc = gtk_tree_model_get_iter_first(model, &host);
    while (rc) {
        rc = gtk_tree_model_iter_nth_child(model, &guest, &host, 0);
        while (rc) {
            gtk_tree_model_get(model, &guest,
                               DPTR_COL, &dom,
                               -1);
            func(dom);
            rc = gtk_tree_model_iter_next(model, &guest);
        }
        rc = gtk_tree_model_iter_next(model, &host);
    }
}

static void domain_configure_vte(struct vconsole_domain *dom)
{
    struct vconsole_window *win = dom->conn->win;
    VteTerminal *vte = VTE_TERMINAL(dom->vte);
    VteTerminalCursorBlinkMode bl =
        win->tty_blink ? VTE_CURSOR_BLINK_ON : VTE_CURSOR_BLINK_OFF;
    GdkColor fg = {0,0,0,0};
    GdkColor bg = {0,0,0,0};

    if (!dom->vte)
        return;

    gdk_color_parse(win->tty_fg, &fg);
    gdk_color_parse(win->tty_bg, &bg);

    vte_terminal_set_font_from_string(vte, win->tty_font);
    vte_terminal_set_cursor_blink_mode(vte, bl);
    vte_terminal_set_color_foreground(vte, &fg);
    vte_terminal_set_color_background(vte, &bg);
}

static void make_dirs(const char *filename)
{
    char *dirname = g_strdup(filename);
    char *slash = strrchr(dirname, '/');

    if (!slash)
        return;
    if (slash == dirname)
        return;
    *slash = 0;
    if (mkdir(dirname, 0777) < 0) {
        if (errno != ENOENT)
            goto err;
        make_dirs(dirname);
        if (mkdir(dirname, 0777) < 0)
            goto err;
    }
    return;

err:
    fprintf(stderr, "mkdir %s: %s\n", dirname, strerror(errno));
    g_free(dirname);
    return;
}

static void domain_log_open(struct vconsole_domain *dom)
{
    virDomainPtr d;

    if (!dom->conn->win->vm_logging)
        return;
    if (!dom->stream)
        return;
    if (dom->logfp)
        return;

    d = virDomainLookupByUUIDString(dom->conn->ptr, dom->uuid);
    dom->logname = g_strdup_printf("%s/vconsole/%s/%s.log",
                                   getenv("HOME"),
                                   virConnectGetHostname(dom->conn->ptr),
                                   virDomainGetName(d));
    dom->logfp = fopen(dom->logname, "a");
    if (dom->logfp == NULL) {
        if (errno != ENOENT)
            goto err;
        make_dirs(dom->logname);
        dom->logfp = fopen(dom->logname, "a");
        if (dom->logfp == NULL)
            goto err;
    }
    setbuf(dom->logfp, NULL);  /* unbuffered please */
    fprintf(dom->logfp, "*** vconsole: log opened ***\n");
    return;

err:
    fprintf(stderr, "open %s: %s\n", dom->logname, strerror(errno));
    g_free(dom->logname);
    dom->logname = NULL;
}

static void domain_log_close(struct vconsole_domain *dom)
{
    if (!dom->logfp)
        return;
    fprintf(dom->logfp, "\n*** vconsole: closing log ***\n");
    fclose(dom->logfp);
    dom->logfp = NULL;
    g_free(dom->logname);
    dom->logname = NULL;
}

static void domain_configure_logging(struct vconsole_domain *dom)
{
    gboolean logging = dom->conn->win->vm_logging;

    if (!logging)
        domain_log_close(dom);
    else
        domain_log_open(dom);
    domain_update_status(dom);
}

void domain_configure_all_vtes(struct vconsole_window *win)
{
    domain_foreach(win, domain_configure_vte);
}

void domain_configure_all_logging(struct vconsole_window *win)
{
    domain_foreach(win, domain_configure_logging);
}

static void domain_disconnect(struct vconsole_domain *dom, virDomainPtr d)
{
    if (!dom->stream)
        return;

    if (debug)
        fprintf(stderr, "%s: %s\n", __func__, virDomainGetName(d));
    virStreamEventRemoveCallback(dom->stream);
    virStreamFree(dom->stream);
    dom->stream = NULL;
    domain_log_close(dom);
    domain_update_status(dom);
}

static void domain_close_tab(struct vconsole_domain *dom, virDomainPtr d)
{
    GtkNotebook *notebook = GTK_NOTEBOOK(dom->conn->win->notebook);
    gint page;

    domain_disconnect(dom, d);
    if (!dom->vbox)
        return;
    page = gtk_notebook_page_num(notebook, dom->vbox);
    gtk_notebook_remove_page(notebook, page);
    dom->vbox = NULL;
    dom->vte = NULL;
    dom->status = NULL;
}

static void domain_console_event(virStreamPtr stream, int events, void *opaque)
{
    struct vconsole_domain *dom = opaque;
    virDomainPtr d = virDomainLookupByUUIDString(dom->conn->ptr, dom->uuid);
    const char *name = virDomainGetName(d);
    char buf[128];
    int rc, bytes = 0;

    if (events & VIR_STREAM_EVENT_READABLE) {
        for (;;) {
            rc = virStreamRecv(stream, buf, sizeof(buf));
            if (rc <= 0)
                break;
            bytes += rc;
            if (dom->vte)
                vte_terminal_feed(VTE_TERMINAL(dom->vte), buf, rc);
            if (dom->logfp)
                fwrite(buf, rc, 1, dom->logfp);
        }
        if (bytes == 0) {
            if (debug)
                fprintf(stderr, "%s: %s eof\n", __func__, name);
            domain_disconnect(dom, d);
        }
    }
    if (events & VIR_STREAM_EVENT_HANGUP) {
        if (debug)
            fprintf(stderr, "%s: %s hangup\n", __func__, name);
        domain_disconnect(dom, d);
    }
}

static void domain_user_input(VteTerminal *vte, gchar *buf, guint len,
                              gpointer opaque)
{
    struct vconsole_domain *dom = opaque;

    if (dom->stream) {
        virStreamSend(dom->stream, buf, len);
        return;
    }
    domain_start(dom);
}

static void domain_connect(struct vconsole_domain *dom, virDomainPtr d)
{
    int rc;

    if (dom->stream)
        return;

    dom->stream = virStreamNew(dom->conn->ptr,
                               VIR_STREAM_NONBLOCK);
    rc = virDomainOpenConsole(d, NULL, dom->stream,
                              VIR_DOMAIN_CONSOLE_FORCE);
    if (rc < 0) {
        if (debug)
            fprintf(stderr, "%s: %s failed\n", __func__, virDomainGetName(d));
        virStreamFree(dom->stream);
        dom->stream = NULL;
        return;
    }

    virStreamEventAddCallback(dom->stream,
                              VIR_STREAM_EVENT_READABLE |
                              VIR_STREAM_EVENT_HANGUP,
                              domain_console_event, dom, NULL);
    if (debug)
        fprintf(stderr, "%s: %s ok\n", __func__, virDomainGetName(d));
    domain_log_open(dom);
    domain_update_status(dom);
}

static void domain_update_info(struct vconsole_domain *dom, virDomainPtr d)
{
    virDomainGetInfo(d, &dom->info);
}

/* ------------------------------------------------------------------ */

void domain_start(struct vconsole_domain *dom)
{
    virDomainPtr d = virDomainLookupByUUIDString(dom->conn->ptr, dom->uuid);

    domain_update_info(dom, d);
    switch (dom->info.state) {
    case VIR_DOMAIN_SHUTOFF:
        virDomainCreate(d);
        break;
    case VIR_DOMAIN_PAUSED:
        virDomainResume(d);
        break;
    default:
        fprintf(stderr, "%s: invalid guest state: %s\n",
                __func__, domain_state_name(dom));
    }
}

void domain_pause(struct vconsole_domain *dom)
{
    virDomainPtr d = virDomainLookupByUUIDString(dom->conn->ptr, dom->uuid);

    domain_update_info(dom, d);
    switch (dom->info.state) {
    case VIR_DOMAIN_RUNNING:
        virDomainSuspend(d);
        break;
    default:
        fprintf(stderr, "%s: invalid guest state: %s\n",
                __func__, domain_state_name(dom));
    }
}

void domain_save(struct vconsole_domain *dom)
{
    virDomainPtr d = virDomainLookupByUUIDString(dom->conn->ptr, dom->uuid);

    domain_update_info(dom, d);
    switch (dom->info.state) {
    case VIR_DOMAIN_RUNNING:
    case VIR_DOMAIN_PAUSED:
        virDomainManagedSave(d, 0);
        break;
    default:
        fprintf(stderr, "%s: invalid guest state: %s\n",
                __func__, domain_state_name(dom));
    }
}

void domain_reboot(struct vconsole_domain *dom)
{
    virDomainPtr d = virDomainLookupByUUIDString(dom->conn->ptr, dom->uuid);

    domain_update_info(dom, d);
    switch (dom->info.state) {
    case VIR_DOMAIN_RUNNING:
        virDomainReboot(d, 0);
        break;
    default:
        fprintf(stderr, "%s: invalid guest state: %s\n",
                __func__, domain_state_name(dom));
    }
}

void domain_shutdown(struct vconsole_domain *dom)
{
    virDomainPtr d = virDomainLookupByUUIDString(dom->conn->ptr, dom->uuid);

    domain_update_info(dom, d);
    switch (dom->info.state) {
    case VIR_DOMAIN_RUNNING:
        virDomainShutdown(d);
        break;
    default:
        fprintf(stderr, "%s: invalid guest state: %s\n",
                __func__, domain_state_name(dom));
    }
}

void domain_kill(struct vconsole_domain *dom)
{
    virDomainPtr d = virDomainLookupByUUIDString(dom->conn->ptr, dom->uuid);

    domain_update_info(dom, d);
    switch (dom->info.state) {
    case VIR_DOMAIN_RUNNING:
        virDomainDestroy(d);
        break;
    default:
        fprintf(stderr, "%s: invalid guest state: %s\n",
                __func__, domain_state_name(dom));
    }
}

void domain_free(struct vconsole_domain *dom)
{
    virDomainPtr d = virDomainLookupByUUIDString(dom->conn->ptr, dom->uuid);

    domain_close_tab(dom, d);
    g_free(dom);
}

void domain_update(struct vconsole_connect *conn,
                   virDomainPtr d, virDomainEventType event)
{
    GtkTreeModel *model = GTK_TREE_MODEL(conn->win->store);
    GtkTreeIter host, guest;
    struct vconsole_domain *dom = NULL;
    void *ptr;
    gboolean rc;
    const char *name;
    char uuid[VIR_UUID_STRING_BUFLEN];
    char idstr[16];
    int id;

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

    /* find guest */
    virDomainGetUUIDString(d, uuid);
    rc = gtk_tree_model_iter_nth_child(model, &guest, &host, 0);
    while (rc) {
        gtk_tree_model_get(model, &guest,
                           DPTR_COL, &dom,
                           -1);
        if (strcmp(uuid, dom->uuid) == 0)
            break;
        dom = NULL;
        rc = gtk_tree_model_iter_next(model, &guest);
    }

    /* no guest found -> create new */
    if (dom == NULL) {
        dom = g_new0(struct vconsole_domain, 1);
        dom->conn = conn;
        virDomainGetUUIDString(d, dom->uuid);
        gtk_tree_store_append(conn->win->store, &guest, &host);
        gtk_tree_store_set(conn->win->store, &guest,
                           DPTR_COL, dom, -1);
    }

    /* handle events */
    switch (event) {
    case VIR_DOMAIN_EVENT_UNDEFINED:
        gtk_tree_store_remove(conn->win->store, &guest);
        domain_free(dom);
        return;
    case VIR_DOMAIN_EVENT_STARTED:
        domain_connect(dom, d);
        break;
    case VIR_DOMAIN_EVENT_STOPPED:
        domain_disconnect(dom, d);
        break;
    default:
        break;
    }

    /* update guest info */
    name = virDomainGetName(d);
    id = virDomainGetID(d);
    if (id < 0)
        strcpy(idstr, "-");
    else
        snprintf(idstr, sizeof(idstr), "%d", id);
    domain_update_info(dom, d);
    gtk_tree_store_set(conn->win->store, &guest,
                       NAME_COL,  name,
                       ID_COL,    idstr,
                       STATE_COL, domain_state_name(dom),
                       -1);
    domain_update_status(dom);
}

void domain_activate(struct vconsole_domain *dom)
{
    virDomainPtr d = virDomainLookupByUUIDString(dom->conn->ptr, dom->uuid);
    struct vconsole_window *win = dom->conn->win;
    GtkWidget *label, *fstatus;
    const char *name;
    gint page;

    if (dom->vte) {
        page = gtk_notebook_page_num(GTK_NOTEBOOK(win->notebook), dom->vbox);
        gtk_notebook_set_current_page(GTK_NOTEBOOK(win->notebook), page);
    } else {
        name = virDomainGetName(d);
        if (debug)
            fprintf(stderr, "new tab: %s\n", name);

        dom->vte = vte_terminal_new();
        g_signal_connect(dom->vte, "commit",
                         G_CALLBACK(domain_user_input), dom);
        vte_terminal_set_scrollback_lines(VTE_TERMINAL(dom->vte), 9999);

        dom->status = gtk_label_new("-");
        gtk_misc_set_alignment(GTK_MISC(dom->status), 0, 0.5);
        gtk_misc_set_padding(GTK_MISC(dom->status), 3, 1);

        dom->vbox = gtk_vbox_new(FALSE, 1);
        gtk_container_set_border_width(GTK_CONTAINER(dom->vbox), 1);
        gtk_box_pack_start(GTK_BOX(dom->vbox), dom->vte, TRUE, TRUE, 0);
        fstatus = gtk_frame_new(NULL);
        gtk_box_pack_end(GTK_BOX(dom->vbox), fstatus, FALSE, TRUE, 0);
        gtk_container_add(GTK_CONTAINER(fstatus), dom->status);

        label = gtk_label_new(name);
        page = gtk_notebook_insert_page(GTK_NOTEBOOK(win->notebook),
                                        dom->vbox, label, -1);
        gtk_widget_show_all(dom->vbox);
        gtk_notebook_set_current_page(GTK_NOTEBOOK(win->notebook), page);
        domain_configure_vte(dom);
        domain_update_status(dom);
    }

    domain_connect(dom, d);
}

struct vconsole_domain *domain_find_current_tab(struct vconsole_window *win)
{
    GtkTreeModel *model = GTK_TREE_MODEL(win->store);
    GtkTreeIter host, guest;
    struct vconsole_domain *dom;
    int rc, cpage, dpage;

    cpage = gtk_notebook_get_current_page(GTK_NOTEBOOK(win->notebook));
    rc = gtk_tree_model_get_iter_first(model, &host);
    while (rc) {
        rc = gtk_tree_model_iter_nth_child(model, &guest, &host, 0);
        while (rc) {
            gtk_tree_model_get(model, &guest,
                               DPTR_COL, &dom,
                               -1);
            if (dom->vbox) {
                dpage = gtk_notebook_page_num(GTK_NOTEBOOK(win->notebook), dom->vbox);
                if (dpage == cpage)
                    return dom;
            }
            rc = gtk_tree_model_iter_next(model, &guest);
        }
        rc = gtk_tree_model_iter_next(model, &host);
    }
    return NULL;
}

void domain_close_current_tab(struct vconsole_window *win)
{
    struct vconsole_domain *dom;
    virDomainPtr d;

    dom = domain_find_current_tab(win);
    if (dom) {
        d = virDomainLookupByUUIDString(dom->conn->ptr, dom->uuid);
        domain_close_tab(dom, d);
    }
}

