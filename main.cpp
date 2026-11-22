#include "systemd.hpp"

#include <adwaita.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <exception>
#include <memory>
#include <string_view>
#include <utility>

namespace {

struct View {
    GtkWindow* window;
    GListStore* rows;
    GtkCustomFilter* filter;
    GtkSingleSelection* selection;
    GtkSearchEntry* search;
    GtkLabel* status;
    GtkWidget* refresh;
    std::array<GtkWidget*, 4> actions;
    std::string query;
    bool busy = false;
};

struct Request {
    Action action;
    std::string unit;
};

struct Result {
    std::vector<UnitRow> rows;
    std::string action_message;
};

enum class Column { pid, name, startup, status };

constexpr std::array actions{Action::start, Action::stop, Action::enable, Action::disable};

UnitRow* get_row(gpointer object)
{
    return static_cast<UnitRow*>(g_object_get_data(G_OBJECT(object), "unit-row"));
}

const UnitRow* selected_row(const View& view)
{
    auto* item = gtk_single_selection_get_selected_item(view.selection);
    return item ? get_row(item) : nullptr;
}

void update_buttons(View& view)
{
    const auto* row = selected_row(view);
    const bool selected = row && !view.busy;
    const std::string_view active = row ? std::string_view(row->active) : "";
    const std::string_view startup = row ? std::string_view(row->startup) : "";
    gtk_widget_set_sensitive(view.refresh, !view.busy);
    gtk_widget_set_sensitive(view.actions[0], selected && !startup.starts_with("masked")
                             && active != "active" && active != "activating");
    gtk_widget_set_sensitive(view.actions[1], selected && (active == "active"
                             || active == "activating" || active == "reloading"));
    gtk_widget_set_sensitive(view.actions[2], selected && (startup == "disabled"
                             || startup == "enabled-runtime" || startup == "indirect"));
    gtk_widget_set_sensitive(view.actions[3], selected && (startup == "enabled"
                             || startup == "enabled-runtime"));
}

void append_column(GtkColumnView* view, const char* title, Column type)
{
    auto* factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(+[](GtkSignalListItemFactory*,
                                                      GtkListItem* item, gpointer) {
        auto* label = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        gtk_widget_set_margin_top(label, 6);
        gtk_widget_set_margin_bottom(label, 6);
        gtk_list_item_set_child(item, label);
    }), nullptr);
    g_signal_connect(factory, "bind", G_CALLBACK(+[](GtkSignalListItemFactory*,
                                                     GtkListItem* item, gpointer data) {
        const auto& row = *get_row(gtk_list_item_get_item(item));
        const auto type = static_cast<Column>(GPOINTER_TO_INT(data));
        auto* label = GTK_LABEL(gtk_list_item_get_child(item));
        std::string text;
        switch (type) {
        case Column::pid: text = row.pid ? std::to_string(row.pid) : "—"; break;
        case Column::name:
            text = row.name;
            if (!row.executable.empty())
                text += "  ·  " + row.executable;
            break;
        case Column::startup: text = row.startup; break;
        case Column::status:
            text = row.active;
            if (!row.substate.empty() && row.substate != row.active)
                text += " (" + row.substate + ")";
            break;
        }
        gtk_label_set_text(label, text.c_str());
        gtk_widget_set_tooltip_text(GTK_WIDGET(label), text.c_str());
    }), GINT_TO_POINTER(static_cast<int>(type)));
    auto* column = gtk_column_view_column_new(title, GTK_LIST_ITEM_FACTORY(factory));
    gtk_column_view_column_set_resizable(column, true);
    gtk_column_view_column_set_expand(column, type == Column::name);
    gtk_column_view_append_column(view, column);
    g_object_unref(column);
}

