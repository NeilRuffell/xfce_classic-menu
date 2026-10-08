/* classic-menu.c - Main plugin file */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>
#include <libxfce4ui/libxfce4ui.h>
#include <libxfce4util/libxfce4util.h>
#include <garcon/garcon.h>
#include <exo/exo.h>

#include "classic-menu.h"

/* Plugin structure */
typedef struct {
    XfcePanelPlugin  *plugin;

    GtkWidget        *menubar;
    GtkWidget        *applications_item;
    GtkWidget        *applications_icon;
    gchar            *icon_name;
    GtkWidget        *places_item;
    GtkWidget        *system_item;

    GarconMenu       *garcon_menu;
    guint             reload_idle_id;
    gboolean          reloading;
    GFileMonitor      *bookmarks_monitor;
    guint              bookmarks_reload_id;
    ClassicMenuConfig config;
}
ClassicMenuPlugin;

/* Config key names */
#define CONFIG_GROUP        "classic-menu"
#define KEY_DRILLDOWN_MODE  "drilldown-mode"
#define KEY_ICON_NAME       "applications-icon"
#define DEFAULT_ICON_NAME   "start-here"
#define KEY_SHOW_APPS       "show-applications"
#define KEY_SHOW_PLACES     "show-places"
#define KEY_SHOW_SYSTEM     "show-system"
#define KEY_SHOW_ICON       "show-applications-icon"
#define KEY_ICON_SIZE       "menu-icon-size"

/* Forward declarations */
static void     classic_menu_construct(          XfcePanelPlugin *plugin);
static void     classic_menu_free(               XfcePanelPlugin *plugin, ClassicMenuPlugin *menu);
static gboolean classic_menu_size_changed(       XfcePanelPlugin *plugin, gint               size,        ClassicMenuPlugin *menu);
static void     classic_menu_orientation_changed(XfcePanelPlugin *plugin, GtkOrientation     orientation, ClassicMenuPlugin *menu);
static void     classic_menu_configure(          XfcePanelPlugin *plugin,                                 ClassicMenuPlugin *menu);

/* ── Config ─────────────────────────────────────────────────────────────── */

static void
classic_menu_config_load(ClassicMenuPlugin *menu)
{
    XfceRc *rc;
    gchar  *path;

    /* Sensible defaults */
    menu->config.drilldown_mode = DRILLDOWN_FOLDERS_ONLY;
    menu->config.show_applications = TRUE;
    menu->config.show_places = TRUE;
    menu->config.show_system = TRUE;
    menu->config.show_icon = TRUE;
    menu->config.icon_size = 16;
    menu->icon_name = g_strdup(DEFAULT_ICON_NAME);

    path = xfce_panel_plugin_lookup_rc_file(menu->plugin);
    if (path == NULL) {
        return;
    }

    rc = xfce_rc_simple_open(path, TRUE);
    g_free(path);
    if (rc == NULL) {
        return;
    }

    xfce_rc_set_group(rc, CONFIG_GROUP);
    {
        const gchar *icon = xfce_rc_read_entry(rc, KEY_ICON_NAME, DEFAULT_ICON_NAME);
        if (icon != NULL && *icon != '\0') {
            g_free(menu->icon_name);
            menu->icon_name = g_strdup(icon);
        }
    }
    menu->config.drilldown_mode = (DrillDownMode)
        xfce_rc_read_int_entry(rc, KEY_DRILLDOWN_MODE, DRILLDOWN_FOLDERS_ONLY);

    menu->config.show_applications = xfce_rc_read_bool_entry(rc, KEY_SHOW_APPS, TRUE);
    menu->config.show_places = xfce_rc_read_bool_entry(rc, KEY_SHOW_PLACES, TRUE);
    menu->config.show_system = xfce_rc_read_bool_entry(rc, KEY_SHOW_SYSTEM, TRUE);
    menu->config.show_icon = xfce_rc_read_bool_entry(rc, KEY_SHOW_ICON, TRUE);
    menu->config.icon_size = CLAMP(xfce_rc_read_int_entry(rc, KEY_ICON_SIZE, 16), 12, 48);
    xfce_rc_close(rc);
}

