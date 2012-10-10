#include "vconsole.h"
#include "libvirt-glib-event.h"

#define APPNAME "vconsole"

/* ------------------------------------------------------------------ */

int debug = 0;
GKeyFile *config;

/* ------------------------------------------------------------------ */

static char *config_file;

static void config_read(void)
{
    char *home = getenv("HOME");
    GError *err = NULL;

    if (!home)
        return;
    config_file = g_strdup_printf("%s/.vconsole", home);
    config = g_key_file_new();
    g_key_file_load_from_file(config, config_file,
                              G_KEY_FILE_KEEP_COMMENTS, &err);
}

void config_write(void)
{
    char *data;
    gsize len;
    GError *err = NULL;
    int fd;

    data = g_key_file_to_data(config, &len, &err);
    fd = open(config_file, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (-1 == fd)
        return;
    write(fd, data, len);
    fsync(fd);
    close(fd);
    g_free(data);
}

/* ------------------------------------------------------------------ */

static char *gtk_msg_type_name[] = {
    [ GTK_MESSAGE_INFO ]     = "INFO",
    [ GTK_MESSAGE_WARNING ]  = "WARNING",
    [ GTK_MESSAGE_QUESTION ] = "QUESTION",
    [ GTK_MESSAGE_ERROR ]    = "ERROR",
};

int gtk_message(GtkWidget *window, GtkMessageType type, char *fmt, ...)
{
    va_list args;
    GtkWidget *dialog;
    char msgbuf[1024];
    int rc;

    va_start(args, fmt);
    rc = vsnprintf(msgbuf, sizeof(msgbuf), fmt, args);
    va_end(args);

    if (debug)
	fprintf(stderr, "%s: %s", gtk_msg_type_name[type], msgbuf);
    dialog = gtk_message_dialog_new(GTK_WINDOW(window),
				    GTK_DIALOG_DESTROY_WITH_PARENT,
				    type, GTK_BUTTONS_CLOSE,
				    "%s", gtk_msg_type_name[type]);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
                                             "%s", msgbuf);
    g_signal_connect_swapped(dialog, "response",
			     G_CALLBACK (gtk_widget_destroy),
			     dialog);
    gtk_widget_show_all(dialog);
    return rc;
}

static int gtk_getstring(GtkWidget *window, char *title, char *message,
                         char *dest, int dlen)
{
    GtkWidget *dialog, *label, *entry, *vbox;
    const char *txt;
    int retval;

    /* Create the widgets */
    dialog = gtk_dialog_new_with_buttons(title,
                                         GTK_WINDOW(window),
                                         GTK_DIALOG_DESTROY_WITH_PARENT,
                                         GTK_STOCK_OK,
                                         GTK_RESPONSE_ACCEPT,
                                         GTK_STOCK_CANCEL,
                                         GTK_RESPONSE_REJECT,
                                         NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);

    label = gtk_label_new(message);
    gtk_misc_set_alignment(GTK_MISC(label), 0, 0.5);

    entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), dest);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);

    vbox = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_add(GTK_CONTAINER(vbox), label);
    gtk_container_add(GTK_CONTAINER(vbox), entry);
    gtk_box_set_spacing(GTK_BOX(vbox), 10);

    /* show and wait for response */
    gtk_widget_show_all(dialog);
    switch (gtk_dialog_run(GTK_DIALOG(dialog))) {
    case GTK_RESPONSE_ACCEPT:
        txt = gtk_entry_get_text(GTK_ENTRY(entry));
        snprintf(dest, dlen, "%s", txt);
        retval = 0;
        break;
    default:
        retval = -1;
        break;
    }
    gtk_widget_destroy(dialog);
    return retval;
}

/* ------------------------------------------------------------------ */

static void menu_cb_connect_ask(GtkAction *action, gpointer userdata)
{
    struct vconsole_window *win = userdata;
    char uri[256] = "";
    int rc;

    rc = gtk_getstring(win->toplevel, "Connect to host", "libvirt uri",
                       uri, sizeof(uri));
    if (rc == 0 && strlen(uri))
        connect_init(win, uri);
}

