/* classic-menu.h - Common header for classic menu plugin */

#ifndef __CLASSIC_MENU_H__
#define __CLASSIC_MENU_H__

#include <gtk/gtk.h>
#include <garcon/garcon.h>

/* Drill-down mode for the Places folder submenus */
typedef enum {
    DRILLDOWN_NONE              = 0,  /* no drill-down submenus */
    DRILLDOWN_FOLDERS_ONLY      = 1,  /* directories only (default) */
    DRILLDOWN_FOLDERS_APPIMAGES = 2,  /* directories + *.AppImage files */
    DRILLDOWN_ALL_CONTENTS      = 3,  /* directories + all files */
} DrillDownMode;

/* Plugin-wide configuration.  Populated from xfce_rc on load, written
 * back on every change, and passed to each menu builder that needs it. */
typedef struct {
    DrillDownMode drilldown_mode;
    gboolean show_applications;
    gboolean show_places;
    gboolean show_system;
    gboolean show_icon;
    gint icon_size;
} ClassicMenuConfig;

/* applications-menu.c */
GtkWidget *build_applications_menu(GarconMenu **garcon_menu_ptr);

/* places-menu.c */
GtkWidget *build_places_menu(const ClassicMenuConfig *config);

/* system-menu.c */
GtkWidget *build_system_menu(GarconMenu **garcon_menu_ptr);

#endif /* __CLASSIC_MENU_H__ */
