/* places-menu.c - Places menu implementation */

#include "classic-menu.h"
#include "appimage-thumbs.h"
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <unistd.h>

/* The active drill-down mode, set once per build_places_menu() call and
 * read by the recursive folder population functions. */
static DrillDownMode current_drilldown_mode = DRILLDOWN_FOLDERS_ONLY;

/* Forward declaration */
static void append_folder_item(GtkWidget   *menu,
                               const gchar *label,
                               const gchar *icon_name,
                               const gchar *path);

/* ── Helpers ────────────────────────────────────────────────────────────── */

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

/* Open a URI in the file manager */
static void
on_place_activate(GtkMenuItem *item, gpointer user_data)
{
    const gchar *uri = (const gchar *)user_data;
    GError *error    = NULL;
    gchar *argv[] = { (gchar *)"exo-open", (gchar *)"--launch",
                      (gchar *)"FileManager", (gchar *)uri, NULL };

    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                       NULL, NULL, NULL, &error)) {
        g_warning(
                "Failed to open location: %s",
                error ? error->message : "Unknown error"
            );
        if (error) {
            g_error_free(error);
        }
    }

}

/* Launch an AppImage directly, prompting to set the executable bit first
 * if needed. */
static void
launch_appimage(const gchar *path, GtkWidget *parent_item)
{
    /* Check executable bit */
    if (access(path, X_OK) != 0) {
        GtkWidget *dialog;
        gint       response;
        gchar     *basename = g_path_get_basename(path);

        dialog = gtk_message_dialog_new(
                NULL,
                GTK_DIALOG_MODAL,
                GTK_MESSAGE_QUESTION,
                GTK_BUTTONS_YES_NO,
                "\"%s\" is not set as executable.\n\n"
                "Set it as executable and run it?",
                basename
            );
        g_free(basename);

        response = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);

        if (response != GTK_RESPONSE_YES) {
            return;
        }

        /* Set executable bit for owner */
        struct stat st;
        if (g_stat(path, &st) == 0) {
            g_chmod(path, st.st_mode | S_IXUSR);
        }
    }

    /* Launch directly */
    {
        GError *error   = NULL;
        gchar  *argv[2] = { (gchar *)path, NULL };

        if (!g_spawn_async(NULL, argv, NULL,
                           G_SPAWN_DEFAULT, NULL, NULL, NULL, &error)) {
            g_warning("Failed to launch AppImage %s: %s", path,
                      error ? error->message : "Unknown error");
            if (error) g_error_free(error);
        }
    }
}

/* Open a local path on click.  Connected to "button-release-event" for
 * plain items and "button-press-event" for items with submenus (since GTK
 * consumes the release event to open the submenu).
 *
 * - Directories open in the file manager.
 * - AppImages are executed directly, with a prompt to set the executable
 *   bit if needed.
 * - All other files open via their default application. */
static gboolean
on_folder_click(GtkWidget *item, GdkEventButton *event, gpointer user_data)
{
    const gchar *uri  = (const gchar *)user_data;
    gchar       *path;

    if (event->button != 1) {
        return FALSE;
    }

    path = g_filename_from_uri(uri, NULL, NULL);

    if (path != NULL && !g_file_test(path, G_FILE_TEST_IS_DIR)) {
        /* It's a file */
        if (g_str_has_suffix(path, ".AppImage")
                || g_str_has_suffix(path, ".appimage")) {
            launch_appimage(path, item);
        } else {
            GError *error = NULL;
            if (!g_app_info_launch_default_for_uri(uri, NULL, &error)) {
                g_warning("Failed to open %s: %s", uri,
                          error ? error->message : "Unknown error");
                if (error) g_error_free(error);
            }
        }
        g_free(path);
        gtk_menu_shell_deactivate(
                GTK_MENU_SHELL(gtk_widget_get_parent(item))
            );
        return TRUE;
    }

    g_free(path);

    /* It's a directory — open in file manager and let GTK continue
     * so the submenu opens on the same click. */
    on_place_activate(GTK_MENU_ITEM(item), user_data);
    return FALSE;
}