static void menu_cb_connect_menu(GtkAction *action, gpointer userdata)
{
    struct vconsole_window *win = userdata;
    GError *err = NULL;
    char name[128];
    char *uri;

    if (1 != sscanf(gtk_action_get_name(action), "ConnectMenu_%127s", name))
	return;
    if (debug)
        fprintf(stderr, "%s: %s\n", __func__, name);
    uri = g_key_file_get_string(config, "hosts", name, &err);
    if (uri) {
        connect_init(win, uri);
        g_free(uri);
    }
}

static void menu_cb_close_tab(GtkAction *action, gpointer userdata)
{
    struct vconsole_window *win = userdata;
    domain_close_current_tab(win);
}

static void menu_cb_close_app(GtkAction *action, gpointer userdata)
{
    struct vconsole_window *win = userdata;
    gtk_widget_destroy(win->toplevel);
}

static void menu_cb_config_font(GtkAction *action, void *data)
{
    struct vconsole_window *win = data;
    GtkWidget *dialog;

    dialog = gtk_font_selection_dialog_new("Terminal font");
    if (win->tty_font)
        gtk_font_selection_dialog_set_font_name
            (GTK_FONT_SELECTION_DIALOG(dialog), win->tty_font);

    gtk_widget_show_all(dialog);
    switch (gtk_dialog_run(GTK_DIALOG(dialog))) {
    case GTK_RESPONSE_OK:
        win->tty_font = gtk_font_selection_dialog_get_font_name
	    (GTK_FONT_SELECTION_DIALOG(dialog));
	g_key_file_set_string(config, "tty", "font", win->tty_font);
        config_write();
        domain_configure_all_vtes(win);
	break;
    }
    gtk_widget_destroy(dialog);
}

static int pickcolor(char *title, char *group, char *key, char *current)
{
    GtkWidget *dialog;
    GdkColor color = {0,0,0,0};
    GtkColorSelection *csel;
    char name[16];
    int rc = -1;

    gdk_color_parse(current, &color);
    dialog = gtk_color_selection_dialog_new(title);
    csel = GTK_COLOR_SELECTION(gtk_color_selection_dialog_get_color_selection
                               (GTK_COLOR_SELECTION_DIALOG(dialog)));
    gtk_color_selection_set_has_opacity_control(csel, FALSE);
    gtk_color_selection_set_current_color(csel, &color);

    gtk_widget_show_all(dialog);
    switch (gtk_dialog_run(GTK_DIALOG(dialog))) {
    case GTK_RESPONSE_OK:
	gtk_color_selection_get_current_color(csel, &color);
        snprintf(name, sizeof(name), "#%04x%04x%04x",
                 color.red, color.green, color.blue);
        g_key_file_set_string(config, group, key, name);
        config_write();
	rc = 0;
    }
    gtk_widget_destroy(dialog);
    return rc;
}

static void menu_cb_config_fg(GtkAction *action, void *data)
{
    struct vconsole_window *win = data;
    GError *err = NULL;

    if (0 != pickcolor("Terminal text color", "tty", "foreground", win->tty_fg))
	return;
    win->tty_fg = g_key_file_get_string(config, "tty", "foreground", &err);
    domain_configure_all_vtes(win);
}

static void menu_cb_config_bg(GtkAction *action, void *data)
{
    struct vconsole_window *win = data;
    GError *err = NULL;

    if (0 != pickcolor("Terminal background", "tty", "background", win->tty_bg))
	return;
    win->tty_bg = g_key_file_get_string(config, "tty", "background", &err);
    domain_configure_all_vtes(win);
}

static void menu_cb_fullscreen(GtkToggleAction *action, gpointer userdata)
{
    struct vconsole_window *win = userdata;
    gboolean active;

    active = gtk_toggle_action_get_active(action);
    if (active) {
        gtk_window_fullscreen(GTK_WINDOW(win->toplevel));
    } else {
        gtk_window_unfullscreen(GTK_WINDOW(win->toplevel));
    }
}

static struct vconsole_domain *find_guest(struct vconsole_window *win)
{
    struct vconsole_domain *dom = NULL;
    GtkTreeSelection *select;
    GtkTreeIter iter;
    GtkTreeModel *model;

