/* applications-menu.c - Applications menu implementation */

#include "classic-menu.h"
#include <garcon/garcon.h>

/* Launch application callback */
static void
on_application_activate(GtkMenuItem *item, gpointer user_data)
{
    GarconMenuItem *garcon_item = GARCON_MENU_ITEM(user_data);
    const gchar *command;
    GError *error = NULL;

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

/* Match GarconGtkMenu's native Xfce drag protocol. The panel and Thunar
 * consume text/uri-list containing the application's original .desktop URI. */
static const GtkTargetEntry launcher_drag_targets[] = {
    { (gchar *)"text/uri-list", 0, 0 }
};

static void
on_launcher_drag_begin(GtkWidget *widget, GdkDragContext *context,
                       gpointer user_data)
{
    GarconMenuItem *item = GARCON_MENU_ITEM(user_data);
    const gchar *icon = garcon_menu_item_get_icon_name(item);
    if (icon != NULL && *icon != '\0')
        gtk_drag_set_icon_name(context, icon, 0, 0);
}

static void
on_launcher_drag_data_get(GtkWidget *widget, GdkDragContext *context,
                          GtkSelectionData *selection, guint info,
                          guint time, gpointer user_data)
{
    GarconMenuItem *item = GARCON_MENU_ITEM(user_data);
    gchar *uri = garcon_menu_item_get_uri(item);
    if (uri != NULL) {
        gchar *uris[] = { uri, NULL };
        gtk_selection_data_set_uris(selection, uris);
        g_free(uri);
    }
}

static void
on_launcher_drag_end(GtkWidget *widget, GdkDragContext *context,
                     gpointer user_data)
{
    GtkWidget *parent = gtk_widget_get_parent(widget);
    if (GTK_IS_MENU(parent))
        gtk_menu_popdown(GTK_MENU(parent));
}

static void
enable_launcher_drag(GtkWidget *widget, GarconMenuItem *item)
{
    gtk_drag_source_set(widget, GDK_BUTTON1_MASK, launcher_drag_targets,
                        G_N_ELEMENTS(launcher_drag_targets), GDK_ACTION_COPY);
    /* Keep source metadata alive through the drag, independently of the
     * Garcon menu tree's lifetime. */
    g_object_set_data_full(G_OBJECT(widget), "launcher-drag-item",
                           g_object_ref(item), g_object_unref);
    g_signal_connect(widget, "drag-begin",
                     G_CALLBACK(on_launcher_drag_begin), item);
    g_signal_connect(widget, "drag-data-get",
                     G_CALLBACK(on_launcher_drag_data_get), item);
    g_signal_connect(widget, "drag-end",
                     G_CALLBACK(on_launcher_drag_end), NULL);
}

/* Search/Run dialog callback */
static void
on_search_activate(GtkMenuItem *item, gpointer user_data)
{
    GError *error = NULL;

    if (!g_spawn_command_line_async("xfce4-appfinder", &error)) {
        g_warning(
                "Failed to launch application finder: %s", 
                error ? error->message : "Unknown error"
            );
        if (error) {
            g_error_free(error);
        }
    }
}

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
        /* Use GTK_ICON_SIZE_MENU as the nominal size, then enforce 16 px
         * explicitly.  Without gtk_image_set_pixel_size() some entries
         * whose icon_name is an absolute path to a full-resolution PNG
         * will render at their native size and look oversized next to
         * theme icons that are properly scaled to 16 px. */
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

/* Recursively build menu from garcon elements.
 *
 * top_level: when TRUE, bare GarconMenuItems at the root scope are
 * silently dropped.  The root of xfce-applications.menu contains items
 * such as "About XFCE" and "Log Out" whose parent category carries
 * NoDisplay=true; garcon surfaces them as top-level GarconMenuItems
 * rather than nesting them under a hidden GarconMenu node.  They belong
 * in the System menu and must never appear here. */
static void
populate_menu_from_elements(GtkWidget *menu, GList *elements, gboolean top_level)
{
    GList *li;

    for (li = elements; li != NULL; li = li->next) {
        GarconMenuElement *element = GARCON_MENU_ELEMENT(li->data);

        if (GARCON_IS_MENU_SEPARATOR(element)) {
            /* Suppress root-level separators — garcon emits them between
             * its internal groupings regardless of whether anything
             * visible flanks them.  The one separator we want (below
             * the Search item) is added explicitly in
             * build_applications_menu(). */
            if (top_level) {
                continue;
            }
            GtkWidget *separator = gtk_separator_menu_item_new();
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), separator);
        }
        else if (GARCON_IS_MENU(element)) {
            /* Submenu (category) */
            GarconMenu          *submenu_data = GARCON_MENU(element);
            GarconMenuDirectory *directory;
            GtkWidget           *category_item;
            GtkWidget           *category_submenu;
            const gchar         *category_name;
            const gchar         *category_icon;
            GList               *submenu_elements;

            /* Use garcon_menu_directory_get_visible() as the upstream
             * garcon-gtk-menu.c does: it is the authoritative combined
             * check for Hidden, NoDisplay, and OnlyShowIn/NotShowIn on
             * the directory.  When the directory is NULL the category
             * has no .directory file and is implicitly visible. */
            directory = garcon_menu_get_directory(submenu_data);
            if (directory != NULL
                    && !garcon_menu_directory_get_visible(directory)) {
                continue;
            }

            /* The display name and icon come from the GarconMenu node,
             * not the GarconMenuDirectory.  Calling
             * garcon_menu_element_get_name() on the directory via the
             * GarconMenuElement interface returns NULL because the
             * directory vfunc is not wired the same way. */
            category_name = garcon_menu_element_get_name(
                    GARCON_MENU_ELEMENT(submenu_data)
                );
            category_icon = garcon_menu_element_get_icon_name(
                    GARCON_MENU_ELEMENT(submenu_data)
                );

            /* At the root level, suppress categories that belong in the
             * System menu.  "Settings" maps to xfce4-settings-manager
             * territory and "System" covers terminal/file manager/task
             * manager entries — both are surfaced under the System menu
             * item in classic-menu.c via build_system_menu(). */
            if (top_level && category_name != NULL
                    && (g_strcmp0(category_name, "Settings") == 0
                        || g_strcmp0(category_name, "System")  == 0)) {
                continue;
            }

            /* Get items in this submenu */
            submenu_elements = garcon_menu_get_elements(submenu_data);
            if (submenu_elements == NULL) {
                continue;
            }

            /* Build submenu recursively (never top-level from here) */
            category_submenu = gtk_menu_new();
            gtk_menu_set_reserve_toggle_size(GTK_MENU(category_submenu), FALSE);
            populate_menu_from_elements(
                    category_submenu,
                    submenu_elements,
                    FALSE
                );

            /* Skip categories that are empty after visibility filtering */
            if (gtk_container_get_children(GTK_CONTAINER(category_submenu)) == NULL) {
                gtk_widget_destroy(category_submenu);
                continue;
            }

            /* Create category menu item */
            category_item = create_menu_item_with_icon(
                    category_name,
                    category_icon
                );

            gtk_menu_item_set_submenu(
                    GTK_MENU_ITEM(category_item),
                    category_submenu
                );
            gtk_menu_shell_append(
                    GTK_MENU_SHELL(menu),
                    category_item
                );
        }
        else if (GARCON_IS_MENU_ITEM(element)) {
            /* Menu item (application) */
            GarconMenuItem *garcon_item = GARCON_MENU_ITEM(element);
            GtkWidget      *menu_item;
            const gchar    *name;
            const gchar    *icon_name;

            /* Drop bare items at the root level — they are XFCE-internal
             * entries that belong in the System menu (see above). */
            if (top_level) {
                continue;
            }

            /* Check visibility */
            if (!garcon_menu_element_get_visible(element)) {
                continue;
            }

            name      = garcon_menu_element_get_name(element);
            icon_name = garcon_menu_element_get_icon_name(element);

            menu_item = create_menu_item_with_icon(name, icon_name);

            g_signal_connect(
                    G_OBJECT(menu_item),
                    "activate",
                    G_CALLBACK(on_application_activate),
                    garcon_item
                );

            enable_launcher_drag(menu_item, garcon_item);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_item);
        }
    }
}

