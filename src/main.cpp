#include "backend.h"
#include "glib_ptr.h"

#include <adwaita.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <exception>
#include <memory>
#include <string_view>
#include <utility>

namespace
{

// Window-owned state shared by GTK callbacks and the request worker.
struct View
{
    GtkWindow* window;
    GListStore* rows;
    GtkCustomFilter* filter;
    GtkSingleSelection* selection;
    GtkSearchEntry* search;
    GtkLabel* status;
    GtkWidget* status_box;
    GtkWidget* retry;
    GtkLabel* counts;
    GtkWidget* refresh;
    std::array<GtkWidget*, 4> actions;
    std::string query;
    Backend backend;
    ObjectPtr<GCancellable> cancellation{g_cancellable_new()};
    GtkColumnViewColumn* command_line_column;
    GtkSorter* cpu_sorter;
    GtkSorter* memory_sorter;
    GtkWidget* command_line_toggle;
    ServiceSamples previous_samples;
    guint metrics_timer = 0;
    guint snapshot_revision = 0;
    bool sampling = false;
    bool busy = false;
    bool running_only = false;
    bool show_system_services = false;

    ~View()
    {
        if (metrics_timer)
            g_source_remove(metrics_timer);
    }
};

constexpr std::array actions{Action::start, Action::stop, Action::enable, Action::disable};

// GLib invokes this once it is finished with a C++ value transferred to it.
template <typename T> void delete_data(gpointer data)
{
    delete static_cast<T*>(data);
}

// Each list item carries the full unit data used by sorting, filtering, and actions.
UnitRow* get_row(gconstpointer object)
{
    return static_cast<UnitRow*>(
        g_object_get_data(G_OBJECT(const_cast<gpointer>(object)), "unit-row"));
}

const UnitRow* selected_row(const View& view)
{
    auto* item = gtk_single_selection_get_selected_item(view.selection);
    return item ? get_row(item) : nullptr;
}

void update_buttons(View& view)
{
    // Start and Stop use live state. Enable and Disable use the unit's startup state.
    const auto* row = selected_row(view);
    const bool selected = row && !view.busy;
    const std::string_view active = row ? std::string_view(row->active) : "";
    const std::string_view startup = row ? std::string_view(row->startup) : "";
    gtk_widget_set_sensitive(view.refresh, !view.busy);
    gtk_widget_set_sensitive(view.command_line_toggle, !view.busy);
    gtk_widget_set_sensitive(view.actions[0], selected && !startup.starts_with("masked") &&
                                                  active != "active" && active != "activating");
    gtk_widget_set_sensitive(
        view.actions[1],
        selected && (active == "active" || active == "activating" || active == "reloading"));
    gtk_widget_set_sensitive(view.actions[2],
                             selected && (startup == "disabled" || startup == "enabled-runtime" ||
                                          startup == "indirect"));
    gtk_widget_set_sensitive(view.actions[3],
                             selected && (startup == "enabled" || startup == "enabled-runtime"));
}

GtkWidget* make_cell_label()
{
    auto* label = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    return label;
}

const char* usage_cell_key(Column type)
{
    return type == Column::cpu ? "cpu-cell" : "memory-cell";
}

void set_usage_cell(GtkLabel* label, const ServiceUsage& usage, Column type)
{
    const auto text = type == Column::cpu ? format_cpu_usage(usage.cpu_percent)
                                          : format_memory_usage(usage.memory_bytes);
    gtk_label_set_text(label, text.c_str());
}

GtkColumnViewColumn* append_column(GtkColumnView* view, const char* title, Column type)
{
    const auto column_type = GINT_TO_POINTER(static_cast<int>(type));
    // GTK reuses cells as the list scrolls. Setup creates widgets and bind fills each row.
    auto* factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup",
                     G_CALLBACK(+[](GtkSignalListItemFactory*, GtkListItem* item, gpointer data)
                                {
                                    auto* label = make_cell_label();
                                    GtkWidget* cell = label;
                                    if (static_cast<Column>(GPOINTER_TO_INT(data)) == Column::name)
                                    {
                                        cell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
                                        auto* description = make_cell_label();
                                        gtk_widget_add_css_class(description, "dim-label");
                                        gtk_box_append(GTK_BOX(cell), label);
                                        gtk_box_append(GTK_BOX(cell), description);
                                    }
                                    gtk_widget_set_margin_top(cell, 6);
                                    gtk_widget_set_margin_bottom(cell, 6);
                                    gtk_list_item_set_child(item, cell);
                                }),
                     column_type);
    g_signal_connect(
        factory, "bind",
        G_CALLBACK(+[](GtkSignalListItemFactory*, GtkListItem* item, gpointer data)
                   {
                       const auto& row = *get_row(gtk_list_item_get_item(item));
                       const auto type = static_cast<Column>(GPOINTER_TO_INT(data));
                       auto* cell = gtk_list_item_get_child(item);
                       std::string text;
                       switch (type)
                       {
                       case Column::pid:
                           text = row.pid ? std::to_string(row.pid) : "—";
                           break;
                       case Column::name:
                           text = row.name;
                           if (!row.executable.empty())
                               text += "  ·  " + row.executable;
                           gtk_label_set_text(GTK_LABEL(gtk_widget_get_first_child(cell)),
                                              text.c_str());
                           gtk_label_set_text(GTK_LABEL(gtk_widget_get_last_child(cell)),
                                              row.description.c_str());
                           gtk_widget_set_visible(gtk_widget_get_last_child(cell),
                                                  !row.description.empty());
                           if (!row.description.empty())
                               text += "\n" + row.description;
                           gtk_widget_set_tooltip_text(cell, text.c_str());
                           return;
                       case Column::command_line:
                           text = row.command_line.empty() ? "—" : row.command_line;
                           break;
                       case Column::startup:
                           text = row.startup;
                           break;
                       case Column::status:
                           text = row.active;
                           if (!row.substate.empty() && row.substate != row.active)
                               text += " (" + row.substate + ")";
                           break;
                       case Column::cpu:
                       case Column::memory:
                           // Keep a reference to the visible cell until GTK recycles it.
                           g_object_set_data(G_OBJECT(gtk_list_item_get_item(item)),
                                             usage_cell_key(type), cell);
                           gtk_label_set_xalign(GTK_LABEL(cell), 1.0F);
                           set_usage_cell(GTK_LABEL(cell), row.usage, type);
                           return;
                       }
                       gtk_label_set_text(GTK_LABEL(cell), text.c_str());
                       gtk_widget_set_tooltip_text(cell, text.c_str());
                   }),
        column_type);
    if (type == Column::cpu || type == Column::memory)
    {
        g_signal_connect(
            factory, "unbind",
            G_CALLBACK(+[](GtkSignalListItemFactory*, GtkListItem* item, gpointer data)
                       {
                           auto* object = G_OBJECT(gtk_list_item_get_item(item));
                           const auto key =
                               usage_cell_key(static_cast<Column>(GPOINTER_TO_INT(data)));
                           if (g_object_get_data(object, key) == gtk_list_item_get_child(item))
                               g_object_set_data(object, key, nullptr);
                       }),
            column_type);
    }
    const ObjectPtr<GtkColumnViewColumn> column(
        gtk_column_view_column_new(title, GTK_LIST_ITEM_FACTORY(factory)));
    const ObjectPtr<GtkCustomSorter> sorter(gtk_custom_sorter_new(
        +[](gconstpointer left, gconstpointer right, gpointer data)
        {
            return compare_rows(*get_row(left), *get_row(right),
                                static_cast<Column>(GPOINTER_TO_INT(data)));
        },
        column_type, nullptr));
    gtk_column_view_column_set_sorter(column.get(), GTK_SORTER(sorter.get()));
    gtk_column_view_column_set_resizable(column.get(), true);
    gtk_column_view_column_set_expand(column.get(), type == Column::name);
    gtk_column_view_append_column(view, column.get());
    if (type == Column::name)
        gtk_column_view_sort_by_column(view, column.get(), GTK_SORT_ASCENDING);
    if (type == Column::command_line)
        gtk_column_view_column_set_fixed_width(column.get(), 360);
    if (type == Column::cpu || type == Column::memory)
        gtk_column_view_column_set_fixed_width(column.get(), 106);
    return column.get();
}