    if (gtk_notebook_get_current_page(GTK_NOTEBOOK(win->notebook)) == 0) {
        select = gtk_tree_view_get_selection(GTK_TREE_VIEW(win->tree));
        if (gtk_tree_selection_get_selected(select, &model, &iter)) {
            gtk_tree_model_get(model, &iter, DPTR_COL, &dom, -1);
        }
    } else {
        dom = domain_find_current_tab(win);
    }
    return dom;
}

static void menu_cb_vm_gfx(GtkAction *action, void *data)
{
    struct vconsole_window *win = data;
    struct vconsole_domain *dom = find_guest(win);
    char *uri;

    if (!dom)
        return;
    uri = virConnectGetURI(dom->conn->ptr);

    if (fork() <= 0) {
        /* parent */
        free(uri);
        return;
    } else {
        /* child */
        execlp("virt-viewer", "virt-viewer", "-w", "-c", uri, dom->uuid, NULL);
        perror("execlp");
        exit(1);
    }
}

static void menu_cb_vm_run(GtkAction *action, void *data)
{
    struct vconsole_window *win = data;
    struct vconsole_domain *dom = find_guest(win);

    if (dom)
        domain_start(dom);
}

static void menu_cb_vm_pause(GtkAction *action, void *data)
{
    struct vconsole_window *win = data;
    struct vconsole_domain *dom = find_guest(win);

    if (dom)
        domain_pause(dom);
}

static void menu_cb_vm_save(GtkAction *action, void *data)
{
    struct vconsole_window *win = data;
    struct vconsole_domain *dom = find_guest(win);

    if (dom)
        domain_save(dom);
}

static void menu_cb_vm_reboot(GtkAction *action, void *data)
{
    struct vconsole_window *win = data;
    struct vconsole_domain *dom = find_guest(win);

    if (dom)
        domain_reboot(dom);
}

static void menu_cb_vm_shutdown(GtkAction *action, void *data)
{
    struct vconsole_window *win = data;
    struct vconsole_domain *dom = find_guest(win);

    if (dom)
        domain_shutdown(dom);
}

static void menu_cb_vm_reset(GtkAction *action, void *data)
{
    struct vconsole_window *win = data;
    struct vconsole_domain *dom = find_guest(win);

    if (dom)
        domain_reset(dom);
}

static void menu_cb_vm_kill(GtkAction *action, void *data)
{
    struct vconsole_window *win = data;
    struct vconsole_domain *dom = find_guest(win);

    if (dom)
        domain_kill(dom);
}

static void menu_cb_about(GtkAction *action, gpointer userdata)
{
    static char *comments = "virtual machine console";
    static char *copyright = "(c) 2012 Gerd Hoffmann";
    static char *authors[] = { "Gerd Hoffmann <kraxel@redhat.com>", NULL };
    struct vconsole_window *win = userdata;

    gtk_show_about_dialog(GTK_WINDOW(win->toplevel),
                          "authors",         authors,
                          "comments",        comments,
                          "copyright",       copyright,
                          "logo-icon-name",  GTK_STOCK_ABOUT,
                          "version",         VERSION,
                          NULL);
}

static void menu_cb_manual(GtkAction *action, gpointer userdata)
{
    if (fork() <= 0) {
        /* parent */
        return;
    } else {
        /* child */
        execlp("xdg-open", "xdg-open", "man:vconsole(1)", NULL);
        perror("execlp");
        exit(1);
    }
}

/* ------------------------------------------------------------------ */

static void menu_cb_blink_cursor(GtkToggleAction *action, gpointer userdata)
{
    struct vconsole_window *win = userdata;

    win->tty_blink = gtk_toggle_action_get_active(action);
    domain_configure_all_vtes(win);
    g_key_file_set_boolean(config, "tty", "blink", win->tty_blink);
    config_write();
}

static void menu_cb_vm_logging(GtkToggleAction *action, gpointer userdata)
{
    struct vconsole_window *win = userdata;

    win->vm_logging = gtk_toggle_action_get_active(action);
    domain_configure_all_logging(win);
    g_key_file_set_boolean(config, "vm", "logging", win->vm_logging);
    config_write();
}

/* ------------------------------------------------------------------ */