static void
append_uri_item(GtkWidget   *menu,
                const gchar *label,
                const gchar *icon_name,
                const gchar *uri)
{
    GtkWidget *item = create_menu_item_with_icon(label, icon_name);
    g_signal_connect_data(
            G_OBJECT(item), "activate",
            G_CALLBACK(on_place_activate),
            g_strdup(uri),
            (GClosureNotify) g_free,
            0
        );
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

/* ── Lazy drill-down ────────────────────────────────────────────────────── */

/* Callback data for lazy submenu population */
typedef struct {
    gchar *path;  /* owned */
} FolderSubmenuData;

static void
folder_submenu_data_free(gpointer data)
{
    FolderSubmenuData *d = (FolderSubmenuData *)data;
    g_free(d->path);
    g_free(d);
}

/* Called the first time a folder submenu is about to be shown.
 * Removes the dummy placeholder, then populates with immediate child
 * directories sorted alphabetically.  Hidden entries (dot-prefixed)
 * are skipped.  Files are included or excluded based on the current
 * drill-down mode.  If there is nothing to show, an "(Empty)" sentinel
 * is shown so the submenu arrow doesn't just disappear. */
static void
on_folder_submenu_show(GtkWidget *submenu, gpointer user_data)
{
    FolderSubmenuData *d = (FolderSubmenuData *)user_data;
    GDir              *dir;
    GError            *error = NULL;
    const gchar       *name;
    GList             *dirs  = NULL;
    GList             *files = NULL;
    GList             *li;
    gboolean           any   = FALSE;

    /* Only populate once */
    g_signal_handlers_disconnect_by_func(
            submenu,
            G_CALLBACK(on_folder_submenu_show),
            user_data
        );

    /* Remove the dummy placeholder child */
    {
        GList *children = gtk_container_get_children(GTK_CONTAINER(submenu));
        g_list_free_full(children, (GDestroyNotify) gtk_widget_destroy);
    }

    dir = g_dir_open(d->path, 0, &error);
    if (dir == NULL) {
        g_warning("Could not open directory %s: %s",
                  d->path, error ? error->message : "Unknown error");
        if (error) g_error_free(error);
        goto empty;
    }

    while ((name = g_dir_read_name(dir)) != NULL) {
        gchar *child_path;

        if (name[0] == '.') {
            continue;
        }

        child_path = g_build_filename(d->path, name, NULL);

        if (g_file_test(child_path, G_FILE_TEST_IS_DIR)) {
            dirs = g_list_prepend(dirs, child_path);
        } else if (current_drilldown_mode == DRILLDOWN_ALL_CONTENTS) {
            files = g_list_prepend(files, child_path);
        } else if (current_drilldown_mode == DRILLDOWN_FOLDERS_APPIMAGES) {
            if (g_str_has_suffix(name, ".AppImage")
                    || g_str_has_suffix(name, ".appimage")) {
                files = g_list_prepend(files, child_path);
            } else {
                g_free(child_path);
            }
        } else {
            g_free(child_path);
        }
    }
    g_dir_close(dir);

    if (dirs == NULL && files == NULL) {
        goto empty;
    }

    /* Directories first, sorted */
    dirs = g_list_sort(dirs, (GCompareFunc) g_ascii_strcasecmp);
    for (li = dirs; li != NULL; li = li->next) {
        gchar       *child_path = (gchar *)li->data;
        const gchar *basename   = g_path_get_basename(child_path);
        append_folder_item(submenu, basename, "folder", child_path);
        any = TRUE;
    }
    g_list_free_full(dirs, g_free);

    /* Files after a separator, sorted */
    if (files != NULL) {
        if (any) {
            GtkWidget *sep = gtk_separator_menu_item_new();
            gtk_menu_shell_append(GTK_MENU_SHELL(submenu), sep);
        }

        files = g_list_sort(files, (GCompareFunc) g_ascii_strcasecmp);
        for (li = files; li != NULL; li = li->next) {
            gchar         *child_path = (gchar *)li->data;
            const gchar   *basename   = g_path_get_basename(child_path);
            gchar         *uri        = g_filename_to_uri(child_path, NULL, NULL);
            GtkWidget     *file_item;
            gchar         *icon_name  = NULL;

            /* For AppImages use the executable icon as placeholder —
             * appimage_load_icon_async will replace it asynchronously. */
            if (g_str_has_suffix(basename, ".AppImage")
                    || g_str_has_suffix(basename, ".appimage")) {
                icon_name = g_strdup("application-x-executable");
            } else {                /* Ask GIO for the MIME icon list, then walk it to find
                 * the first name that actually exists in the current
                 * theme.  This avoids broken icons from overly-specific
                 * names (e.g. "application-x-iso9660-appimage") that
                 * most themes don't ship. */
                GFile         *gfile      = g_file_new_for_path(child_path);
                GFileInfo     *info       = g_file_query_info(
                        gfile,
                        G_FILE_ATTRIBUTE_STANDARD_ICON,
                        G_FILE_QUERY_INFO_NONE,
                        NULL, NULL
                    );
                GtkIconTheme  *theme      = gtk_icon_theme_get_default();

                if (info != NULL) {
                    GIcon *gicon = g_file_info_get_icon(info);
                    if (G_IS_THEMED_ICON(gicon)) {
                        const gchar * const *names =
                            g_themed_icon_get_names(G_THEMED_ICON(gicon));
                        for (gint n = 0;
                                names != NULL && names[n] != NULL && icon_name == NULL;
                                n++) {
                            if (gtk_icon_theme_has_icon(theme, names[n])) {
                                icon_name = g_strdup(names[n]);
                            }
                        }
                    }
                    g_object_unref(info);
                }
                g_object_unref(gfile);
            }

            if (icon_name == NULL) {
                icon_name = g_strdup("text-x-generic");
            }

            file_item = create_menu_item_with_icon(basename, icon_name);

            /* For AppImages, kick off async thumbnail loading to replace
             * the placeholder icon once extraction/cache lookup is done. */
            if (g_str_has_suffix(basename, ".AppImage")
                    || g_str_has_suffix(basename, ".appimage")) {
                GtkWidget *box  = gtk_bin_get_child(GTK_BIN(file_item));
                GList     *kids = gtk_container_get_children(GTK_CONTAINER(box));
                if (kids != NULL && GTK_IS_IMAGE(kids->data)) {
                    appimage_load_icon_async(GTK_IMAGE(kids->data), child_path);
                }
                g_list_free(kids);
            }

            g_free(icon_name);

            g_signal_connect_data(
                    G_OBJECT(file_item), "button-release-event",
                    G_CALLBACK(on_folder_click),
                    uri,
                    (GClosureNotify) g_free,
                    0
                );
            gtk_menu_shell_append(GTK_MENU_SHELL(submenu), file_item);
            any = TRUE;
        }
        g_list_free_full(files, g_free);
    }

    if (any) {
        gtk_widget_show_all(submenu);
        return;
    }

empty:
    {
        GtkWidget *empty_item = gtk_menu_item_new_with_label("(Empty)");
        gtk_widget_set_sensitive(empty_item, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(submenu), empty_item);
        gtk_widget_show_all(submenu);
    }
}



static void
append_folder_item(GtkWidget   *menu,
                   const gchar *label,
                   const gchar *icon_name,
                   const gchar *path)
{
    GtkWidget         *item;
    gchar             *uri;
    gboolean           has_subdirs = FALSE;

    item = create_menu_item_with_icon(label, icon_name);

    uri = g_filename_to_uri(path, NULL, NULL);

    /* When drill-down is disabled, just append the item with no submenu.
     * Use button-release-event since there's no submenu to compete with. */
    if (current_drilldown_mode == DRILLDOWN_NONE) {
        g_signal_connect_data(
                G_OBJECT(item), "button-release-event",
                G_CALLBACK(on_folder_click),
                uri,
                (GClosureNotify) g_free,
                0
            );
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        return;
    }

    /* Check cheaply whether there is anything to show in the submenu
     * so we only attach a submenu arrow when it's actually warranted. */
    {
        GDir        *dir = g_dir_open(path, 0, NULL);
        const gchar *name;

        if (dir != NULL) {
            while ((name = g_dir_read_name(dir)) != NULL) {
                gchar *child;

                if (name[0] == '.') {
                    continue;
                }

                child = g_build_filename(path, name, NULL);

                if (g_file_test(child, G_FILE_TEST_IS_DIR)) {
                    has_subdirs = TRUE;
                    g_free(child);
                    break;
                }

                /* Also count files that would be shown per the current mode */
                if (current_drilldown_mode == DRILLDOWN_ALL_CONTENTS) {
                    has_subdirs = TRUE;
                    g_free(child);
                    break;
                }
                if (current_drilldown_mode == DRILLDOWN_FOLDERS_APPIMAGES
                        && (g_str_has_suffix(name, ".AppImage")
                            || g_str_has_suffix(name, ".appimage"))) {
                    has_subdirs = TRUE;
                    g_free(child);
                    break;
                }

                g_free(child);
            }
            g_dir_close(dir);
        }
    }

    if (has_subdirs) {
        /* Attach a lazy submenu.  GTK requires a submenu to have at
         * least one child before it will show the arrow, so we insert
         * a dummy placeholder that gets replaced on first show. */
        GtkWidget         *submenu = gtk_menu_new();
        GtkWidget         *dummy   = gtk_menu_item_new();
        FolderSubmenuData *data    = g_new0(FolderSubmenuData, 1);

        gtk_menu_set_reserve_toggle_size(GTK_MENU(submenu), FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(submenu), dummy);

        data->path = g_strdup(path);

        g_signal_connect_data(
                G_OBJECT(submenu), "show",
                G_CALLBACK(on_folder_submenu_show),
                data,
                (GClosureNotify) folder_submenu_data_free,
                0
            );

        gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), submenu);

        g_object_set_data_full(G_OBJECT(item), "folder-uri",
                               g_strdup(uri), (GDestroyNotify) g_free);

        /* GTK consumes button-release-event on items with submenus, but
         * button-press-event fires before GTK grabs it. */
        g_signal_connect_data(G_OBJECT(item), "button-press-event",
                              G_CALLBACK(on_folder_click),
                              uri,
                              (GClosureNotify) g_free,
                              0);

    } else {
        /* No submenu — button-release-event opens in file manager */
        g_signal_connect_data(
                G_OBJECT(item), "button-release-event",
                G_CALLBACK(on_folder_click),
                uri,
                (GClosureNotify) g_free,
                0
            );
    }

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