void replace_rows(View& view, std::vector<UnitRow>& rows, const std::string& selected)
{
    std::vector<gpointer> objects;
    objects.reserve(rows.size());
    for (auto& row : rows) {
        auto* object = gtk_string_object_new(row.name.c_str());
        g_object_set_data_full(G_OBJECT(object), "unit-row", new UnitRow(std::move(row)),
                               +[](gpointer data) { delete static_cast<UnitRow*>(data); });
        objects.push_back(object);
    }
    g_list_store_splice(view.rows, 0, g_list_model_get_n_items(G_LIST_MODEL(view.rows)),
                       objects.data(), static_cast<guint>(objects.size()));
    for (auto* object : objects)
        g_object_unref(object);

    auto* model = gtk_single_selection_get_model(view.selection);
    for (guint index = 0; !selected.empty() && index < g_list_model_get_n_items(model); ++index) {
        auto* item = g_list_model_get_item(model, index);
        const bool matches = get_row(item)->name == selected;
        g_object_unref(item);
        if (matches) {
            gtk_single_selection_set_selected(view.selection, index);
            break;
        }
    }
}

void run(View& view, Action action)
{
    if (view.busy)
        return;
    auto* row = selected_row(view);
    if (action != Action::refresh && !row)
        return;

    view.busy = true;
    update_buttons(view);
    gtk_label_set_text(view.status, action == Action::refresh ? "Loading units…" : "Applying action…");
    auto* app = gtk_window_get_application(view.window);
    g_application_hold(G_APPLICATION(app));
    auto* task = g_task_new(view.window, nullptr, +[](GObject* source, GAsyncResult* done,
                                                     gpointer application) {
        auto& view = *static_cast<View*>(g_object_get_data(source, "view"));
        GError* error = nullptr;
        std::unique_ptr<Result> result(static_cast<Result*>(
            g_task_propagate_pointer(G_TASK(done), &error)));
        auto* request = static_cast<Request*>(g_task_get_task_data(G_TASK(done)));
        if (!gtk_window_get_application(view.window)) {
            g_clear_error(&error);
            g_application_release(G_APPLICATION(application));
            return;
        }
        if (error) {
            gtk_label_set_text(view.status, error->message);
            g_error_free(error);
        } else {
            replace_rows(view, result->rows, request->unit);
            if (!result->action_message.empty()) {
                gtk_label_set_text(view.status, result->action_message.c_str());
            } else {
                const auto count = std::to_string(result->rows.size()) + " units";
                gtk_label_set_text(view.status, count.c_str());
            }
        }
        view.busy = false;
        update_buttons(view);
        g_application_release(G_APPLICATION(application));
    }, app);
    g_task_set_task_data(task, new Request{action, row ? row->name : ""},
                         +[](gpointer data) { delete static_cast<Request*>(data); });
    g_task_run_in_thread(task, +[](GTask* task, gpointer, gpointer data, GCancellable*) {
        const auto& request = *static_cast<Request*>(data);
        auto result = std::make_unique<Result>();
        try {
            result->action_message = change_unit(request.action, request.unit);
        } catch (const std::exception& error) {
            result->action_message = error.what();
        }
        try {
            result->rows = list_units();
            g_task_return_pointer(task, result.release(),
                                  +[](gpointer data) { delete static_cast<Result*>(data); });
        } catch (const std::exception& error) {
            std::string message = "Could not refresh units. ";
            message += error.what();
            if (!result->action_message.empty())
                message = result->action_message + "\n" + message;
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", message.c_str());
        }
    });
    g_object_unref(task);
}

std::string casefold(const char* value)
{
    std::unique_ptr<char, decltype(&g_free)> folded(g_utf8_casefold(value, -1), g_free);
    return folded.get();
}