static const GtkActionEntry entries[] = {
    {
        /* --- menu bar --- */
	.name        = "FileMenu",
	.label       = "_File",
    },{
	.name        = "ViewMenu",
	.label       = "_View",
    },{
	.name        = "GuestMenu",
	.label       = "_Guest",
    },{
	.name        = "HelpMenu",
	.label       = "_Help",
    },{

        /* --- submenus --- */
	.name        = "ConnectMenu",
	.label       = "_Recent",
    },{

        /* --- file menu --- */
	.name        = "ConnectAsk",
	.stock_id    = GTK_STOCK_CONNECT,
	.label       = "_Connect ...",
	.callback    = G_CALLBACK(menu_cb_connect_ask),
    },{
	.name        = "CloseTab",
	.stock_id    = GTK_STOCK_CLOSE,
	.label       = "Close _Tab",
        .tooltip     = "Close Tab",
	.callback    = G_CALLBACK(menu_cb_close_tab),
    },{
	.name        = "CloseApp",
	.stock_id    = GTK_STOCK_QUIT,
	.label       = "_Quit",
	.callback    = G_CALLBACK(menu_cb_close_app),
    },{

        /* --- view menu --- */
	.name        = "TerminalFont",
	.stock_id    = GTK_STOCK_SELECT_FONT,
	.label       = "Terminal _font ...",
	.callback    = G_CALLBACK(menu_cb_config_font),
    },{
	.name        = "TerminalForeground",
	.stock_id    = GTK_STOCK_SELECT_COLOR,
	.label       = "Terminal _text color ...",
	.callback    = G_CALLBACK(menu_cb_config_fg),
    },{
	.name        = "TerminalBackground",
	.label       = "Terminal _background ...",
	.callback    = G_CALLBACK(menu_cb_config_bg),
    },{

        /* --- guest menu --- */
	.name        = "GuestGfx",
	.label       = "Show graphic console",
	.callback    = G_CALLBACK(menu_cb_vm_gfx),
    },{
	.name        = "GuestRun",
	.stock_id    = GTK_STOCK_MEDIA_PLAY,
	.label       = "Run",
        .tooltip     = "Run guest",
	.callback    = G_CALLBACK(menu_cb_vm_run),
    },{
	.name        = "GuestPause",
	.stock_id    = GTK_STOCK_MEDIA_PAUSE,
	.label       = "Pause",
        .tooltip     = "Pause guest",
	.callback    = G_CALLBACK(menu_cb_vm_pause),
    },{
	.name        = "GuestSave",
        .stock_id    = GTK_STOCK_SAVE,
	.label       = "Save to disk",
        .tooltip     = "Save guest to disk",
	.accelerator = "", /* don't catch Ctrl-S */
	.callback    = G_CALLBACK(menu_cb_vm_save),
    },{
	.name        = "GuestReboot",
	.label       = "Reboot",
        .tooltip     = "Reboot guest",
	.callback    = G_CALLBACK(menu_cb_vm_reboot),
    },{
	.name        = "GuestShutdown",
	.label       = "Shutdown",
        .tooltip     = "Shutdown guest",
	.callback    = G_CALLBACK(menu_cb_vm_shutdown),
    },{
	.name        = "GuestReset",
	.label       = "Reset",
        .tooltip     = "Reset guest",
	.callback    = G_CALLBACK(menu_cb_vm_reset),
    },{
	.name        = "GuestKill",
	.label       = "Destroy",
        .tooltip     = "Destriy guest",
	.callback    = G_CALLBACK(menu_cb_vm_kill),

    },{
        /* --- help menu --- */
	.name        = "About",
        .stock_id    = GTK_STOCK_ABOUT,
	.label       = "_About ...",
	.callback    = G_CALLBACK(menu_cb_about),
    },{
	.name        = "Manual",
        .stock_id    = GTK_STOCK_HELP,
	.label       = "_Manual page",
	.callback    = G_CALLBACK(menu_cb_manual),
    },
};

static const GtkToggleActionEntry tentries[] = {
    {
	.name        = "TerminalBlink",
	.label       = "Blinking cursor",
	.callback    = G_CALLBACK(menu_cb_blink_cursor),
    },{
	.name        = "FullScreen",
        .stock_id    = GTK_STOCK_FULLSCREEN,
	.label       = "_Fullscreen",
	.accelerator = "F11",
	.callback    = G_CALLBACK(menu_cb_fullscreen),
    },{
	.name        = "GuestLogging",
	.label       = "Log to file",
	.callback    = G_CALLBACK(menu_cb_vm_logging),
    }
};