void select_unit(View& view, const std::string& selected)
{
    if (selected.empty())
        return;
    auto* model = gtk_single_selection_get_model(view.selection);
    for (guint index = 0; index < g_list_model_get_n_items(model); ++index)
    {
        const ObjectPtr<GObject> item(static_cast<GObject*>(g_list_model_get_item(model, index)));
        if (get_row(item.get())->name == selected)
        {
            gtk_single_selection_set_selected(view.selection, index);
            break;
        }
    }
}

void replace_rows(View& view, std::vector<UnitRow>& rows, const std::string& selected)
{
    // Replace the snapshot in one update, then restore selection by unit name.
    // The selected position may have changed after sorting or filtering.
    const GlibPtr<GPtrArray, g_ptr_array_unref> objects(
        g_ptr_array_new_full(static_cast<guint>(rows.size()), g_object_unref));
    for (auto& row : rows)
    {
        ObjectPtr<GtkStringObject> object(gtk_string_object_new(row.name.c_str()));
        auto data = std::make_unique<UnitRow>(std::move(row));
        g_object_set_data_full(G_OBJECT(object.get()), "unit-row", data.release(),
                               delete_data<UnitRow>);
        g_ptr_array_add(objects.get(), object.release());
    }
    g_list_store_splice(view.rows, 0, g_list_model_get_n_items(G_LIST_MODEL(view.rows)),
                        objects->pdata, objects->len);

    select_unit(view, selected);
}