void activate(GApplication* app, gpointer)
{
    if (auto* existing = gtk_application_get_active_window(GTK_APPLICATION(app))) {
        gtk_window_present(existing);
        return;
    }

    auto* window = GTK_WINDOW(adw_application_window_new(GTK_APPLICATION(app)));
    gtk_window_set_title(window, "Service Manager");
    gtk_window_set_default_size(window, 940, 640);
    auto* view = new View{};
    view->window = window;
    g_object_set_data_full(G_OBJECT(window), "view", view,
                           +[](gpointer data) { delete static_cast<View*>(data); });

    auto* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), content);
    auto* header = ADW_HEADER_BAR(adw_header_bar_new());
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(header));

    view->search = GTK_SEARCH_ENTRY(gtk_search_entry_new());
    gtk_widget_set_size_request(GTK_WIDGET(view->search), 240, -1);
    adw_header_bar_set_title_widget(header, GTK_WIDGET(view->search));
    view->refresh = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(view->refresh, "Refresh units");
    adw_header_bar_pack_start(header, view->refresh);
    g_signal_connect_swapped(view->refresh, "clicked", G_CALLBACK(+[](View* view) {
        run(*view, Action::refresh);
    }), view);

    view->rows = g_list_store_new(GTK_TYPE_STRING_OBJECT);
    view->filter = gtk_custom_filter_new(+[](gpointer item, gpointer data) -> gboolean {
        const auto& view = *static_cast<View*>(data);
        const auto& row = *get_row(item);
        return view.query.empty()
            || casefold(row.name.c_str()).find(view.query) != std::string::npos
            || casefold(row.executable.c_str()).find(view.query) != std::string::npos;
    }, view, nullptr);
    auto* filtered = gtk_filter_list_model_new(G_LIST_MODEL(view->rows), GTK_FILTER(view->filter));
    view->selection = gtk_single_selection_new(G_LIST_MODEL(filtered));
    gtk_single_selection_set_autoselect(view->selection, false);
    gtk_single_selection_set_can_unselect(view->selection, true);
    g_signal_connect_swapped(view->selection, "notify::selected-item",
                             G_CALLBACK(+[](View* view) { update_buttons(*view); }), view);
    g_signal_connect_swapped(view->search, "search-changed", G_CALLBACK(+[](View* view) {
        view->query = casefold(gtk_editable_get_text(GTK_EDITABLE(view->search)));
        gtk_filter_changed(GTK_FILTER(view->filter), GTK_FILTER_CHANGE_DIFFERENT);
    }), view);

    auto* columns = GTK_COLUMN_VIEW(gtk_column_view_new(GTK_SELECTION_MODEL(view->selection)));
    gtk_column_view_set_show_row_separators(columns, true);
    append_column(columns, "PID", Column::pid);
    append_column(columns, "Name / Executable", Column::name);
    append_column(columns, "Enabled", Column::startup);
    append_column(columns, "Run Status", Column::status);

    auto* scroller = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroller, true);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), GTK_WIDGET(columns));
    gtk_box_append(GTK_BOX(content), scroller);

    auto* footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(footer, 12);
    gtk_widget_set_margin_end(footer, 12);
    gtk_widget_set_margin_top(footer, 10);
    gtk_widget_set_margin_bottom(footer, 10);
    gtk_box_append(GTK_BOX(content), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(content), footer);

    view->status = GTK_LABEL(gtk_label_new(nullptr));
    gtk_label_set_xalign(view->status, 0.0F);
    gtk_label_set_wrap(view->status, true);
    gtk_label_set_selectable(view->status, true);
    gtk_widget_set_hexpand(GTK_WIDGET(view->status), true);
    gtk_box_append(GTK_BOX(footer), GTK_WIDGET(view->status));

    constexpr std::array labels{"Start", "Stop", "Enable", "Disable"};
    for (std::size_t index = 0; index < actions.size(); ++index) {
        auto* button = gtk_button_new_with_label(labels[index]);
        view->actions[index] = button;
        gtk_box_append(GTK_BOX(footer), button);
        g_object_set_data(G_OBJECT(button), "action-index", GUINT_TO_POINTER(index));
        g_signal_connect(button, "clicked", G_CALLBACK(+[](GtkButton* button, gpointer data) {
            const auto index = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "action-index"));
            run(*static_cast<View*>(data), actions[index]);
        }), view);
    }
    gtk_window_present(window);
    run(*view, Action::refresh);
}

} // namespace

int main(int argc, char** argv)
{
    if (geteuid() != 0) {
        std::fputs("Service Manager requires root. Launch it with sudo from your GNOME terminal.\n",
                   stderr);
        return 1;
    }

    auto* app = adw_application_new("local.ServiceManager", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), nullptr);
    const int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