static char ui_xml[] =
"<ui>\n"
"  <menubar name='MainMenu'>\n"
"    <menu action='FileMenu'>\n"
"      <menuitem action='ConnectAsk'/>\n"
"      <menu action='ConnectMenu'>\n"
"      </menu>\n"
"      <separator/>\n"
"      <menuitem action='CloseTab'/>\n"
"      <menuitem action='CloseApp'/>\n"
"    </menu>\n"
"    <menu action='ViewMenu'>\n"
"      <menuitem action='TerminalFont'/>\n"
"      <menuitem action='TerminalForeground'/>\n"
"      <menuitem action='TerminalBackground'/>\n"
"      <menuitem action='TerminalBlink'/>\n"
"      <separator/>\n"
"      <menuitem action='FullScreen'/>\n"
"    </menu>\n"
"    <menu action='GuestMenu'>\n"
"      <menuitem action='GuestLogging'/>\n"
"      <separator/>\n"
"      <menuitem action='GuestGfx'/>\n"
"      <separator/>\n"
"      <menuitem action='GuestRun'/>\n"
"      <menuitem action='GuestPause'/>\n"
"      <menuitem action='GuestSave'/>\n"
"      <menuitem action='GuestReboot'/>\n"
"      <menuitem action='GuestShutdown'/>\n"
"      <separator/>\n"
"      <menuitem action='GuestReset'/>\n"
"      <menuitem action='GuestKill'/>\n"
"    </menu>\n"
"    <menu action='HelpMenu'>\n"
"      <menuitem action='Manual'/>\n"
"      <menuitem action='About'/>\n"
"    </menu>\n"
"  </menubar>\n"
"  <toolbar action='ToolBar'>"
"    <toolitem action='CloseTab'/>\n"
"    <separator/>\n"
"    <toolitem action='GuestRun'/>\n"
"    <toolitem action='GuestPause'/>\n"
"    <toolitem action='GuestSave'/>\n"
"  </toolbar>\n"
"</ui>\n";

static char recent_xml[] =
"<ui>\n"
"  <menubar name='MainMenu'>\n"
"    <menu action='FileMenu'>\n"
"      <menu action='ConnectMenu'>\n"
"%s"
"      </menu>\n"
"    </menu>\n"
"  </menubar>\n"
"</ui>\n";

/* ------------------------------------------------------------------ */

static gboolean window_state_cb(GtkWidget *widget, GdkEventWindowState *event,
				gpointer userdata)
{
    struct vconsole_window *win = userdata;
    GtkWidget *fs, *tb;

    if (!(event->changed_mask & GDK_WINDOW_STATE_FULLSCREEN))
        return TRUE;
    win->fullscreen = event->new_window_state & GDK_WINDOW_STATE_FULLSCREEN;

    fs = gtk_ui_manager_get_widget(win->ui, "/MainMenu/ViewMenu/FullScreen");
    tb = gtk_ui_manager_get_widget(win->ui, "/ToolBar");

    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(fs), win->fullscreen);
    if (win->fullscreen) {
        gtk_widget_hide(tb);
    } else {
        gtk_widget_show(tb);
    }
    return TRUE;
}

static void destroy(GtkWidget *widget, gpointer data)
{
    gtk_main_quit();
}