static void
classic_menu_config_save(ClassicMenuPlugin *menu)
{
    XfceRc *rc;
    gchar  *path;

    path = xfce_panel_plugin_save_location(menu->plugin, TRUE);
    if (path == NULL) {
        return;
    }

    rc = xfce_rc_simple_open(path, FALSE);
    g_free(path);
    if (rc == NULL) {
        return;
    }

    xfce_rc_set_group(rc, CONFIG_GROUP);
    xfce_rc_write_int_entry(rc, KEY_DRILLDOWN_MODE,
                            (gint) menu->config.drilldown_mode);
    xfce_rc_write_entry(rc, KEY_ICON_NAME, menu->icon_name);
    xfce_rc_write_bool_entry(rc, KEY_SHOW_APPS, menu->config.show_applications);
    xfce_rc_write_bool_entry(rc, KEY_SHOW_PLACES, menu->config.show_places);
    xfce_rc_write_bool_entry(rc, KEY_SHOW_SYSTEM, menu->config.show_system);
    xfce_rc_write_bool_entry(rc, KEY_SHOW_ICON, menu->config.show_icon);
    xfce_rc_write_int_entry(rc, KEY_ICON_SIZE, menu->config.icon_size);

    xfce_rc_close(rc);
}

static void
classic_menu_update_icon(ClassicMenuPlugin *menu)
{
    if (g_path_is_absolute(menu->icon_name)) {
        gtk_image_set_from_file(GTK_IMAGE(menu->applications_icon),
                                menu->icon_name);
    } else {
        gtk_image_set_from_icon_name(GTK_IMAGE(menu->applications_icon),
                                     menu->icon_name, GTK_ICON_SIZE_MENU);
    }
    gtk_image_set_pixel_size(GTK_IMAGE(menu->applications_icon), menu->config.icon_size);
    gtk_widget_set_visible(menu->applications_icon, menu->config.show_icon);
}

static void
on_icon_button_clicked(GtkButton *button, gpointer user_data)
{
    ClassicMenuPlugin *menu = user_data;
    GtkWindow *parent = GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(button)));
    GtkWidget *chooser = exo_icon_chooser_dialog_new(
            "Choose Applications Icon", parent,
            "_Cancel", GTK_RESPONSE_CANCEL,
            "_Select", GTK_RESPONSE_ACCEPT, NULL);

    exo_icon_chooser_dialog_set_icon(EXO_ICON_CHOOSER_DIALOG(chooser),
                                     menu->icon_name);
    if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
        gchar *icon = exo_icon_chooser_dialog_get_icon(
                EXO_ICON_CHOOSER_DIALOG(chooser));
        if (icon != NULL && *icon != '\0') {
            g_free(menu->icon_name);
            menu->icon_name = icon;
            classic_menu_update_icon(menu);
            classic_menu_config_save(menu);
        } else {
            g_free(icon);
        }
    }
    gtk_widget_destroy(chooser);
}

/* Garcon emits reload-required when desktop menu configuration changes.
 * Coalesce changes and rebuild outside signal emission. */
static gboolean
classic_menu_reload_idle(gpointer data)
{
    ClassicMenuPlugin *menu = data;
    menu->reload_idle_id = 0;
    menu->reloading = TRUE;
    /* Destroy the old GTK menus before reloading the Garcon element tree. */
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(menu->applications_item), NULL);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(menu->system_item), NULL);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(menu->applications_item),
                              build_applications_menu(&menu->garcon_menu));
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(menu->system_item),
                              build_system_menu(&menu->garcon_menu));
    menu->reloading = FALSE;
    return G_SOURCE_REMOVE;
}

static void
on_garcon_reload_required(GarconMenu *garcon, gpointer user_data)
{
    ClassicMenuPlugin *menu = user_data;
    if (!menu->reloading && menu->reload_idle_id == 0)
        menu->reload_idle_id = g_idle_add(classic_menu_reload_idle, menu);
}

/* Use an already-installed XDG menu editor; no parallel editor implementation. */
static void
on_edit_menu_clicked(GtkButton *button, gpointer user_data)
{
    const gchar *editor = NULL;
    gchar *path = g_find_program_in_path("menulibre");
    if (path != NULL) {
        editor = "menulibre";
        g_free(path);
    } else {
        path = g_find_program_in_path("alacarte");
        if (path != NULL) {
            editor = "alacarte";
            g_free(path);
        }
    }
    if (editor != NULL) {
        gchar *argv[] = { (gchar *)editor, NULL };
        GError *error = NULL;
        if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                           NULL, NULL, NULL, &error)) {
            g_warning("Cannot open menu editor: %s", error->message);
            g_clear_error(&error);
        }
    }
}

