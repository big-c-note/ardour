/*
    Copyright (C) 2019 Nikolaus Gullotta

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <iostream>
#include <stdio.h>

#include "ardour/filesystem_paths.h"
#include "ardour/session_state_utils.h"
#include "ardour/session_directory.h"

#include <glib.h>
#include <glibmm.h>
#include <glibmm/datetime.h>
#include <glibmm/fileutils.h>

#include <gtkmm/table.h>
#include <gtkmm/filechooserdialog.h>

#include "editor.h"
#include "mixer_snapshot_dialog.h"
#include "utils.h"

#include "pbd/basename.h"
#include "pbd/file_utils.h"
#include "pbd/gstdio_compat.h"
#include "pbd/i18n.h"

#include "widgets/tooltips.h"
#include "widgets/choice.h"
#include "widgets/prompter.h"

using namespace Gtk;
using namespace PBD;
using namespace std;
using namespace ARDOUR;
using namespace ArdourWidgets;
using namespace ARDOUR_UI_UTILS;

struct ColumnInfo {
    int           index;
    int           sort_idx;
    AlignmentEnum al;
    const char*   label;
    const char*   tooltip;
};

MixerSnapshotDialog::MixerSnapshotDialog(Session* s)
    : ArdourWindow(_("Mixer Snapshot Manager:"))
{
    global_model = Gtk::ListStore::create(_columns);
    local_model  = Gtk::ListStore::create(_columns);

    bootstrap_display_and_model(global_display, global_scroller, global_model, true);
    bootstrap_display_and_model(local_display, local_scroller, local_model,  false);

    add(top_level_view_box);

    vector<TargetEntry> target_table;
    target_table.push_back(TargetEntry("string"));

    global_display.drag_dest_set(target_table);
    local_display.drag_dest_set(target_table);

    global_display.signal_drag_data_received().connect(sigc::bind(sigc::mem_fun(*this, &MixerSnapshotDialog::display_drag_data_received), true));
    local_display.signal_drag_data_received().connect(sigc::bind(sigc::mem_fun(*this, &MixerSnapshotDialog::display_drag_data_received), false));

    global_display.signal_button_press_event().connect(sigc::bind(sigc::mem_fun(*this, &MixerSnapshotDialog::button_press), true), false);
    local_display.signal_button_press_event().connect(sigc::bind(sigc::mem_fun(*this, &MixerSnapshotDialog::button_press), false), false);

    // signal_show().connect(sigc::mem_fun(*this, &MixerSnapshotDialog::window_opened));

    set_session(s);
}

void MixerSnapshotDialog::set_session(Session* s)
{
    if(s) {
        ArdourWindow::set_session(s);
    }

    refill();
}

void MixerSnapshotDialog::ensure_directory(bool global)
{
    string path;
    if(global) {
        path = Glib::build_filename(user_config_directory(-1), "mixer_snapshots");
    } else {
        path = Glib::build_filename(_session->session_directory().root_path(), "mixer_snapshots");
    }

    if(!Glib::file_test(path.c_str(), Glib::FILE_TEST_EXISTS & Glib::FILE_TEST_IS_DIR)) {
        ::g_mkdir(path.c_str(), 0775);
    }
}

void MixerSnapshotDialog::window_opened()
{
    refill();
}

void MixerSnapshotDialog::display_drag_data_received(const Glib::RefPtr<Gdk::DragContext>& context, int x, int y, const Gtk::SelectionData& data, guint info, guint time, bool global)
{
    if (data.get_target() != "string") {
        context->drag_finish(false, false, time);
        return;
    }

    const void* d = data.get_data();
    const Gtkmm2ext::DnDTreeView<string>* tv = reinterpret_cast<const Gtkmm2ext::DnDTreeView<string>*>(d);

    bool ok = false;
    if(tv) {
        list<string> paths;
        TreeView* source;
        tv->get_object_drag_data(paths, &source);

        if(!paths.empty()) {
            ensure_directory(global);
        }

        for (list<string>::const_iterator i = paths.begin(); i != paths.end(); i++) {
            string new_path;
            if(global) {
                new_path = Glib::build_filename(user_config_directory(-1), "mixer_snapshots", basename((*i).c_str()));
            } else {
                new_path = Glib::build_filename(_session->session_directory().root_path(), "mixer_snapshots", basename((*i).c_str()));
            }
            ::g_rename((*i).c_str(), new_path.c_str());
        }
        ok = true;
    }

    context->drag_finish(ok, false, time);
    refill();
}

bool MixerSnapshotDialog::button_press(GdkEventButton* ev, bool global)
{
    if (ev->button == 3) {

        Gtk::TreeModel::Path path;
        Gtk::TreeViewColumn* col;
        int cx;
        int cy;

        TreeModel::iterator iter;
        if(global) {
            global_display.get_path_at_pos ((int) ev->x, (int) ev->y, path, col, cx, cy);
            iter = global_model->get_iter(path);
        } else {
            local_display.get_path_at_pos ((int) ev->x, (int) ev->y, path, col, cx, cy);
            iter = local_model->get_iter(path);
        }

        if (iter) {
            TreeModel::Row row = *(iter);
            popup_context_menu(ev->button, ev->time, row[_columns.full_path]);
            return true;
        }
    };

    if (ev->type == GDK_2BUTTON_PRESS) {

        TreeModel::iterator iter;
        if(global) {
            iter = global_display.get_selection()->get_selected();
        } else {
            iter = local_display.get_selection()->get_selected();
        }

        global_display.get_selection()->unselect_all();
        local_display.get_selection()->unselect_all();

        if(iter) {
            MixerSnapshot* s = (*iter)[_columns.snapshot];
            s->recall();
            return true;
        }
    }
    return false;
}

void MixerSnapshotDialog::popup_context_menu(int btn, int64_t time, string path)
{
    using namespace Menu_Helpers;
    MenuList& items(menu.items());
    items.clear();
    add_item_with_sensitivity(items, MenuElem(_("Recall"), sigc::bind(sigc::mem_fun(*this, &MixerSnapshotDialog::load_snapshot), path)), true);
    add_item_with_sensitivity(items, MenuElem(_("Rename..."), sigc::bind(sigc::mem_fun(*this, &MixerSnapshotDialog::rename_snapshot), path)), true);
    add_item_with_sensitivity(items, MenuElem(_("Remove"), sigc::bind(sigc::mem_fun(*this, &MixerSnapshotDialog::remove_snapshot), path)), true);
    menu.popup(btn, time);
}

void MixerSnapshotDialog::remove_snapshot(const string path)
{
    ::g_remove(path.c_str());
    refill();
}

void MixerSnapshotDialog::load_snapshot(const string path)
{
    MixerSnapshot n = MixerSnapshot(_session, path);
    n.recall();
}

void MixerSnapshotDialog::rename_snapshot(const string old_path)
{
    string dir_name  = Glib::path_get_dirname(old_path);

    Prompter prompt(true);
    prompt.set_name("Rename MixerSnapshot Prompter");
    prompt.set_title(_("New Snapshot Name:"));
    prompt.add_button(Stock::SAVE, RESPONSE_ACCEPT);
    prompt.set_prompt(_("Rename Mixer Snapshot:"));
    prompt.set_initial_text(basename_nosuffix(old_path));

    if (prompt.run() == RESPONSE_ACCEPT) {
        string new_label;
        prompt.get_result(new_label);
        if (new_label.length() > 0) {
            string new_path = Glib::build_filename(dir_name, new_label + ".xml");
            ::g_rename(old_path.c_str(), new_path.c_str());
            refill();
        }
    }
}

bool MixerSnapshotDialog::bootstrap_display_and_model(Gtkmm2ext::DnDTreeView<string>& display, Gtk::ScrolledWindow& scroller, Glib::RefPtr<ListStore> model, bool global)
{
    if(!model) {
        return false;
    }

    display.set_model(model);

    display.append_column(_("Fav"),            _columns.favorite);
    display.append_column(_("Name"),           _columns.name);
    display.append_column(_("# Tracks"),       _columns.n_tracks);
    display.append_column(_("# VCAs"),         _columns.n_vcas);
    display.append_column(_("# Groups"),       _columns.n_groups);
    display.append_column(_("Special Tracks"), _columns.has_specials);
    display.append_column(_("Date"),           _columns.date);
    display.append_column(_("Version"),        _columns.version);

    //newest snaps should be at the top
    model->set_sort_column(6, SORT_DESCENDING);

    //flag setting columns

    /* dumb work around here because we're doing an #ifdef MIXBUS -
       Basically, append_column() returns the no. of columns *after*
       appending, we can just put this in a vector and use it later */
    vector<int> column_counts {
#ifdef MIXBUS
        display.append_column(_("EQ"),     _columns.recall_eq),
        display.append_column(_("Comp"),   _columns.recall_comp),
#endif
        display.append_column(_("I/O"),    _columns.recall_io),
        display.append_column(_("Groups"), _columns.recall_groups),
        display.append_column(_("VCAs"),   _columns.recall_vcas),
    };

    for(vector<int>::iterator i = column_counts.begin(); i != column_counts.end(); i++) {
        int index = (*i) - 1; //the actual index at the time of appending
        CellRendererToggle* cell = dynamic_cast<CellRendererToggle*>(display.get_column_cell_renderer(index));
        string col_title = display.get_column(index)->get_title();
        cell->property_activatable() = true;
        cell->property_radio() = true;
        cell->signal_toggled().connect(sigc::bind(sigc::mem_fun(*this, &MixerSnapshotDialog::recall_flag_cell_action), global, col_title));
    }

    display.set_headers_visible(true);
    display.set_headers_clickable(true);
    display.set_reorderable(false);
    display.set_rules_hint(true);
    display.add_object_drag(_columns.full_path.index(), "string");
    display.set_drag_column(_columns.name.index());

    CellRendererToggle* fav_cell = dynamic_cast<CellRendererToggle*>(display.get_column_cell_renderer(0));
    fav_cell->property_activatable() = true;
    fav_cell->property_radio() = true;
    fav_cell->signal_toggled().connect(sigc::bind(sigc::mem_fun(*this, &MixerSnapshotDialog::fav_cell_action), global));

    HBox* add_remove = manage(new HBox);
    Button* btn_add  = manage(new Button("New"));
    Button* btn_new  = manage(new Button("New From Session"));
    add_remove->pack_start(*btn_add,  true, true, 50);
    add_remove->pack_start(*btn_new, true, true, 45);

    VBox* vbox = manage(new VBox);
    vbox->set_homogeneous();
    vbox->pack_start(*add_remove);
    vbox->set_size_request(800, -1);

    btn_add->signal_clicked().connect(sigc::bind(sigc::mem_fun(*this, &MixerSnapshotDialog::new_snapshot), global));
    btn_new->signal_clicked().connect(sigc::bind(sigc::mem_fun(*this, &MixerSnapshotDialog::new_snap_from_session), global));

    scroller.set_border_width(10);
    scroller.set_policy(POLICY_AUTOMATIC, POLICY_AUTOMATIC);
    scroller.add(display);

    Table* table = manage(new Table(3, 3));
    table->set_size_request(-1, 400);
    table->attach(scroller,        0, 3, 0, 5                         );
    table->attach(*vbox,           2, 3, 6, 8, FILL|EXPAND, FILL, 5, 5);
    top_level_view_box.pack_start(*table);

    ColumnInfo ci[] = {
        { 0,  0,  ALIGN_CENTER,  _("Favorite"),       _("") },
        { 1,  1,  ALIGN_LEFT,    _("Name"),           _("") },
        { 2,  2,  ALIGN_CENTER,  _("# Tracks"),       _("") },
        { 3,  3,  ALIGN_CENTER,  _("# VCAs"),         _("") },
        { 4,  4,  ALIGN_CENTER,  _("# Groups"),       _("") },
        { 5,  5,  ALIGN_CENTER,  _("Special Tracks"), _("") },
        { 6,  8,  ALIGN_LEFT,    _("Date"),           _("") },
        { 7,  7,  ALIGN_LEFT,    _("Version"),        _("") },
        { -1,-1,  ALIGN_CENTER, 0, 0 }
    };

    for (int i = 0; ci[i].index >= 0; ++i) {
        ColumnInfo info = ci[i];

        TreeViewColumn* col = display.get_column(info.index);

        Label* label = manage(new Label (info.label));
        label->set_alignment(info.al);
        set_tooltip(*label, info.tooltip);
        col->set_widget(*label);
        label->show();

        col->set_sort_column(info.sort_idx);
        col->set_expand(false);
        col->set_alignment(info.al);

        //...and this sets the alignment for the data cells
        CellRendererText* rend = dynamic_cast<CellRendererText*>(display.get_column_cell_renderer(info.index));
        if (rend) {
            rend->property_xalign() = (info.al == ALIGN_RIGHT ? 1.0 : (info.al == ALIGN_LEFT ? 0.0 : 0.5));
        }
    }
    return true;
}

