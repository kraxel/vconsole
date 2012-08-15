#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <assert.h>

#include <gtk/gtk.h>
#include <vte/vte.h>

#include <libvirt/libvirt.h>

/* ------------------------------------------------------------------ */

enum vconsole_cols {
    /* common */
    NAME_COL,

    /* hosts only */
    CPTR_COL,  // vconsole_connect
    URI_COL,

    /* guests only */
    DPTR_COL,  // vconsole_domain
    ID_COL,
    STATE_COL,
    N_COLUMNS
};

struct vconsole_window {
    /* toplevel window */
    GtkWidget                 *toplevel;
    GtkWidget                 *notebook;
    GtkUIManager              *ui;

    /* recent hosts */
    GtkActionGroup            *r_ag;
    guint                     r_id;

    /* domain list tab */
    GtkTreeStore              *store;
    GtkWidget                 *tree;

    /* options */
    gboolean                  tty_blink;
    char                      *tty_font;
    char                      *tty_fg;
    char                      *tty_bg;
};

extern int debug;
extern GKeyFile *config;

void config_write(void);

/* ------------------------------------------------------------------ */

struct vconsole_connect {
    struct vconsole_window    *win;
    virConnectPtr             ptr;
};

struct vconsole_connect *connect_init(struct vconsole_window *win,
                                      const char *uri);
/* ------------------------------------------------------------------ */

struct vconsole_domain {
    struct vconsole_connect   *conn;
    char                      uuid[VIR_UUID_STRING_BUFLEN];

    GtkWidget                 *vbox, *vte, *status;
    int                       page;
    virStreamPtr              stream;
    virDomainInfo             info;
};

void domain_update(struct vconsole_connect *conn,
                   virDomainPtr d, virDomainEventType event);
void domain_activate(struct vconsole_domain *dom);
void domain_configure_all_vtes(struct vconsole_window *win);
void domain_close_current_tab(struct vconsole_window *win);
