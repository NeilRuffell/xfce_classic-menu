/* system-menu.c - System menu implementation */

#include "classic-menu.h"
#include <garcon/garcon.h>
#define WNCK_I_KNOW_THIS_IS_UNSTABLE
#include <libwnck/libwnck.h>

/* Create a menu item with icon and label */
static GtkWidget *
create_menu_item_with_icon(const gchar *label, const gchar *icon_name)
{
    GtkWidget *menu_item;
    GtkWidget *box;
    GtkWidget *icon;
    GtkWidget *label_widget;

    menu_item = gtk_menu_item_new();
    box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    if (icon_name != NULL) {
        icon = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_MENU);
        gtk_image_set_pixel_size(GTK_IMAGE(icon), 16);
        gtk_box_pack_start(GTK_BOX(box), icon, FALSE, FALSE, 0);
    }

    label_widget = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(label_widget), 0.0);
    gtk_box_pack_start(GTK_BOX(box), label_widget, TRUE, TRUE, 0);

    gtk_container_add(GTK_CONTAINER(menu_item), box);
    gtk_widget_show_all(menu_item);

    return menu_item;
}

/* System action callback */
static void
on_system_action(GtkMenuItem *item, gpointer user_data)
{
    const gchar *command = (const gchar *)user_data;
    GError      *error   = NULL;

    if (!g_spawn_command_line_async(command, &error)) {
        g_warning(
                "Failed to execute command: %s",
                error ? error->message : "Unknown error"
            );
        if (error) {
            g_error_free(error);
        }
    }
}

/* Launch application from a GarconMenuItem */
static void
on_application_activate(GtkMenuItem *item, gpointer user_data)
{
    GarconMenuItem *garcon_item = GARCON_MENU_ITEM(user_data);
    const gchar    *command;
    GError         *error = NULL;

    command = garcon_menu_item_get_command(garcon_item);
    if (command == NULL) {
        g_warning("No command found for menu item");
        return;
    }

    if (!g_spawn_command_line_async(command, &error)) {
        g_warning(
                "Failed to launch application: %s",
                error ? error->message : "Unknown error"
            );
        if (error) {
            g_error_free(error);
        }
    }
}

/* Populate a GtkMenu from a flat list of GarconMenuElements.
 * Used for the Preferences and Administration submenus, which only
 * need one level of items (no further nesting). */
static void
populate_flat_menu(GtkWidget *menu, GList *elements)
{
    GList *li;

    for (li = elements; li != NULL; li = li->next) {
        GarconMenuElement *element = GARCON_MENU_ELEMENT(li->data);

        if (GARCON_IS_MENU_SEPARATOR(element)) {
            GtkWidget *sep = gtk_separator_menu_item_new();
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep);
        }
        else if (GARCON_IS_MENU_ITEM(element)) {
            GarconMenuItem *garcon_item = GARCON_MENU_ITEM(element);
            const gchar    *name;
            const gchar    *icon_name;
            GtkWidget      *menu_item;

            if (!garcon_menu_element_get_visible(element)) {
                continue;
            }
            if (garcon_menu_item_get_no_display(garcon_item)) {
                continue;
            }

            name      = garcon_menu_element_get_name(element);
            icon_name = garcon_menu_element_get_icon_name(element);

            menu_item = create_menu_item_with_icon(name, icon_name);
            g_signal_connect(
                    G_OBJECT(menu_item), "activate",
                    G_CALLBACK(on_application_activate),
                    garcon_item
                );
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_item);
        }
        else if (GARCON_IS_MENU(element)) {
            /* Recurse one level for nested subcategories */
            GarconMenu          *submenu_data = GARCON_MENU(element);
            GarconMenuDirectory *directory;
            const gchar         *category_name;
            const gchar         *category_icon;
            GList               *sub_elements;
            GtkWidget           *category_item;
            GtkWidget           *category_submenu;

            directory = garcon_menu_get_directory(submenu_data);
            if (directory != NULL
                    && !garcon_menu_directory_get_visible(directory)) {
                continue;
            }

            category_name = garcon_menu_element_get_name(
                    GARCON_MENU_ELEMENT(submenu_data)
                );
            category_icon = garcon_menu_element_get_icon_name(
                    GARCON_MENU_ELEMENT(submenu_data)
                );

            sub_elements = garcon_menu_get_elements(submenu_data);
            if (sub_elements == NULL) {
                continue;
            }

            category_submenu = gtk_menu_new();
            gtk_menu_set_reserve_toggle_size(
                    GTK_MENU(category_submenu), FALSE
                );
            populate_flat_menu(category_submenu, sub_elements);

            if (gtk_container_get_children(
                        GTK_CONTAINER(category_submenu)
                    ) == NULL) {
                gtk_widget_destroy(category_submenu);
                continue;
            }

            category_item = create_menu_item_with_icon(
                    category_name, category_icon
                );
            gtk_menu_item_set_submenu(
                    GTK_MENU_ITEM(category_item),
                    category_submenu
                );
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), category_item);
        }
    }
}

