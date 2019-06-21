#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <assert.h>
#include <inttypes.h>

#include <sys/stat.h>
#include <sys/time.h>

#include <gtk/gtk.h>
#include <vte/vte.h>

#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>

/* ------------------------------------------------------------------ */

enum vconsole_cols {
    /* common */
    NAME_COL,

    /* hosts only */
    CPTR_COL,  // vconsole_connect
    TYPE_COL,
    URI_COL,

    /* guests only */
    DPTR_COL,  // vconsole_domain
    ID_COL,
    STATE_COL,
    NR_CPUS_COL,
    LOAD_STR_COL,
    LOAD_INT_COL,
    MEMORY_COL,

    /* flags */
    IS_RUNNING_COL,
    HAS_MEMCPU_COL,

    /* beautify */
    FOREGROUND_COL,
    WEIGHT_COL,

    /* end of list */
    N_COLUMNS
};

struct vconsole_window {
    /* toplevel window */
    GtkWidget                 *toplevel;
    GtkWidget                 *notebook;
    GtkUIManager              *ui;
    gboolean                  fullscreen;

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
    gboolean                  vm_logging;
    gboolean                  darkmode;
};

extern int debug;
extern GKeyFile *config;

void gtk_message(GtkWidget *parent, GtkWidget **dialog, GtkMessageType type,
                char *fmt, ...)
    __attribute__ ((format (printf, 4, 0)));

void config_write(void);

/* ------------------------------------------------------------------ */

struct vconsole_connect {
    struct vconsole_window    *win;
    virConnectPtr             ptr;
    GtkWidget                 *warn;
    GtkWidget                 *err;
    GtkWidget                 *info;
    gboolean                  cap_migration;
    gboolean                  cap_start_paused;
    gboolean                  cap_console_force;
};

struct vconsole_connect *connect_init(struct vconsole_window *win,
                                      const char *uri);
void connect_close(virConnectPtr c, int reason, void *opaque);

/* ------------------------------------------------------------------ */

struct vconsole_domain {
    struct vconsole_connect   *conn;
    char                      uuid[VIR_UUID_STRING_BUFLEN];
    int                       id;
    const char                *name;

    GtkWidget                 *window, *vbox, *vte, *status;
    virStreamPtr              stream;
    virDomainInfo             info;
    gboolean                  saved;
    gboolean                  unpause;

    struct timeval            ts;
    struct timeval            last_ts;
    virDomainInfo             last_info;
    int                       load;

    FILE                      *logfp;
    char                      *logname;
};

void domain_untabify(struct vconsole_domain *dom);

void domain_start(struct vconsole_domain *dom);
void domain_pause(struct vconsole_domain *dom);
void domain_save(struct vconsole_domain *dom);
void domain_reboot(struct vconsole_domain *dom);
void domain_shutdown(struct vconsole_domain *dom);
void domain_reset(struct vconsole_domain *dom);
void domain_kill(struct vconsole_domain *dom);

void domain_free(struct vconsole_domain *dom);
void domain_update(struct vconsole_connect *conn,
                   virDomainPtr d, virDomainEventType event);
void domain_activate(struct vconsole_domain *dom);
void domain_configure_all_vtes(struct vconsole_window *win);
void domain_configure_all_logging(struct vconsole_window *win);
struct vconsole_domain *domain_find_current_tab(struct vconsole_window *win);
void domain_close_current_tab(struct vconsole_window *win);

void domain_update_all(struct vconsole_window *win);