static void
classic_menu_apply_visibility(ClassicMenuPlugin *menu)
{
    gtk_widget_set_visible(menu->applications_item, menu->config.show_applications);
    gtk_widget_set_visible(menu->places_item, menu->config.show_places);
    gtk_widget_set_visible(menu->system_item, menu->config.show_system);
    gtk_widget_set_visible(menu->applications_icon, menu->config.show_icon);
}

static void
on_visibility_toggled(GtkToggleButton *button, gpointer user_data)
{
    ClassicMenuPlugin *menu = user_data;
    const gchar *which = g_object_get_data(G_OBJECT(button), "menu-section");
    gboolean active = gtk_toggle_button_get_active(button);
    if (g_strcmp0(which, "applications") == 0)
        menu->config.show_applications = active;
    else if (g_strcmp0(which, "places") == 0)
        menu->config.show_places = active;
    else if (g_strcmp0(which, "system") == 0)
        menu->config.show_system = active;
    else if (g_strcmp0(which, "icon") == 0)
        menu->config.show_icon = active;
    classic_menu_apply_visibility(menu);
    classic_menu_config_save(menu);
}

static void
on_icon_size_changed(GtkSpinButton *spin, gpointer user_data)
{
    ClassicMenuPlugin *menu = user_data;
    menu->config.icon_size = gtk_spin_button_get_value_as_int(spin);
    classic_menu_update_icon(menu);
    classic_menu_config_save(menu);
}

static void on_places_deactivate(GtkWidget *widget, gpointer user_data);

/* Never replace a submenu while GTK is displaying it. A bookmarks event
 * queues a refresh; if Places is open, wait for deactivation. */
static gboolean
bookmarks_refresh_idle(gpointer data)
{
    ClassicMenuPlugin *menu = data;
    menu->bookmarks_reload_id = 0;
    GtkWidget *current = gtk_menu_item_get_submenu(GTK_MENU_ITEM(menu->places_item));
    if (current != NULL && gtk_widget_get_mapped(current))
        return G_SOURCE_REMOVE;
    GtkWidget *replacement = build_places_menu(&menu->config);
    g_signal_connect(replacement, "deactivate",
                     G_CALLBACK(on_places_deactivate), menu);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(menu->places_item), replacement);
    return G_SOURCE_REMOVE;
}

static void
on_places_deactivate(GtkWidget *widget, gpointer user_data)
{
    ClassicMenuPlugin *menu = user_data;
    if (menu->bookmarks_reload_id != 0)
        return;
    /* Rebuild only after the menu has finished being dismissed. */
    menu->bookmarks_reload_id = g_idle_add(bookmarks_refresh_idle, menu);
}

static void
on_bookmarks_changed(GFileMonitor *monitor, GFile *file, GFile *other_file,
                     GFileMonitorEvent event, gpointer user_data)
{
    ClassicMenuPlugin *menu = user_data;
    GtkWidget *current = gtk_menu_item_get_submenu(GTK_MENU_ITEM(menu->places_item));
    if (current != NULL && gtk_widget_get_mapped(current))
        return; /* The deactivate callback refreshes it safely. */
    if (menu->bookmarks_reload_id == 0)
        menu->bookmarks_reload_id = g_idle_add(bookmarks_refresh_idle, menu);
}

/* ── Properties dialog ──────────────────────────────────────────────────── */

static void
on_drilldown_changed(GtkComboBox *combo, gpointer user_data)
{
    ClassicMenuPlugin *menu = (ClassicMenuPlugin *)user_data;

    menu->config.drilldown_mode =
        (DrillDownMode) gtk_combo_box_get_active(combo);

    classic_menu_config_save(menu);

    /* Rebuild the Places menu so the change takes effect immediately */
    {
        GtkWidget *places_menu = build_places_menu(&menu->config);
        gtk_menu_item_set_submenu(
                GTK_MENU_ITEM(menu->places_item),
                places_menu
            );
    }
}

