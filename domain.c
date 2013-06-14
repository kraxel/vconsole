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
    line = g_strdup_printf("%s%s%s%s%s", domain_state_name(dom),
                           dom->saved   ? ", saved"     : "",
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
    if (!dom->conn->win->vm_logging)
        return;
    if (!dom->stream)
        return;
    if (dom->logfp)
        return;

    dom->logname = g_strdup_printf("%s/vconsole/%s/%s.log",
                                   getenv("HOME"),
                                   virConnectGetHostname(dom->conn->ptr),
                                   dom->name);
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
        fprintf(stderr, "%s: %s\n", __func__, dom->name);
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
                fprintf(stderr, "%s: %s eof\n", __func__, dom->name);
            domain_disconnect(dom, d);
        }
    }
    if (events & VIR_STREAM_EVENT_HANGUP) {
        if (debug)
            fprintf(stderr, "%s: %s hangup\n", __func__, dom->name);
        domain_disconnect(dom, d);
    }
    virDomainFree(d);
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
    int flags = 0;
    int rc;

    if (dom->stream)
        return;

    dom->stream = virStreamNew(dom->conn->ptr,
                               VIR_STREAM_NONBLOCK);
    if (dom->conn->cap_console_force)
        flags |= VIR_DOMAIN_CONSOLE_FORCE;
    rc = virDomainOpenConsole(d, NULL, dom->stream, flags);
    if (rc < 0) {
        if (debug)
            fprintf(stderr, "%s: %s failed\n", __func__, dom->name);
        virStreamFree(dom->stream);
        dom->stream = NULL;
        return;
    }

    virStreamEventAddCallback(dom->stream,
                              VIR_STREAM_EVENT_READABLE |
                              VIR_STREAM_EVENT_HANGUP,
                              domain_console_event, dom, NULL);
    if (debug)
        fprintf(stderr, "%s: %s ok\n", __func__, dom->name);
    domain_log_open(dom);
    domain_update_status(dom);
}

static int domain_update_info(struct vconsole_domain *dom, virDomainPtr d)
{
    struct timeval ts;
    const char *name;
    int id, rc;
    gboolean saved = FALSE;
    virDomainInfo info;

    gettimeofday(&ts, NULL);
    name = virDomainGetName(d);
    id = virDomainGetID(d);
    if (dom->conn->cap_migration)
        saved = virDomainHasManagedSaveImage(d, 0);
    rc = virDomainGetInfo(d, &info);
    if (rc != 0) {
        return rc;
    }

    if (dom->name)
        g_free((gpointer)dom->name);
    dom->last_info = dom->info;
    dom->last_ts   = dom->ts;
    dom->ts        = ts;
    dom->name      = g_strdup(name);
    dom->id        = id;
    dom->saved     = saved;
    dom->info      = info;

    if (dom->last_ts.tv_sec) {
        uint64_t real, cpu;
        real  = (dom->ts.tv_sec - dom->last_ts.tv_sec) * 1000000000;
        real += (dom->ts.tv_usec - dom->last_ts.tv_usec) * 1000;
        cpu   = dom->info.cpuTime - dom->last_info.cpuTime;
        dom->load = cpu * 100 / real;
    }

    domain_update_status(dom);
    return 0;
}

static void domain_update_tree_store(struct vconsole_domain *dom,
                                     GtkTreeIter *guest)
{
    const char *foreground;
    char load[16], mem[16];
    PangoWeight weight;

    switch (dom->info.state) {
    case VIR_DOMAIN_RUNNING:
        foreground = "darkgreen";
        weight = PANGO_WEIGHT_BOLD;
        break;
    default:
        foreground = "black";
        weight = PANGO_WEIGHT_NORMAL;
        break;
    }
    snprintf(load, sizeof(load), "%d%%", dom->load);
    snprintf(mem, sizeof(mem), "%ld M", dom->info.memory / 1024);

    gtk_tree_store_set(dom->conn->win->store, guest,
                       NAME_COL,       dom->name,
                       ID_COL,         dom->id,
                       IS_RUNNING_COL, dom->info.state == VIR_DOMAIN_RUNNING,
                       HAS_MEMCPU_COL, dom->info.state == VIR_DOMAIN_RUNNING,
                       STATE_COL,      domain_state_name(dom),
                       NR_CPUS_COL,    dom->info.nrVirtCpu,
                       LOAD_STR_COL,   load,
                       LOAD_INT_COL,   MIN(dom->load / dom->info.nrVirtCpu, 100),
                       MEMORY_COL,     mem,
                       FOREGROUND_COL, foreground,
                       WEIGHT_COL,     weight,
                       -1);
}

/* ------------------------------------------------------------------ */

void domain_start(struct vconsole_domain *dom)
{
    virDomainPtr d = virDomainLookupByUUIDString(dom->conn->ptr, dom->uuid);
    uint32_t flags = 0;

    domain_update_info(dom, d);
    switch (dom->info.state) {
    case VIR_DOMAIN_SHUTOFF:
        if (dom->vte && dom->conn->cap_start_paused) {
            flags |= VIR_DOMAIN_START_PAUSED;
            dom->unpause = TRUE;
        }
        virDomainCreateWithFlags(d, flags);
        break;
    case VIR_DOMAIN_PAUSED:
        virDomainResume(d);
        break;
    default:
        fprintf(stderr, "%s: invalid guest state: %s\n",
                __func__, domain_state_name(dom));
    }
    virDomainFree(d);
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
    virDomainFree(d);
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
    virDomainFree(d);
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
    virDomainFree(d);
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
    virDomainFree(d);
}