static void vconsole_build_recent(struct vconsole_window *win)
{
    GError *err = NULL;
    GtkActionEntry entry;
    char *xml, *h, *entries = NULL;
    gchar **keys, *action;
    gsize i, nkeys = 0;

    /* cleanup */
    if (win->r_id) {
	gtk_ui_manager_remove_ui(win->ui, win->r_id);
	win->r_id = 0;
    }
    if (win->r_ag) {
	gtk_ui_manager_remove_action_group(win->ui, win->r_ag);
	g_object_unref(win->r_ag);
	win->r_ag = NULL;
    }

    /* start */
    win->r_ag = gtk_action_group_new("RecentActions");

    /* add entries */
    keys = g_key_file_get_keys(config, "hosts", &nkeys, &err);
    for (i = 0; i < nkeys; i++) {
        action = g_strdup_printf("ConnectMenu_%s", keys[i]);
        memset(&entry, 0, sizeof(entry));
        entry.callback = G_CALLBACK(menu_cb_connect_menu);
        entry.name = action;
        entry.label = keys[i];
        gtk_action_group_add_actions(win->r_ag, &entry, 1, win);
        h = entries;
        entries = g_strdup_printf("%s        <menuitem action='%s'/>\n",
                                  h ? h : "", action);
        g_free(h);
        g_free(action);
    }
    g_strfreev(keys);

    /* finish */
    xml = g_strdup_printf(recent_xml, entries ? entries : "");
    if (debug)
        fprintf(stderr, "---\n%s---\n", xml);
    gtk_ui_manager_insert_action_group(win->ui, win->r_ag, 1);
    win->r_id = gtk_ui_manager_add_ui_from_string(win->ui, xml, -1, &err);
    if (!win->r_id) {
	g_message("building menu failed: %s", err->message);
	g_error_free(err);
    }
    g_free(xml);
}

static struct vconsole_window *vconsole_toplevel_create(void)
{
    struct vconsole_window *win;
    GtkWidget *vbox, *menubar, *toolbar, *item;
    GtkAccelGroup *accel;
    GtkActionGroup *ag;
    GError *err;

    win = g_new0(struct vconsole_window, 1);
    win->toplevel = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win->toplevel), APPNAME);
    gtk_window_set_default_size(GTK_WINDOW(win->toplevel), 640, 400);
    g_signal_connect(G_OBJECT(win->toplevel), "destroy",
		     G_CALLBACK(destroy), win);
    g_signal_connect(G_OBJECT(win->toplevel), "window-state-event",
		     G_CALLBACK(window_state_cb), win);

    /* menu + toolbar */
    win->ui = gtk_ui_manager_new();
    ag = gtk_action_group_new("MenuActions");
    gtk_action_group_add_actions(ag, entries, G_N_ELEMENTS(entries), win);
    gtk_action_group_add_toggle_actions(ag, tentries,
					G_N_ELEMENTS(tentries), win);
    gtk_ui_manager_insert_action_group(win->ui, ag, 0);
    accel = gtk_ui_manager_get_accel_group(win->ui);
    gtk_window_add_accel_group(GTK_WINDOW(win->toplevel), accel);

    err = NULL;
    if (!gtk_ui_manager_add_ui_from_string(win->ui, ui_xml, -1, &err)) {
	g_message("building menus failed: %s", err->message);
	g_error_free(err);
	exit(1);
    }

    /* main area */
    win->notebook = gtk_notebook_new();

    /* Make a vbox and put stuff in */
    vbox = gtk_vbox_new(FALSE, 1);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 1);
    gtk_container_add(GTK_CONTAINER(win->toplevel), vbox);
    menubar = gtk_ui_manager_get_widget(win->ui, "/MainMenu");
    gtk_box_pack_start(GTK_BOX(vbox), menubar, FALSE, FALSE, 0);
    toolbar = gtk_ui_manager_get_widget(win->ui, "/ToolBar");
    if (toolbar)
	gtk_box_pack_start(GTK_BOX(vbox), toolbar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), win->notebook, TRUE, TRUE, 0);

    /* read config */
    err = NULL;
    win->tty_font = g_key_file_get_string(config, "tty", "font", &err);
    err = NULL;
    win->tty_fg = g_key_file_get_string(config, "tty", "foreground", &err);
    err = NULL;
    win->tty_bg = g_key_file_get_string(config, "tty", "background", &err);
    err = NULL;
    win->tty_blink = g_key_file_get_boolean(config, "tty", "blink", &err);
    err = NULL;
    win->vm_logging = g_key_file_get_boolean(config, "vm", "logging", &err);

    /* config defaults */
    if (!win->tty_font)
        win->tty_font = "Monospace 12";
    if (!win->tty_fg)
        win->tty_fg = "white";
    if (!win->tty_bg)
        win->tty_bg = "black";

    /* apply config */
    item = gtk_ui_manager_get_widget(win->ui, "/MainMenu/ViewMenu/TerminalBlink");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), win->tty_blink);
    item = gtk_ui_manager_get_widget(win->ui, "/MainMenu/GuestMenu/GuestLogging");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), win->vm_logging);

    return win;
}