/* ── Computer drill-down ────────────────────────────────────────────────── */

/* Lazily populate a submenu for a mounted volume by delegating to the
 * folder drill-down logic on the mount's root path. */
static void
on_mount_submenu_show(GtkWidget *submenu, gpointer user_data)
{
    const gchar *root_path = (const gchar *)user_data;
    GDir        *dir;
    const gchar *name;
    GList       *dirs = NULL;
    GList       *li;
    gboolean     any  = FALSE;

    g_signal_handlers_disconnect_by_func(
            submenu,
            G_CALLBACK(on_mount_submenu_show),
            user_data
        );

    /* Remove dummy placeholder */
    {
        GList *children = gtk_container_get_children(GTK_CONTAINER(submenu));
        g_list_free_full(children, (GDestroyNotify) gtk_widget_destroy);
    }

    dir = g_dir_open(root_path, 0, NULL);
    if (dir == NULL) {
        goto empty;
    }

    while ((name = g_dir_read_name(dir)) != NULL) {
        gchar *child_path;

        if (name[0] == '.') {
            continue;
        }

        child_path = g_build_filename(root_path, name, NULL);
        if (g_file_test(child_path, G_FILE_TEST_IS_DIR)) {
            dirs = g_list_prepend(dirs, child_path);
        } else {
            g_free(child_path);
        }
    }
    g_dir_close(dir);

    if (dirs == NULL) {
        goto empty;
    }

    dirs = g_list_sort(dirs, (GCompareFunc) g_ascii_strcasecmp);

    for (li = dirs; li != NULL; li = li->next) {
        gchar       *child_path = (gchar *)li->data;
        const gchar *basename   = g_path_get_basename(child_path);
        append_folder_item(submenu, basename, "folder", child_path);
        any = TRUE;
    }

    g_list_free_full(dirs, g_free);

    if (any) {
        gtk_widget_show_all(submenu);
        return;
    }

empty:
    {
        GtkWidget *empty_item = gtk_menu_item_new_with_label("(Empty)");
        gtk_widget_set_sensitive(empty_item, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(submenu), empty_item);
        gtk_widget_show_all(submenu);
    }
}