void update_usage(View& view, ServiceSamples samples)
{
    bool cpu_changed = false;
    bool memory_changed = false;
    auto* rows = G_LIST_MODEL(view.rows);
    for (guint index = 0; index < g_list_model_get_n_items(rows); ++index)
    {
        const ObjectPtr<GObject> item(static_cast<GObject*>(g_list_model_get_item(rows, index)));
        auto& row = *get_row(item.get());
        const auto current = samples.find(row.name);
        const auto previous = view.previous_samples.find(row.name);
        ServiceUsage usage{};
        if (is_running_service(row) && current != samples.end())
        {
            usage = measure_usage(current->second, previous == view.previous_samples.end()
                                                       ? nullptr
                                                       : &previous->second);
        }
        const bool new_cpu = row.usage.cpu_percent != usage.cpu_percent;
        const bool new_memory = row.usage.memory_bytes != usage.memory_bytes;
        row.usage = usage;
        if (new_cpu)
        {
            if (auto* label = g_object_get_data(item.get(), usage_cell_key(Column::cpu)))
                set_usage_cell(GTK_LABEL(label), usage, Column::cpu);
            cpu_changed = true;
        }
        if (new_memory)
        {
            if (auto* label = g_object_get_data(item.get(), usage_cell_key(Column::memory)))
                set_usage_cell(GTK_LABEL(label), usage, Column::memory);
            memory_changed = true;
        }
    }
    view.previous_samples = std::move(samples);

    // Sorting by live usage can move rows. Keep the selected unit the same.
    const auto* row = selected_row(view);
    const auto selected = row ? row->name : std::string{};
    if (cpu_changed)
        gtk_sorter_changed(view.cpu_sorter, GTK_SORTER_CHANGE_DIFFERENT);
    if (memory_changed)
        gtk_sorter_changed(view.memory_sorter, GTK_SORTER_CHANGE_DIFFERENT);
    if (!selected_row(view) || selected_row(view)->name != selected)
        select_unit(view, selected);
}

void sample_usage(View& view)
{
    if (view.busy || view.sampling)
        return;
    auto* app = gtk_window_get_application(view.window);
    if (!app)
        return;
    view.sampling = true;
    const ObjectPtr<GTask> task(g_task_new(
        view.window, view.cancellation.get(),
        +[](GObject* source, GAsyncResult* done, gpointer application)
        {
            const GlibPtr<GApplication, g_application_release> hold(G_APPLICATION(application));
            auto& view = *static_cast<View*>(g_object_get_data(source, "view"));
            view.sampling = false;
            const std::unique_ptr<ServiceSamples> samples(
                static_cast<ServiceSamples*>(g_task_propagate_pointer(G_TASK(done), nullptr)));
            const auto revision = GPOINTER_TO_UINT(g_task_get_task_data(G_TASK(done)));
            if (gtk_window_get_application(view.window) && !view.busy &&
                revision == view.snapshot_revision)
                update_usage(view, samples ? std::move(*samples) : ServiceSamples{});
        },
        app));
    g_task_set_task_data(task.get(), GUINT_TO_POINTER(view.snapshot_revision), nullptr);
    g_application_hold(G_APPLICATION(app));
    g_task_run_in_thread(
        task.get(),
        +[](GTask* task, gpointer, gpointer, GCancellable* cancellation)
        {
            try
            {
                auto samples =
                    std::make_unique<ServiceSamples>(sample_running_services(cancellation));
                g_task_return_pointer(task, samples.release(), delete_data<ServiceSamples>);
            }
            catch (const std::exception& error)
            {
                g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", error.what());
            }
        });
}

