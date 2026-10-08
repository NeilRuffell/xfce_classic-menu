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
    ClassicMenuConfig config;
}
ClassicMenuPlugin;

/* Config key names */
#define CONFIG_GROUP        "classic-menu"
#define KEY_DRILLDOWN_MODE  "drilldown-mode"
#define KEY_ICON_NAME       "applications-icon"
#define DEFAULT_ICON_NAME   "start-here"

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
    gtk_image_set_pixel_size(GTK_IMAGE(menu->applications_icon), 18);
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