void domain_reset(struct vconsole_domain *dom)
{
    virDomainPtr d = virDomainLookupByUUIDString(dom->conn->ptr, dom->uuid);

    domain_update_info(dom, d);
    switch (dom->info.state) {
    case VIR_DOMAIN_RUNNING:
        virDomainReset(d, 0);
        break;
    default:
        fprintf(stderr, "%s: invalid guest state: %s\n",
                __func__, domain_state_name(dom));
    }
    virDomainFree(d);
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
    virDomainFree(d);
}

void domain_free(struct vconsole_domain *dom)
{
    virDomainPtr d = virDomainLookupByUUIDString(dom->conn->ptr, dom->uuid);

    domain_close_tab(dom, d);
    g_free(dom);
    virDomainFree(d);
}

void domain_update(struct vconsole_connect *conn,
                   virDomainPtr d, virDomainEventType event)
{
    GtkTreeModel *model = GTK_TREE_MODEL(conn->win->store);
    GtkTreeIter host, guest;
    struct vconsole_domain *dom = NULL;
    void *ptr;
    gboolean rc;
    char uuid[VIR_UUID_STRING_BUFLEN];

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
    domain_update_info(dom, d);

    /* update tree store cols */
    domain_update_tree_store(dom, &guest);

    if (dom->unpause && dom->info.state == VIR_DOMAIN_PAUSED) {
        virDomainResume(d);
        dom->unpause = FALSE;
    }
}

void domain_update_all(struct vconsole_window *win)
{
    GtkTreeModel *model = GTK_TREE_MODEL(win->store);
    GtkTreeIter host, guest;
    struct vconsole_connect *conn;
    struct vconsole_domain *dom;
    char mem[16];
    virDomainPtr d;
    unsigned long memory, vcpus;
    int rc, domcount, errcount;

    /* all hosts */
    rc = gtk_tree_model_get_iter_first(model, &host);
    while (rc) {
        gtk_tree_model_get(model, &host,
                           CPTR_COL, &conn,
                           -1);

        memory = 0;
        vcpus = 0;
        domcount = 0;
        errcount = 0;

        /* all guests */
        rc = gtk_tree_model_iter_nth_child(model, &guest, &host, 0);
        while (rc) {
            domcount++;
            gtk_tree_model_get(model, &guest,
                               DPTR_COL, &dom,
                               -1);
            /* update */
            d = virDomainLookupByUUIDString(conn->ptr, dom->uuid);
            if (d == NULL) {
                errcount++;
            } else if (0 != domain_update_info(dom, d)) {
                errcount++;
            }
            if (dom->info.state == VIR_DOMAIN_RUNNING) {
                memory += dom->info.memory;
                vcpus  += dom->info.nrVirtCpu;
            }
            domain_update_tree_store(dom, &guest);
            if (d)
                virDomainFree(d);
            rc = gtk_tree_model_iter_next(model, &guest);
        }
        if (errcount) {
            fprintf(stderr, "%s: %d/%d\n", __func__, errcount, domcount);
        }
        if (errcount && errcount == domcount) {
            /* all domains failed, disconnected ? */
            connect_close(conn->ptr, 0, conn);
            return;
        }

        snprintf(mem, sizeof(mem), "%ld M", memory / 1024);
        gtk_tree_store_set(win->store,     &host,
                           NR_CPUS_COL,    vcpus,
                           MEMORY_COL,     mem,
                           HAS_MEMCPU_COL, (gboolean)(memory > 0),
                           -1);

        rc = gtk_tree_model_iter_next(model, &host);
    }
}

void domain_activate(struct vconsole_domain *dom)
{
    virDomainPtr d = virDomainLookupByUUIDString(dom->conn->ptr, dom->uuid);
    struct vconsole_window *win = dom->conn->win;
    GtkWidget *label, *fstatus;
    gint page;

    if (dom->vte) {
        page = gtk_notebook_page_num(GTK_NOTEBOOK(win->notebook), dom->vbox);
        gtk_notebook_set_current_page(GTK_NOTEBOOK(win->notebook), page);
    } else {
        if (debug)
            fprintf(stderr, "new tab: %s\n", dom->name);

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

        label = gtk_label_new(dom->name);
        page = gtk_notebook_insert_page(GTK_NOTEBOOK(win->notebook),
                                        dom->vbox, label, -1);
        gtk_widget_show_all(dom->vbox);
        gtk_notebook_set_current_page(GTK_NOTEBOOK(win->notebook), page);
        domain_configure_vte(dom);

        domain_update_info(dom, d);
        if (dom->info.state == VIR_DOMAIN_RUNNING)
            domain_connect(dom, d);
    }

    virDomainFree(d);
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
        virDomainFree(d);
    }
}