void MixerSnapshotDialog::new_snapshot(bool global)
{
    if(!_session) {
        return;
    }

    MixerSnapshot* snap = new MixerSnapshot(_session);

    Prompter prompt(true);
    prompt.set_name("New Mixer Snapshot Prompter");
    prompt.set_title(_("Mixer Snapshot Name:"));
    prompt.add_button(Stock::SAVE, RESPONSE_ACCEPT);
    prompt.set_prompt(_("Set Mixer Snapshot Name"));
    prompt.set_initial_text(_session->name());

    RouteList rl = PublicEditor::instance().get_selection().tracks.routelist();

    CheckButton* sel = new CheckButton(_("Selected Tracks Only:"));
    sel->set_active(!rl.empty());
    sel->show();
    prompt.get_vbox()->pack_start(*sel);

    if(prompt.run() == RESPONSE_ACCEPT) {
        ensure_directory(global);
        string new_label;
        prompt.get_result(new_label);
        if (new_label.length() > 0) {
            snap->set_label(new_label);

            string path = "";
            if(global) {
                path = Glib::build_filename(user_config_directory(-1), "mixer_snapshots", snap->get_label() + ".xml");
            } else {
                path = Glib::build_filename(_session->session_directory().root_path(), "mixer_snapshots", snap->get_label() + ".xml");
            }

            if(!rl.empty() && sel->get_active()) {
                snap->snap(rl);
            } else {
                snap->snap();
            }

            snap->write(path);
            refill();
        }
    }
}