void show_status(View& view, const char* message, bool can_retry = false)
{
    gtk_label_set_text(view.status, message);
    gtk_widget_set_visible(view.retry, can_retry);
    gtk_widget_set_visible(view.status_box, *message != '\0');
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
    show_status(view, action == Action::refresh ? "Loading units…" : "Applying action…");
    // Keep the app alive while a systemd request is in flight, even if the window closes.
    auto* app = gtk_window_get_application(view.window);
    const ObjectPtr<GTask> task(g_task_new(
        view.window, view.cancellation.get(),
        +[](GObject* source, GAsyncResult* done, gpointer application)
        {
            // Balance the application's hold even if the window closed during the task.
            const GlibPtr<GApplication, g_application_release> hold(G_APPLICATION(application));
            auto& view = *static_cast<View*>(g_object_get_data(source, "view"));
            GError* error = nullptr;
            std::unique_ptr<Result> result(
                static_cast<Result*>(g_task_propagate_pointer(G_TASK(done), &error)));
            const ErrorPtr task_error(error);
            const auto* request = static_cast<const Request*>(g_task_get_task_data(G_TASK(done)));
            // The task can finish after the window was closed. Skip updates in that case.
            if (!gtk_window_get_application(view.window))
                return;
            if (task_error)
                show_status(view, task_error->message);
            else if (result->authorization_cancelled)
                show_status(view, result->action_message.c_str(), true);
            else
            {
                // Count the full snapshot before any display filters hide rows.
                const auto running = std::ranges::count_if(result->rows, is_running_service);
                const auto counts = "Total units: " + std::to_string(result->rows.size()) +
                                    "  ·  Running services: " + std::to_string(running);
                gtk_label_set_text(view.counts, counts.c_str());
                replace_rows(view, result->rows, request->unit);
                ++view.snapshot_revision;
                show_status(view, result->action_message.c_str());
            }
            view.busy = false;
            update_buttons(view);
            if (result && !result->authorization_cancelled && !view.metrics_timer)
            {
                view.metrics_timer = g_timeout_add(
                    2000,
                    +[](gpointer data) -> gboolean
                    {
                        sample_usage(*static_cast<View*>(data));
                        return G_SOURCE_CONTINUE;
                    },
                    &view);
                sample_usage(view);
            }
        },
        app));
    // Capture the selected unit name on the UI thread before handing the request to the worker.
    auto request = std::make_unique<Request>(
        action, row ? row->name : "",
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(view.command_line_toggle)) != FALSE);
    g_task_set_task_data(task.get(), request.release(), delete_data<Request>);
    g_application_hold(G_APPLICATION(app));
    // Perform blocking systemd calls in a worker. Only the completion callback updates GTK.
    g_task_run_in_thread(
        task.get(),
        +[](GTask* task, gpointer source, gpointer data, GCancellable* cancellation)
        {
            const auto& request = *static_cast<Request*>(data);
            auto& view = *static_cast<View*>(g_object_get_data(G_OBJECT(source), "view"));
            try
            {
                auto result = std::make_unique<Result>(view.backend.run(request, cancellation));
                g_task_return_pointer(task, result.release(), delete_data<Result>);
            }
            catch (const std::exception& error)
            {
                g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", error.what());
            }
        });
}