static void vconsole_tab_list_activate(GtkTreeView *tree_view,
                                       GtkTreePath *path,
                                       GtkTreeViewColumn *column,
                                       gpointer user_data)
{
    struct vconsole_window *win = user_data;
    GtkTreeModel *model = GTK_TREE_MODEL(win->store);
    GtkTreeIter parent, iter;
    gboolean is_host;
    char *name;
    struct vconsole_domain *dom;

    if (!gtk_tree_model_get_iter(model, &iter, path))
        return;
    gtk_tree_model_get(model, &iter, NAME_COL, &name, -1);
    is_host = !gtk_tree_model_iter_parent(model, &parent, &iter);
    if (is_host) {
        if (debug)
            fprintf(stderr, "%s: host %s\n", __func__, name);
        if (gtk_tree_view_row_expanded(tree_view, path)) {
            gtk_tree_view_collapse_row(tree_view, path);
        } else {
            gtk_tree_view_expand_row(tree_view, path, FALSE);
        }
    } else {
        if (debug)
            fprintf(stderr, "%s: guest %s\n", __func__, name);
        gtk_tree_model_get(model, &iter, DPTR_COL, &dom, -1);
        domain_activate(dom);
    }
    g_free(name);
}

static gint gtk_sort_iter_compare_str(GtkTreeModel *model,
                                      GtkTreeIter  *a,
                                      GtkTreeIter  *b,
                                      gpointer      userdata)
{
    gint sortcol = GPOINTER_TO_INT(userdata);
    char *aa,*bb;
    int ret;

    gtk_tree_model_get(model, a, sortcol, &aa, -1);
    gtk_tree_model_get(model, b, sortcol, &bb, -1);
    if (NULL == aa && NULL == bb) {
        ret = 0;
    } else if (NULL == aa) {
        ret = 1;
    } else if (NULL == bb) {
        ret = -1;
    } else {
        ret = strcmp(aa,bb);
    }
    g_free(aa);
    g_free(bb);
    return ret;
}