/* Append a single volume entry.  Mounted volumes get a submenu arrow that
 * drills into the mount root; unmounted volumes are plain clickable items
 * that open computer:// in the file manager. */
static void
append_volume_item(GtkWidget *menu, GVolume *volume)
{
    gchar     *name;
    GIcon     *gicon;
    gchar     *icon_name = NULL;
    GMount    *mount;
    GtkWidget *item;

    name  = g_volume_get_name(volume);
    gicon = g_volume_get_icon(volume);
    mount = g_volume_get_mount(volume);

    if (G_IS_THEMED_ICON(gicon)) {
        const gchar * const *names =
            g_themed_icon_get_names(G_THEMED_ICON(gicon));
        if (names != NULL && names[0] != NULL) {
            icon_name = g_strdup(names[0]);
        }
    }
    if (icon_name == NULL) {
        icon_name = g_strdup("drive-harddisk");
    }

    item = create_menu_item_with_icon(name, icon_name);

    if (mount != NULL) {
        /* Mounted — clicking opens the mount root, hovering drills in */
        GFile *root      = g_mount_get_root(mount);
        gchar *uri       = g_file_get_uri(root);
        gchar *root_path = g_file_get_path(root);

        g_signal_connect_data(
                G_OBJECT(item), "button-release-event",
                G_CALLBACK(on_folder_click),
                g_strdup(uri),
                (GClosureNotify) g_free,
                0
            );

        /* Only attach a drill-down submenu if drill-down is enabled and
         * the mount has a local path we can scan. */
        if (current_drilldown_mode != DRILLDOWN_NONE && root_path != NULL) {
            GDir        *dir        = g_dir_open(root_path, 0, NULL);
            gboolean     has_subdirs = FALSE;
            const gchar *child_name;

            if (dir != NULL) {
                while ((child_name = g_dir_read_name(dir)) != NULL) {
                    if (child_name[0] == '.') continue;
                    gchar *child = g_build_filename(root_path, child_name, NULL);
                    if (g_file_test(child, G_FILE_TEST_IS_DIR)) {
                        has_subdirs = TRUE;
                        g_free(child);
                        break;
                    }
                    g_free(child);
                }
                g_dir_close(dir);
            }

            if (has_subdirs) {
                GtkWidget *submenu = gtk_menu_new();
                GtkWidget *dummy   = gtk_menu_item_new();

                gtk_menu_set_reserve_toggle_size(GTK_MENU(submenu), FALSE);
                gtk_menu_shell_append(GTK_MENU_SHELL(submenu), dummy);

                g_signal_connect_data(
                        G_OBJECT(submenu), "show",
                        G_CALLBACK(on_mount_submenu_show),
                        g_strdup(root_path),
                        (GClosureNotify) g_free,
                        0
                    );

                gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), submenu);
            }

            g_free(root_path);
        }

        g_free(uri);
        g_object_unref(root);
        g_object_unref(mount);
    } else {
        /* Unmounted — clicking opens computer:// so the user can mount it */
        g_signal_connect_data(
                G_OBJECT(item), "button-release-event",
                G_CALLBACK(on_folder_click),
                g_strdup("computer://"),
                (GClosureNotify) g_free,
                0
            );
    }

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    g_free(icon_name);
    g_free(name);
    g_object_unref(gicon);
}