void activate(GApplication* app, gpointer)
{
    // Activating the launcher again brings the existing window to the foreground.
    if (auto* existing = gtk_application_get_active_window(GTK_APPLICATION(app)))
    {
        gtk_window_present(existing);
        return;
    }

    auto* window = GTK_WINDOW(adw_application_window_new(GTK_APPLICATION(app)));
    gtk_window_set_title(window, "Service Panel");
    gtk_window_set_default_size(window, 940, 640);
    auto state = std::make_unique<View>();
    auto* view = state.get();
    view->window = window;
    g_object_set_data_full(G_OBJECT(window), "view", state.release(), delete_data<View>);

    g_signal_connect(window, "close-request",
                     G_CALLBACK(+[](GtkWindow*, gpointer data) -> gboolean
                                {
                                    auto& view = *static_cast<View*>(data);
                                    if (view.metrics_timer)
                                    {
                                        g_source_remove(view.metrics_timer);
                                        view.metrics_timer = 0;
                                    }
                                    g_cancellable_cancel(view.cancellation.get());
                                    return FALSE;
                                }),
                     view);

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
    g_signal_connect_swapped(view->refresh, "clicked",
                             G_CALLBACK(+[](View* view)
                                        {
                                            run(*view, Action::refresh);
                                        }),
                             view);

    // Apply display filters before sorting and selecting rows.
    view->rows = g_list_store_new(GTK_TYPE_STRING_OBJECT);
    view->filter = gtk_custom_filter_new(
        +[](gpointer item, gpointer data) -> gboolean
        {
            const auto& view = *static_cast<View*>(data);
            const auto& row = *get_row(item);
            return (!view.running_only || is_running_service(row)) &&
                   (view.show_system_services || !is_systemd_unit(row)) &&
                   matches_query(row, view.query);
        },
        view, nullptr);
    auto* filtered = gtk_filter_list_model_new(G_LIST_MODEL(view->rows), GTK_FILTER(view->filter));
    auto* sorted = gtk_sort_list_model_new(G_LIST_MODEL(filtered), nullptr);
    view->selection = gtk_single_selection_new(G_LIST_MODEL(sorted));
    auto* columns = GTK_COLUMN_VIEW(gtk_column_view_new(GTK_SELECTION_MODEL(view->selection)));
    gtk_sort_list_model_set_sorter(sorted, gtk_column_view_get_sorter(columns));
    gtk_single_selection_set_autoselect(view->selection, false);
    gtk_single_selection_set_can_unselect(view->selection, true);
    g_signal_connect_swapped(view->selection, "notify::selected-item",
                             G_CALLBACK(+[](View* view)
                                        {
                                            update_buttons(*view);
                                        }),
                             view);
    g_signal_connect_swapped(
        view->search, "search-changed",
        G_CALLBACK(+[](View* view)
                   {
                       view->query = casefold(gtk_editable_get_text(GTK_EDITABLE(view->search)));
                       gtk_filter_changed(GTK_FILTER(view->filter), GTK_FILTER_CHANGE_DIFFERENT);
                   }),
        view);

    gtk_column_view_set_show_row_separators(columns, true);
    append_column(columns, "PID", Column::pid);
    append_column(columns, "Name / Executable", Column::name);
    append_column(columns, "Enabled", Column::startup);
    append_column(columns, "Run Status", Column::status);
    view->cpu_sorter =
        gtk_column_view_column_get_sorter(append_column(columns, "CPU Usage", Column::cpu));
    view->memory_sorter =
        gtk_column_view_column_get_sorter(append_column(columns, "RAM Usage", Column::memory));
    view->command_line_column = append_column(columns, "Command Line", Column::command_line);
    gtk_column_view_column_set_visible(view->command_line_column, false);

    view->command_line_toggle = gtk_toggle_button_new_with_label("Command Line");
    gtk_widget_set_tooltip_text(view->command_line_toggle, "Show arguments of running services");
    adw_header_bar_pack_end(header, view->command_line_toggle);
    g_signal_connect_swapped(view->command_line_toggle, "toggled",
                             G_CALLBACK(+[](View* view)
                                        {
                                            gtk_column_view_column_set_visible(
                                                view->command_line_column,
                                                gtk_toggle_button_get_active(
                                                    GTK_TOGGLE_BUTTON(view->command_line_toggle)));
                                            run(*view, Action::refresh);
                                        }),
                             view);

    auto* scroller = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroller, true);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), GTK_WIDGET(columns));
    gtk_box_append(GTK_BOX(content), scroller);

    view->status = GTK_LABEL(gtk_label_new(nullptr));
    gtk_label_set_xalign(view->status, 0.0F);
    gtk_label_set_wrap(view->status, true);
    gtk_label_set_selectable(view->status, true);
    gtk_widget_set_hexpand(GTK_WIDGET(view->status), true);
    view->status_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(view->status_box, 12);
    gtk_widget_set_margin_end(view->status_box, 12);
    gtk_widget_set_margin_bottom(view->status_box, 6);
    gtk_widget_set_visible(view->status_box, false);
    gtk_box_append(GTK_BOX(view->status_box), GTK_WIDGET(view->status));
    view->retry = gtk_button_new_with_label("Try Again");
    gtk_widget_set_valign(view->retry, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(view->status_box), view->retry);
    gtk_box_append(GTK_BOX(content), view->status_box);
    g_signal_connect_swapped(view->retry, "clicked",
                             G_CALLBACK(+[](View* view)
                                        {
                                            run(*view, Action::refresh);
                                        }),
                             view);

    auto* footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(footer, 12);
    gtk_widget_set_margin_end(footer, 12);
    gtk_widget_set_margin_top(footer, 10);
    gtk_widget_set_margin_bottom(footer, 10);
    gtk_box_append(GTK_BOX(content), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(content), footer);

    auto* running_only = gtk_check_button_new_with_label("Show running services only");
    gtk_box_append(GTK_BOX(footer), running_only);
    g_signal_connect(running_only, "toggled",
                     G_CALLBACK(+[](GtkCheckButton* button, gpointer data)
                                {
                                    auto& view = *static_cast<View*>(data);
                                    view.running_only = gtk_check_button_get_active(button);
                                    gtk_filter_changed(GTK_FILTER(view.filter),
                                                       view.running_only
                                                           ? GTK_FILTER_CHANGE_MORE_STRICT
                                                           : GTK_FILTER_CHANGE_LESS_STRICT);
                                }),
                     view);

    auto* show_system = gtk_check_button_new_with_label("Show system services");
    gtk_widget_set_tooltip_text(show_system, "Show units whose names start with systemd-");
    gtk_box_append(GTK_BOX(footer), show_system);
    g_signal_connect(show_system, "toggled",
                     G_CALLBACK(+[](GtkCheckButton* button, gpointer data)
                                {
                                    auto& view = *static_cast<View*>(data);
                                    view.show_system_services = gtk_check_button_get_active(button);
                                    gtk_filter_changed(GTK_FILTER(view.filter),
                                                       view.show_system_services
                                                           ? GTK_FILTER_CHANGE_LESS_STRICT
                                                           : GTK_FILTER_CHANGE_MORE_STRICT);
                                }),
                     view);

    view->counts = GTK_LABEL(gtk_label_new("Total units: —  ·  Running services: —"));
    gtk_label_set_xalign(view->counts, 0.0F);
    gtk_widget_set_hexpand(GTK_WIDGET(view->counts), true);
    gtk_widget_set_tooltip_text(GTK_WIDGET(view->counts),
                                "Counts are based on the full list before filtering.");
    gtk_box_append(GTK_BOX(footer), GTK_WIDGET(view->counts));

    constexpr std::array labels{"Start", "Stop", "Enable", "Disable"};
    for (std::size_t index = 0; index < actions.size(); ++index)
    {
        auto* button = gtk_button_new_with_label(labels[index]);
        view->actions[index] = button;
        gtk_box_append(GTK_BOX(footer), button);
        g_object_set_data(G_OBJECT(button), "action-index", GUINT_TO_POINTER(index));
        g_signal_connect(button, "clicked",
                         G_CALLBACK(+[](GtkButton* button, gpointer data)
                                    {
                                        const auto index = GPOINTER_TO_UINT(
                                            g_object_get_data(G_OBJECT(button), "action-index"));
                                        run(*static_cast<View*>(data), actions[index]);
                                    }),
                         view);
    }
    gtk_window_present(window);
    run(*view, Action::refresh);
}

} // namespace

int main(int argc, char** argv)
{
    // The elevated helper handles requests without starting a GTK session.
    if (argc == 2 && std::string_view(argv[1]) == "--backend")
        return run_backend();

    const ObjectPtr<AdwApplication> app(
        adw_application_new("local.ServicePanel", geteuid() == 0 ? G_APPLICATION_NON_UNIQUE
                                                                 : G_APPLICATION_DEFAULT_FLAGS));
    g_signal_connect(app.get(), "activate", G_CALLBACK(activate), nullptr);
    return g_application_run(G_APPLICATION(app.get()), argc, argv);
}