static void
classic_menu_configure(XfcePanelPlugin *plugin, ClassicMenuPlugin *menu)
{
    GtkWidget *dialog;
    GtkWidget *grid;
    GtkWidget *label;
    GtkWidget *combo;
    GtkWidget *icon_button;
    GtkWidget *editor_button;
    GtkWidget *check;
    GtkWidget *spin;

    xfce_panel_plugin_block_menu(plugin);

    dialog = xfce_titled_dialog_new_with_mixed_buttons(
            "Classic Menu Preferences",
            NULL,
            GTK_DIALOG_DESTROY_WITH_PARENT,
            "window-close-symbolic", "Close", GTK_RESPONSE_OK,
            NULL
        );

    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);

    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    gtk_box_pack_start(
            GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))),
            grid, TRUE, TRUE, 0
        );

    label = gtk_label_new("Folder drill-down shows:");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 0, 1, 1);

    combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(
            GTK_COMBO_BOX_TEXT(combo), "None"
        );
    gtk_combo_box_text_append_text(
            GTK_COMBO_BOX_TEXT(combo), "Folders only"
        );
    gtk_combo_box_text_append_text(
            GTK_COMBO_BOX_TEXT(combo), "Folders and AppImages"
        );
    gtk_combo_box_text_append_text(
            GTK_COMBO_BOX_TEXT(combo), "All contents"
        );
    gtk_combo_box_set_active(
            GTK_COMBO_BOX(combo), (gint) menu->config.drilldown_mode
        );
    gtk_grid_attach(GTK_GRID(grid), combo, 1, 0, 1, 1);

    label = gtk_label_new("Applications icon:");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 1, 1, 1);
    icon_button = gtk_button_new_with_label("Choose Icon...");
    gtk_grid_attach(GTK_GRID(grid), icon_button, 1, 1, 1, 1);
    g_signal_connect(icon_button, "clicked",
                     G_CALLBACK(on_icon_button_clicked), menu);

    editor_button = gtk_button_new_with_label("Edit Applications Menu...");
    gtk_grid_attach(GTK_GRID(grid), editor_button, 1, 2, 1, 1);
    g_signal_connect(editor_button, "clicked",
                     G_CALLBACK(on_edit_menu_clicked), menu);
    {
        gchar *ml = g_find_program_in_path("menulibre");
        gchar *al = g_find_program_in_path("alacarte");
        gtk_widget_set_sensitive(editor_button, ml != NULL || al != NULL);
        if (ml == NULL && al == NULL)
            gtk_widget_set_tooltip_text(editor_button,
                  "Install MenuLibre or Alacarte to edit application menus");
        g_free(ml);
        g_free(al);
    }

    const gchar *sections[] = {"applications", "places", "system", "icon"};
    const gchar *titles[] = {"Show Applications", "Show Places", "Show System", "Show Applications icon"};
    gboolean states[] = {menu->config.show_applications, menu->config.show_places,
                         menu->config.show_system, menu->config.show_icon};
    for (gint i = 0; i < 4; i++) {
        check = gtk_check_button_new_with_label(titles[i]);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), states[i]);
        g_object_set_data(G_OBJECT(check), "menu-section", (gpointer)sections[i]);
        gtk_grid_attach(GTK_GRID(grid), check, 0, i + 3, 2, 1);
        g_signal_connect(check, "toggled", G_CALLBACK(on_visibility_toggled), menu);
    }
    label = gtk_label_new("Applications icon size:");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 7, 1, 1);
    spin = gtk_spin_button_new_with_range(12, 48, 2);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), menu->config.icon_size);
    gtk_grid_attach(GTK_GRID(grid), spin, 1, 7, 1, 1);
    g_signal_connect(spin, "value-changed", G_CALLBACK(on_icon_size_changed), menu);

    g_signal_connect(
            G_OBJECT(combo), "changed",
            G_CALLBACK(on_drilldown_changed),
            menu
        );

    g_signal_connect_swapped(
            G_OBJECT(dialog), "response",
            G_CALLBACK(gtk_widget_destroy),
            dialog
        );
    g_signal_connect_swapped(
            G_OBJECT(dialog), "destroy",
            G_CALLBACK(xfce_panel_plugin_unblock_menu),
            plugin
        );

    gtk_widget_show_all(dialog);
}

/* ── Plugin construction ────────────────────────────────────────────────── */