/* Find a top-level category in the garcon tree by display name and
 * return a populated GtkMenu for it, or NULL if not found. */
static GtkWidget *
build_category_submenu(GarconMenu *root, const gchar *category_name)
{
    GList *elements;
    GList *li;

    if (root == NULL) {
        return NULL;
    }

    elements = garcon_menu_get_elements(root);

    for (li = elements; li != NULL; li = li->next) {
        GarconMenuElement *element = GARCON_MENU_ELEMENT(li->data);

        if (!GARCON_IS_MENU(element)) {
            continue;
        }

        GarconMenu  *submenu_data = GARCON_MENU(element);
        const gchar *name         = garcon_menu_element_get_name(
                GARCON_MENU_ELEMENT(submenu_data)
            );

        if (g_strcmp0(name, category_name) != 0) {
            continue;
        }

        GList     *sub_elements = garcon_menu_get_elements(submenu_data);
        GtkWidget *gtk_submenu;

        if (sub_elements == NULL) {
            return NULL;
        }

        gtk_submenu = gtk_menu_new();
        gtk_menu_set_reserve_toggle_size(GTK_MENU(gtk_submenu), FALSE);
        populate_flat_menu(gtk_submenu, sub_elements);

        if (gtk_container_get_children(GTK_CONTAINER(gtk_submenu)) == NULL) {
            gtk_widget_destroy(gtk_submenu);
            return NULL;
        }

        return gtk_submenu;
    }

    return NULL;
}

/* Use the window-manager interface underlying XFCE's Show Desktop. */
static void
on_show_desktop_activate(GtkMenuItem *item, gpointer user_data)
{
    WnckScreen *screen = wnck_screen_get_default();
    if (screen != NULL) {
        wnck_screen_force_update(screen);
        wnck_screen_toggle_showing_desktop(
                screen, !wnck_screen_get_showing_desktop(screen));
    }
}

/* Build the System menu */
GtkWidget *
build_system_menu(GarconMenu **garcon_menu_ptr)
{
    GtkWidget *submenu = gtk_menu_new();
    GtkWidget *item;
    GtkWidget *category_submenu;

    gtk_menu_set_reserve_toggle_size(GTK_MENU(submenu), FALSE);

    /* ── Settings Manager ─────────────────────────────────────────────── */

    item = create_menu_item_with_icon("Settings Manager", "preferences-system");
    g_signal_connect(
            G_OBJECT(item), "activate",
            G_CALLBACK(on_system_action),
            "xfce4-settings-manager"
        );
    gtk_menu_shell_append(GTK_MENU_SHELL(submenu), item);

    /* ── Preferences (garcon "Settings" category) ─────────────────────── */

    item = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(submenu), item);

    category_submenu = build_category_submenu(*garcon_menu_ptr, "Settings");
    if (category_submenu != NULL) {
        item = create_menu_item_with_icon("Preferences", "preferences-other");
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), category_submenu);
        gtk_menu_shell_append(GTK_MENU_SHELL(submenu), item);
    }

    /* ── Administration (garcon "System" category) ────────────────────── */

    category_submenu = build_category_submenu(*garcon_menu_ptr, "System");
    if (category_submenu != NULL) {
        item = create_menu_item_with_icon("Administration", "system-config-users");
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), category_submenu);
        gtk_menu_shell_append(GTK_MENU_SHELL(submenu), item);
    }

    /* ── Help / About ─────────────────────────────────────────────────── */

    item = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(submenu), item);

    item = create_menu_item_with_icon("Help", "help-browser");
    g_signal_connect(
            G_OBJECT(item), "activate",
            G_CALLBACK(on_system_action),
            "xfhelp4"
        );
    gtk_menu_shell_append(GTK_MENU_SHELL(submenu), item);

    item = create_menu_item_with_icon("About XFCE", "xfce4-logo");
    g_signal_connect(
            G_OBJECT(item), "activate",
            G_CALLBACK(on_system_action),
            "xfce4-about"
        );
    gtk_menu_shell_append(GTK_MENU_SHELL(submenu), item);

    item = create_menu_item_with_icon("Show Desktop", "user-desktop");
    g_signal_connect(item, "activate",
                     G_CALLBACK(on_show_desktop_activate), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(submenu), item);

    /* ── Session ──────────────────────────────────────────────────────── */

    item = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(submenu), item);

    item = create_menu_item_with_icon("Lock Screen", "system-lock-screen");
    g_signal_connect(
            G_OBJECT(item), "activate",
            G_CALLBACK(on_system_action),
            "xflock4"
        );
    gtk_menu_shell_append(GTK_MENU_SHELL(submenu), item);

    item = create_menu_item_with_icon("Log Out...", "system-log-out");
    g_signal_connect(
            G_OBJECT(item), "activate",
            G_CALLBACK(on_system_action),
            "xfce4-session-logout"
        );
    gtk_menu_shell_append(GTK_MENU_SHELL(submenu), item);

    gtk_widget_show_all(submenu);
    return submenu;
}