static void vconsole_tab_list_create(struct vconsole_window *win)
{
    GtkCellRenderer *renderer;
    GtkTreeViewColumn *column;
    GtkWidget *label, *scroll;
    GtkTreeSortable *sortable;

    /* store & view */
    win->store = gtk_tree_store_new(N_COLUMNS,
                                    G_TYPE_STRING,   // NAME_COL
                                    G_TYPE_POINTER,  // CPTR_COL
                                    G_TYPE_STRING,   // URI_COL
                                    G_TYPE_POINTER,  // DPTR_COL
                                    G_TYPE_INT,      // ID_COL
                                    G_TYPE_STRING,   // STATE_COL
                                    G_TYPE_INT,      // NR_CPUS_COL
                                    G_TYPE_STRING,   // LOAD_STR_COL
                                    G_TYPE_INT,      // LOAD_INT_COL
                                    G_TYPE_STRING,   // MEMORY_COL
                                    G_TYPE_BOOLEAN,  // IS_RUNNING_COL
                                    G_TYPE_BOOLEAN,  // HAS_MEMCPU_COL
                                    G_TYPE_STRING,   // FOREGROUND_COL
                                    G_TYPE_INT);     // WEIGHT_COL
    sortable = GTK_TREE_SORTABLE(win->store);
    win->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(win->store));

    g_signal_connect(G_OBJECT(win->tree), "row-activated",
                     G_CALLBACK(vconsole_tab_list_activate),
                     win);

    /* name */
    renderer = gtk_cell_renderer_text_new();
    column = gtk_tree_view_column_new_with_attributes("Name",
                                                      renderer,
                                                      "text", NAME_COL,
                                                      "weight", WEIGHT_COL,
                                                      "foreground", FOREGROUND_COL,
                                                      NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(win->tree), column);
    gtk_tree_sortable_set_sort_func(sortable, NAME_COL,
                                    gtk_sort_iter_compare_str,
                                    GINT_TO_POINTER(NAME_COL), NULL);
    gtk_tree_view_column_set_sort_column_id(column, NAME_COL);

    /* id */
    renderer = gtk_cell_renderer_text_new();
    g_object_set(renderer, "xalign", 0.5, NULL);
    column = gtk_tree_view_column_new_with_attributes("ID",
                                                      renderer,
                                                      "text", ID_COL,
                                                      "visible", IS_RUNNING_COL,
                                                      NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(win->tree), column);

    /* state */
    renderer = gtk_cell_renderer_text_new();
    column = gtk_tree_view_column_new_with_attributes("State",
                                                      renderer,
                                                      "text", STATE_COL,
                                                      NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(win->tree), column);

    /* memory */
    renderer = gtk_cell_renderer_text_new();
    g_object_set(renderer, "xalign", 1.0, NULL);
    column = gtk_tree_view_column_new_with_attributes("memory",
                                                      renderer,
                                                      "text", MEMORY_COL,
                                                      "visible", HAS_MEMCPU_COL,
                                                      NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(win->tree), column);

    /* cpu count */
    renderer = gtk_cell_renderer_text_new();
    g_object_set(renderer, "xalign", 0.5, NULL);
    column = gtk_tree_view_column_new_with_attributes("vcpus",
                                                      renderer,
                                                      "text", NR_CPUS_COL,
                                                      "visible", HAS_MEMCPU_COL,
                                                      NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(win->tree), column);

    /* cpu load */
    renderer = gtk_cell_renderer_progress_new();
    g_object_set(renderer, "width", 100, NULL);
    column = gtk_tree_view_column_new_with_attributes("Load",
                                                      renderer,
                                                      "text", LOAD_STR_COL,
                                                      "value", LOAD_INT_COL,
                                                      "visible", IS_RUNNING_COL,
                                                      NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(win->tree), column);

    /* padding */
    renderer = gtk_cell_renderer_text_new();
    g_object_set(renderer, "visible", FALSE, NULL);
    column = gtk_tree_view_column_new_with_attributes("", renderer, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(win->tree), column);

    /* sort store */
    gtk_tree_sortable_set_sort_column_id(sortable, NAME_COL,
                                         GTK_SORT_ASCENDING);

    /* add tab */
    label = gtk_label_new("Guests");
    scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scroll), win->tree);
    gtk_notebook_insert_page(GTK_NOTEBOOK(win->notebook),
                             scroll, label, 0);
}

static gboolean vconsole_update(gpointer data)
{
    struct vconsole_window *win = data;

    domain_update_all(win);
    return TRUE;
}

/* ------------------------------------------------------------------ */

static void usage(FILE *fp)
{
    fprintf(fp,
	    "This is a virtual machine console\n"
	    "\n"
	    "usage: %s [ options ]\n"
	    "options:\n"
	    "   -h          Print this text.\n"
	    "   -d          Enable debugging.\n"
	    "   -c <uri>    Connect to libvirt.\n"
	    "\n"
	    "-- \n"
	    "(c) 2012 Gerd Hoffmann <kraxel@redhat.com>\n",
            APPNAME);
}

int
main(int argc, char *argv[])
{
    struct vconsole_window *win;
    char *uri = NULL;
    int c;

    gtk_init(&argc, &argv);
    for (;;) {
        if (-1 == (c = getopt(argc, argv, "hdc:")))
            break;
        switch (c) {
	case 'd':
	    debug++;
	    break;
        case 'c':
            uri = optarg;
            break;
        case 'h':
            usage(stdout);
            exit(0);
        default:
            usage(stderr);
            exit(1);
        }
    }

    if (uri == NULL)
        uri = getenv("LIBVIRT_DEFAULT_URI");
    if (uri == NULL)
        uri = getenv("VIRSH_DEFAULT_CONNECT_URI");

    /* init */
    g_thread_init(NULL);
    gvir_event_register();
    config_read();

    /* main window */
    win = vconsole_toplevel_create();
    vconsole_tab_list_create(win);
    gtk_widget_show_all(win->toplevel);
    gtk_widget_grab_focus(win->notebook);

    if (uri)
        connect_init(win, uri);
    vconsole_build_recent(win);

    g_timeout_add(10 * 1000, vconsole_update, win);

    /* main loop */
    gtk_main();

    /* cleanup */
    exit(0);
}