/* Mount a volume and open it in the file manager once mounted */
typedef struct {
    GVolume *volume;  /* owned */
} MountData;

static void
on_mount_ready(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GVolume *volume = G_VOLUME(source);
    GError  *error  = NULL;

    if (!g_volume_mount_finish(volume, result, &error)) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_warning("Failed to mount volume: %s",
                      error ? error->message : "Unknown error");
        }
        if (error) g_error_free(error);
        return;
    }

    /* Now mounted — open the mount root in the file manager */
    {
        GMount *mount = g_volume_get_mount(volume);
        if (mount == NULL) {
            g_warning("Volume mount reported success but no mount was available");
            return;
        }
        GFile *root = g_mount_get_root(mount);
        if (root != NULL) {
            gchar *uri = g_file_get_uri(root);
            gchar *argv[] = { (gchar *)"exo-open", (gchar *)"--launch",
                              (gchar *)"FileManager", uri, NULL };
            GError *spawn_error = NULL;
            if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                               NULL, NULL, NULL, &spawn_error)) {
                g_warning("Failed to open mounted volume: %s",
                          spawn_error ? spawn_error->message : "unknown");
                g_clear_error(&spawn_error);
            }
            g_free(uri);
            g_object_unref(root);
        }
        g_object_unref(mount);
    }
}