/* Build the Applications menu */
GtkWidget *
build_applications_menu(GarconMenu **garcon_menu_ptr)
{
    GtkWidget *main_menu;
    GtkWidget *search_item;
    GtkWidget *separator;
    GList     *elements;

    main_menu = gtk_menu_new();
    gtk_menu_set_reserve_toggle_size(GTK_MENU(main_menu), FALSE);

    /* Add search item at the top */
    search_item = create_menu_item_with_icon(
            "Search for applications...", 
            "system-search"
        );
    g_signal_connect(G_OBJECT(search_item), "activate",
                    G_CALLBACK(on_search_activate), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(main_menu), search_item);

    /* Add separator */
    separator = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(main_menu), separator);

    /* Load the applications menu */
    if (*garcon_menu_ptr == NULL) {
        *garcon_menu_ptr = garcon_menu_new_applications();
    }

    if (!garcon_menu_load(*garcon_menu_ptr, NULL, NULL)) {
        g_warning("Failed to load applications menu");
        gtk_widget_show_all(main_menu);
        return main_menu;
    }

    /* Get all menu elements */
    elements = garcon_menu_get_elements(*garcon_menu_ptr);

    /* Recursively populate the menu, passing top_level=TRUE so that bare
     * GarconMenuItems at the root scope (XFCE system entries) are dropped. */
    populate_menu_from_elements(main_menu, elements, TRUE);

    gtk_widget_show_all(main_menu);
    return main_menu;
}