void MixerSnapshotDialog::new_snap_from_session(bool global)
{
    Gtk::FileChooserDialog session_selector(_("Open Session"), FILE_CHOOSER_ACTION_OPEN);
    string session_parent_dir = Glib::path_get_dirname(_session->path());
    session_selector.add_button(Stock::CANCEL, RESPONSE_CANCEL);
    session_selector.add_button(Stock::OPEN, RESPONSE_ACCEPT);
    session_selector.set_current_folder(session_parent_dir);

    int response = session_selector.run();
    session_selector.hide();

    if (response == RESPONSE_CANCEL) {
        return;
    }

    ensure_directory(global);

    string session_path = session_selector.get_filename();

    MixerSnapshot* snapshot = new MixerSnapshot(_session, session_path);

    snapshot->set_label(basename_nosuffix(session_path));

    string path = "";
    if(global) {
        path = Glib::build_filename(user_config_directory(-1), "mixer_snapshots", snapshot->get_label() + ".xml");
    } else {
        path = Glib::build_filename(_session->session_directory().root_path(), "mixer_snapshots", snapshot->get_label() + ".xml");
    }

    snapshot->write(path);
    refill();
}

void MixerSnapshotDialog::refill()
{
    global_model->clear();

    string global_directory = Glib::build_filename(user_config_directory(-1), "mixer_snapshots");

    vector<string> files;
    find_files_matching_pattern(files, global_directory, "*.xml");

    for(vector<string>::iterator i = files.begin(); i != files.end(); i++) {
        string path = *(i);
        string name = basename_nosuffix(*(i));

        MixerSnapshot* snap = new MixerSnapshot(_session, path);
        snap->set_label(name);

        TreeModel::Row row = *(global_model->append());
        if (name.length() > 48) {
            name = name.substr (0, 48);
            name.append("...");
        }

        row[_columns.name]         = name;
        row[_columns.favorite]     = snap->get_favorite();
        row[_columns.version]      = snap->get_last_modified_with();
        row[_columns.n_tracks]     = snap->get_routes().size();
        row[_columns.n_vcas]       = snap->get_vcas().size();
        row[_columns.n_groups]     = snap->get_groups().size();
        row[_columns.has_specials] = snap->has_specials();

        GStatBuf gsb;
        g_stat(path.c_str(), &gsb);
        Glib::DateTime gdt(Glib::DateTime::create_now_local(gsb.st_ctime));

        row[_columns.timestamp] = gsb.st_ctime;
        row[_columns.date]      = gdt.format ("%F %H:%M");
        row[_columns.full_path] = path;
        row[_columns.snapshot]  = snap;

#ifdef MIXBUS
        row[_columns.recall_eq]     = snap->get_recall_eq();
        row[_columns.recall_comp]   = snap->get_recall_comp();
#endif
        row[_columns.recall_io]     = snap->get_recall_io();
        row[_columns.recall_groups] = snap->get_recall_group();
        row[_columns.recall_vcas]   = snap->get_recall_vca();
    }

    local_model->clear();
    files.clear();

    string local_directory = Glib::build_filename(_session->session_directory().root_path(), "mixer_snapshots");
    find_files_matching_pattern(files, local_directory, "*.xml");

    for(vector<string>::iterator i = files.begin(); i != files.end(); i++) {
        string path = *(i);
        string name = basename_nosuffix(*(i));

        MixerSnapshot* snap = new MixerSnapshot(_session, path);
        snap->set_label(name);

        TreeModel::Row row = *(local_model->append());
        if (name.length() > 48) {
            name = name.substr (0, 48);
            name.append("...");
        }

        row[_columns.name]         = name;
        row[_columns.favorite]     = snap->get_favorite();
        row[_columns.version]      = snap->get_last_modified_with();
        row[_columns.n_tracks]     = snap->get_routes().size();
        row[_columns.n_vcas]       = snap->get_vcas().size();
        row[_columns.n_groups]     = snap->get_groups().size();
        row[_columns.has_specials] = snap->has_specials();

        GStatBuf gsb;
        g_stat(path.c_str(), &gsb);
        Glib::DateTime gdt(Glib::DateTime::create_now_local(gsb.st_ctime));

        row[_columns.timestamp] = gsb.st_ctime;
        row[_columns.date]      = gdt.format ("%F %H:%M");
        row[_columns.full_path] = path;
        row[_columns.snapshot]  = snap;

#ifdef MIXBUS
        row[_columns.recall_eq]     = snap->get_recall_eq();
        row[_columns.recall_comp]   = snap->get_recall_comp();
#endif
        row[_columns.recall_io]     = snap->get_recall_io();
        row[_columns.recall_groups] = snap->get_recall_group();
        row[_columns.recall_vcas]   = snap->get_recall_vca();
    }
}