static gboolean
on_unmounted_volume_click(GtkWidget      *item,
                          GdkEventButton *event,
                          gpointer        user_data)
{
    GVolume              *volume = G_VOLUME(user_data);
    GMountOperation      *op;

    if (event->button != 1) {
        return FALSE;
    }

    gtk_menu_shell_deactivate(
            GTK_MENU_SHELL(gtk_widget_get_parent(item))
        );

    op = g_mount_operation_new();
    g_volume_mount(
            volume,
            G_MOUNT_MOUNT_NONE,
            op,
            NULL,
            on_mount_ready,
            NULL
        );
    g_object_unref(op);

    return TRUE;
}

/* Populate the Computer submenu every time it is shown so that drives
 * which have been removed or mounted/unmounted since last open are
 * always reflected accurately. */
static void
on_computer_submenu_show(GtkWidget *submenu, gpointer user_data)
{
    GVolumeMonitor *monitor;
    GList          *volumes;
    GList          *li;
    GList          *mounted   = NULL;
    GList          *unmounted = NULL;

    /* Clear whatever was there last time */
    {
        GList *children = gtk_container_get_children(GTK_CONTAINER(submenu));
        g_list_free_full(children, (GDestroyNotify) gtk_widget_destroy);
    }

    /* ── File System (/) ──────────────────────────────────────────────── */
    append_folder_item(submenu, "File System", "drive-harddisk", "/");

    monitor = g_volume_monitor_get();
    volumes = g_volume_monitor_get_volumes(monitor);

    for (li = volumes; li != NULL; li = li->next) {
        GVolume *volume = G_VOLUME(li->data);
        GMount  *mount  = g_volume_get_mount(volume);

        if (mount != NULL) {
            mounted = g_list_append(mounted, g_object_ref(volume));
            g_object_unref(mount);
        } else {
            unmounted = g_list_append(unmounted, g_object_ref(volume));
        }
    }

    g_list_free_full(volumes, g_object_unref);
    g_object_unref(monitor);

    /* ── Mounted volumes ──────────────────────────────────────────────── */
    if (mounted != NULL) {
        GtkWidget *sep = gtk_separator_menu_item_new();
        gtk_menu_shell_append(GTK_MENU_SHELL(submenu), sep);

        for (li = mounted; li != NULL; li = li->next) {
            append_volume_item(submenu, G_VOLUME(li->data));
        }
        g_list_free_full(mounted, g_object_unref);
    }

    /* ── Unmounted volumes ────────────────────────────────────────────── */
    if (unmounted != NULL) {
        GtkWidget *sep = gtk_separator_menu_item_new();
        gtk_menu_shell_append(GTK_MENU_SHELL(submenu), sep);

        for (li = unmounted; li != NULL; li = li->next) {
            GVolume     *volume    = G_VOLUME(li->data);
            gchar       *name      = g_volume_get_name(volume);
            GIcon       *gicon     = g_volume_get_icon(volume);
            gchar       *icon_name = NULL;
            GtkWidget   *item;

            if (G_IS_THEMED_ICON(gicon)) {
                const gchar * const *names =
                    g_themed_icon_get_names(G_THEMED_ICON(gicon));
                if (names != NULL && names[0] != NULL) {
                    icon_name = g_strdup(names[0]);
                }
            }
            if (icon_name == NULL) {
                icon_name = g_strdup("drive-harddisk");
            }

            item = create_menu_item_with_icon(name, icon_name);
            g_signal_connect_object(
                    G_OBJECT(item), "button-release-event",
                    G_CALLBACK(on_unmounted_volume_click),
                    volume,
                    0
                );
            gtk_menu_shell_append(GTK_MENU_SHELL(submenu), item);

            g_free(icon_name);
            g_free(name);
            g_object_unref(gicon);
        }

        g_list_free_full(unmounted, g_object_unref);
    }

    gtk_widget_show_all(submenu);
}