static void
classic_menu_construct(XfcePanelPlugin *plugin)
{
    ClassicMenuPlugin *menu;
    GtkWidget         *applications_menu;
    GtkWidget         *places_menu;
    GtkWidget         *system_menu;

    menu         = g_new0(ClassicMenuPlugin, 1);
    menu->plugin = plugin;

    classic_menu_config_load(menu);

    /* Create menubar */
    menu->menubar = gtk_menu_bar_new();
    gtk_container_add(GTK_CONTAINER(plugin), menu->menubar);
    xfce_panel_plugin_add_action_widget(plugin, menu->menubar);

    /* Applications */
    menu->applications_item = gtk_menu_item_new();
    {
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
        GtkWidget *label = gtk_label_new("Applications");
        menu->applications_icon = gtk_image_new();
        classic_menu_update_icon(menu);
        gtk_box_pack_start(GTK_BOX(box), menu->applications_icon, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(menu->applications_item), box);
    }
    applications_menu       = build_applications_menu(&menu->garcon_menu);
    gtk_menu_item_set_submenu(
            GTK_MENU_ITEM(menu->applications_item),
            applications_menu
        );
    gtk_menu_shell_append(
            GTK_MENU_SHELL(menu->menubar),
            menu->applications_item
        );

    if (menu->garcon_menu != NULL)
        g_signal_connect(menu->garcon_menu, "reload-required",
                         G_CALLBACK(on_garcon_reload_required), menu);

    /* Places */
    menu->places_item = gtk_menu_item_new_with_label("Places");
    places_menu       = build_places_menu(&menu->config);
    gtk_menu_item_set_submenu(
            GTK_MENU_ITEM(menu->places_item),
            places_menu
        );
    gtk_menu_shell_append(
            GTK_MENU_SHELL(menu->menubar),
            menu->places_item
        );

    /* System */
    menu->system_item = gtk_menu_item_new_with_label("System");
    system_menu       = build_system_menu(&menu->garcon_menu);
    gtk_menu_item_set_submenu(
            GTK_MENU_ITEM(menu->system_item),
            system_menu
        );
    gtk_menu_shell_append(
            GTK_MENU_SHELL(menu->menubar),
            menu->system_item
        );

    gtk_widget_show_all(menu->menubar);
    classic_menu_apply_visibility(menu);
    {
        GtkWidget *places_submenu = gtk_menu_item_get_submenu(GTK_MENU_ITEM(menu->places_item));
        g_signal_connect(places_submenu, "deactivate", G_CALLBACK(on_places_deactivate), menu);
    }
    /* Watch the parent directory so atomic bookmark-file replacement is noticed. */
    {
        gchar *directory = g_build_filename(g_get_user_config_dir(), "gtk-3.0", NULL);
        GFile *dir = g_file_new_for_path(directory);
        menu->bookmarks_monitor = g_file_monitor_directory(dir, G_FILE_MONITOR_NONE, NULL, NULL);
        if (menu->bookmarks_monitor != NULL)
            g_signal_connect(menu->bookmarks_monitor, "changed", G_CALLBACK(on_bookmarks_changed), menu);
        g_object_unref(dir);
        g_free(directory);
    }

    /* Plugin signals */
    g_signal_connect(
            G_OBJECT(plugin), "free-data",
            G_CALLBACK(classic_menu_free), menu
        );
    g_signal_connect(
            G_OBJECT(plugin), "size-changed",
            G_CALLBACK(classic_menu_size_changed), menu
        );
    g_signal_connect(
            G_OBJECT(plugin), "orientation-changed",
            G_CALLBACK(classic_menu_orientation_changed), menu
        );

    /* Show the Configure entry in the right-click plugin menu */
    xfce_panel_plugin_menu_show_configure(plugin);
    g_signal_connect(
            G_OBJECT(plugin), "configure-plugin",
            G_CALLBACK(classic_menu_configure), menu
        );
}

/* ── Boilerplate ────────────────────────────────────────────────────────── */

static void
classic_menu_free(XfcePanelPlugin *plugin, ClassicMenuPlugin *menu)
{
    if (menu->reload_idle_id != 0)
        g_source_remove(menu->reload_idle_id);
    if (menu->bookmarks_reload_id != 0)
        g_source_remove(menu->bookmarks_reload_id);
    g_clear_object(&menu->bookmarks_monitor);
    if (menu->garcon_menu != NULL) {
        g_object_unref(menu->garcon_menu);
    }

    g_free(menu->icon_name);
    g_free(menu);
}

static gboolean
classic_menu_size_changed(
    XfcePanelPlugin   *plugin,
    gint               size,
    ClassicMenuPlugin *menu
)
{
    gtk_widget_set_size_request(GTK_WIDGET(plugin), -1, -1);
    return TRUE;
}

static void
classic_menu_orientation_changed(
    XfcePanelPlugin   *plugin,
    GtkOrientation     orientation,
    ClassicMenuPlugin *menu
)
{
    /* Menubar automatically handles orientation */
}

/* Register the plugin */
XFCE_PANEL_PLUGIN_REGISTER(classic_menu_construct);