void MixerSnapshotDialog::fav_cell_action(const string& path, bool global)
{
    TreeModel::iterator iter;
    if(global) {
        iter = global_model->get_iter(path);
    } else {
        iter = local_model->get_iter(path);
    }

    if(iter) {
        MixerSnapshot* snap = (*iter)[_columns.snapshot];
        snap->set_favorite(!snap->get_favorite());
        (*iter)[_columns.favorite] = snap->get_favorite();
        snap->write((*iter)[_columns.full_path]);
    }
}

void MixerSnapshotDialog::recall_flag_cell_action(const std::string& path, bool global, string title)
{
    TreeModel::iterator iter;
    if(global) {
        iter = global_model->get_iter(path);
    } else {
        iter = local_model->get_iter(path);
    }

    if(iter) {
        MixerSnapshot* snap = (*iter)[_columns.snapshot];

#ifdef MIXBUS
        if(title == "EQ") {
            snap->set_recall_eq(!snap->get_recall_eq());
            (*iter)[_columns.recall_eq] = snap->get_recall_eq();
        }

        if(title == "Comp") {
            snap->set_recall_comp(!snap->get_recall_comp());
            (*iter)[_columns.recall_comp] = snap->get_recall_comp();
        }
#endif
        if(title == "I/O") {
                snap->set_recall_io(!snap->get_recall_io());
                (*iter)[_columns.recall_io] = snap->get_recall_io();
        }

        if(title == "Groups") {
            snap->set_recall_group(!snap->get_recall_group());
            (*iter)[_columns.recall_groups] = snap->get_recall_group();
        }

        if(title == "VCAs") {
            snap->set_recall_vca(!snap->get_recall_vca());
            (*iter)[_columns.recall_vcas] = snap->get_recall_vca();
        }

        snap->write((*iter)[_columns.full_path]);
    }
}