static void
append_computer_item(GtkWidget *menu)
{
    GtkWidget *item;
    GtkWidget *submenu;
    GtkWidget *dummy;

    item = create_menu_item_with_icon("Computer", "computer");
    g_signal_connect_data(
            G_OBJECT(item), "button-release-event",
            G_CALLBACK(on_folder_click),
            g_strdup("computer://"),
            (GClosureNotify) g_free,
            0
        );

    /* Always attach a submenu — File System alone guarantees it's never
     * empty, and we repopulate on every show to reflect the current
     * mount state. */
    submenu = gtk_menu_new();
    dummy   = gtk_menu_item_new();

    gtk_menu_set_reserve_toggle_size(GTK_MENU(submenu), FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(submenu), dummy);

    g_signal_connect(
            G_OBJECT(submenu), "show",
            G_CALLBACK(on_computer_submenu_show),
            NULL
        );

    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), submenu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

/* Read ~/.config/gtk-3.0/bookmarks (falling back to ~/.gtk-bookmarks) and
 * append each entry to the menu.  Local file:// entries get full
 * drill-down navigation; remote URIs get a plain item. */
static gboolean
append_gtk_bookmarks(GtkWidget *menu)
{
    gchar    *path;
    gchar    *contents = NULL;
    gchar   **lines;
    guint     i;
    gboolean  any = FALSE;

    path = g_build_filename(
            g_get_user_config_dir(), "gtk-3.0", "bookmarks", NULL
        );
    if (!g_file_get_contents(path, &contents, NULL, NULL)) {
        g_free(path);
        path = g_build_filename(g_get_home_dir(), ".gtk-bookmarks", NULL);
        if (!g_file_get_contents(path, &contents, NULL, NULL)) {
            g_free(path);
            return FALSE;
        }
    }
    g_free(path);

    lines = g_strsplit(contents, "\n", -1);
    g_free(contents);

    for (i = 0; lines[i] != NULL; i++) {
        gchar       *line  = g_strstrip(lines[i]);
        gchar       *uri;
        gchar       *space;
        const gchar *label;
        gchar       *derived_label = NULL;

        if (*line == '\0' || *line == '#') {
            continue;
        }

        space = strchr(line, ' ');
        if (space != NULL) {
            *space = '\0';
            label  = (*(space + 1) != '\0') ? space + 1 : NULL;
        } else {
            label = NULL;
        }
        uri = line;

        if (label == NULL) {
            GFile *file   = g_file_new_for_uri(uri);
            derived_label = g_file_get_basename(file);
            g_object_unref(file);
            label = derived_label;
        }

        if (g_str_has_prefix(uri, "file://")) {
            gchar *local_path = g_filename_from_uri(uri, NULL, NULL);
            if (local_path != NULL) {
                append_folder_item(menu, label, "folder", local_path);
                g_free(local_path);
            } else {
                append_uri_item(menu, label, "folder", uri);
            }
        } else {
            append_uri_item(menu, label, "folder-remote", uri);
        }

        g_free(derived_label);
        any = TRUE;
    }

    g_strfreev(lines);
    return any;
}

/* GTK's shared recent-file list, rebuilt when its submenu is opened. */
static void
on_recent_document_activate(GtkMenuItem *item, gpointer user_data)
{
    const gchar *uri = user_data;
    GError *error = NULL;
    if (!g_app_info_launch_default_for_uri(uri, NULL, &error)) {
        g_warning("Cannot open recent document: %s",
                  error ? error->message : "unknown error");
        g_clear_error(&error);
    }
}

static void
on_recent_menu_show(GtkWidget *submenu, gpointer user_data)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(submenu));
    for (GList *l = children; l != NULL; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);

    GtkRecentManager *manager = gtk_recent_manager_get_default();
    GList *items = gtk_recent_manager_get_items(manager);
    guint count = 0;
    for (GList *l = items; l != NULL && count < 20; l = l->next) {
        GtkRecentInfo *info = l->data;
        if (!gtk_recent_info_exists(info))
            continue;
        const gchar *uri = gtk_recent_info_get_uri(info);
        GtkWidget *entry = create_menu_item_with_icon(
                gtk_recent_info_get_display_name(info), "document-open-recent");
        g_signal_connect_data(entry, "activate",
                G_CALLBACK(on_recent_document_activate), g_strdup(uri),
                (GClosureNotify)g_free, 0);
        gtk_menu_shell_append(GTK_MENU_SHELL(submenu), entry);
        count++;
    }
    g_list_free_full(items, (GDestroyNotify)gtk_recent_info_unref);
    if (count == 0) {
        GtkWidget *empty = gtk_menu_item_new_with_label("No Recent Documents");
        gtk_widget_set_sensitive(empty, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(submenu), empty);
    }
    gtk_widget_show_all(submenu);
}

static void
append_recent_documents_item(GtkWidget *menu)
{
    GtkWidget *item = create_menu_item_with_icon(
            "Recent Documents", "document-open-recent");
    GtkWidget *submenu = gtk_menu_new();
    gtk_menu_set_reserve_toggle_size(GTK_MENU(submenu), FALSE);
    g_signal_connect(submenu, "show", G_CALLBACK(on_recent_menu_show), NULL);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), submenu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

/* Invoke XFCE's existing file search application if installed. */
static void
on_file_search_activate(GtkMenuItem *item, gpointer user_data)
{
    gchar *argv[] = { (gchar *)"catfish", NULL };
    GError *error = NULL;
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                       NULL, NULL, NULL, &error)) {
        g_warning("Cannot launch Catfish: %s", error->message);
        g_clear_error(&error);
    }
}

/* ── Build ──────────────────────────────────────────────────────────────── */

GtkWidget *
build_places_menu(const ClassicMenuConfig *config)
{
    GtkWidget   *submenu  = gtk_menu_new();
    GtkWidget   *sep;
    const gchar *home_dir = g_get_home_dir();

    /* Store the mode in a file-scoped variable so the recursive folder
     * population callbacks can access it without threading it through
     * every call. */
    current_drilldown_mode = config != NULL
        ? config->drilldown_mode
        : DRILLDOWN_FOLDERS_ONLY;

    gtk_menu_set_reserve_toggle_size(GTK_MENU(submenu), FALSE);

    /* ── Home ─────────────────────────────────────────────────────────── */

    append_folder_item(submenu, "Home Folder", "user-home", home_dir);

    /* ── GTK bookmarks ────────────────────────────────────────────────── */

    sep = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(submenu), sep);

    if (!append_gtk_bookmarks(submenu)) {
        /* Fall back to XDG dirs if the bookmarks file is missing */
        gchar *path;

        path = g_build_filename(home_dir, "Desktop",   NULL);
        append_folder_item(submenu, "Desktop",   "user-desktop",    path); g_free(path);

        path = g_build_filename(home_dir, "Documents", NULL);
        append_folder_item(submenu, "Documents", "folder-documents", path); g_free(path);

        path = g_build_filename(home_dir, "Music",     NULL);
        append_folder_item(submenu, "Music",     "folder-music",     path); g_free(path);

        path = g_build_filename(home_dir, "Pictures",  NULL);
        append_folder_item(submenu, "Pictures",  "folder-pictures",  path); g_free(path);

        path = g_build_filename(home_dir, "Videos",    NULL);
        append_folder_item(submenu, "Videos",    "folder-videos",    path); g_free(path);

        path = g_build_filename(home_dir, "Downloads", NULL);
        append_folder_item(submenu, "Downloads", "folder-download",  path); g_free(path);

        sep = gtk_separator_menu_item_new();
        gtk_menu_shell_append(GTK_MENU_SHELL(submenu), sep);
    }

    /* ── Computer / Network ───────────────────────────────────────────── */

    append_computer_item(submenu);
    append_uri_item(submenu, "Network",  "network-workgroup", "network://");
    append_recent_documents_item(submenu);
    {
        gchar *catfish = g_find_program_in_path("catfish");
        if (catfish != NULL) {
            GtkWidget *search = create_menu_item_with_icon(
                    "Search for Files...", "system-search");
            g_signal_connect(search, "activate",
                             G_CALLBACK(on_file_search_activate), NULL);
            gtk_menu_shell_append(GTK_MENU_SHELL(submenu), search);
            g_free(catfish);
        }
    }

    sep = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(submenu), sep);

    append_uri_item(submenu, "Connect to Server...", "network-server", "network://");

    gtk_widget_show_all(submenu);
    return submenu;
}
