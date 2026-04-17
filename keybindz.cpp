// keybindz — MangoWM keybinding manager
// Manages ~/.config/mango/keybindz.conf
// Format: bind=MODS,KEY,action,command  (native MangoWM syntax)

#include <gtk/gtk.h>
#include <gdk/gdk.h>

#ifdef GDK_WINDOWING_WAYLAND
#  include <gdk/gdkwayland.h>
#  include <wayland-client.h>
#  include "inhibit-protocol.h"
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// ─── Wayland keyboard shortcuts inhibitor ─────────────────────────────────────
// Prevents the compositor from eating Super/Alt combos while we have focus.

#ifdef GDK_WINDOWING_WAYLAND

static struct wl_registry                                *s_reg  = nullptr;
static struct wl_seat                                    *s_seat = nullptr;
static struct zwp_keyboard_shortcuts_inhibit_manager_v1 *s_mgr  = nullptr;
static struct zwp_keyboard_shortcuts_inhibitor_v1       *s_inh  = nullptr;

static void reg_global(void *, struct wl_registry *reg, uint32_t name,
                        const char *iface, uint32_t ver)
{
    if (!std::strcmp(iface, wl_seat_interface.name)) {
        s_seat = static_cast<struct wl_seat *>(
            wl_registry_bind(reg, name, &wl_seat_interface, std::min(ver, 7u)));
    } else if (!std::strcmp(iface,
               zwp_keyboard_shortcuts_inhibit_manager_v1_interface.name)) {
        s_mgr = static_cast<struct zwp_keyboard_shortcuts_inhibit_manager_v1 *>(
            wl_registry_bind(reg, name,
                &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1));
    }
}
static void reg_global_remove(void *, struct wl_registry *, uint32_t) {}
static const struct wl_registry_listener REG_LISTENER = {
    reg_global, reg_global_remove
};

static void inh_active(void *, struct zwp_keyboard_shortcuts_inhibitor_v1 *)
{ std::cout << "[keybindz] compositor: keyboard inhibitor active\n"; }
static void inh_inactive(void *, struct zwp_keyboard_shortcuts_inhibitor_v1 *)
{ std::cerr << "[keybindz] compositor: keyboard inhibitor inactive (WM may consume keys)\n"; }
static const struct zwp_keyboard_shortcuts_inhibitor_v1_listener INH_LISTENER = {
    inh_active, inh_inactive
};

static void wayland_inhibit(GtkWidget *widget)
{
    GdkDisplay *gdpy = gdk_display_get_default();
    if (!GDK_IS_WAYLAND_DISPLAY(gdpy)) return;

    struct wl_display *wdpy = gdk_wayland_display_get_wl_display(gdpy);

    s_reg = wl_display_get_registry(wdpy);
    wl_registry_add_listener(s_reg, &REG_LISTENER, nullptr);
    wl_display_roundtrip(wdpy);   // fill s_seat, s_mgr
    wl_display_roundtrip(wdpy);   // pick up any deferred events

    if (!s_mgr || !s_seat) {
        std::cerr << "[keybindz] zwp_keyboard_shortcuts_inhibit_manager not available\n";
        return;
    }

    GdkWindow *gwin = gtk_widget_get_window(widget);
    if (!gwin || !GDK_IS_WAYLAND_WINDOW(gwin)) return;

    struct wl_surface *surf = gdk_wayland_window_get_wl_surface(gwin);
    s_inh = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(
        s_mgr, surf, s_seat);
    zwp_keyboard_shortcuts_inhibitor_v1_add_listener(s_inh, &INH_LISTENER, nullptr);
    wl_display_flush(wdpy);
}

#endif // GDK_WINDOWING_WAYLAND

// ─── Data structures ──────────────────────────────────────────────────────────

struct Binding {
    std::string mods;     // "SUPER+SHIFT", "ALT", "none"
    std::string key;      // "Return", "q", "F5", "XF86AudioRaiseVolume"
    bool        shell;    // false → spawn, true → spawn_shell
    std::string command;  // "foot", "bash -c '...'"
};

static std::vector<Binding> s_bindings;
static std::string          s_pending_mods;
static std::string          s_pending_key;

// ─── Widget references ────────────────────────────────────────────────────────

static GtkWidget *s_win         = nullptr;
static GtkWidget *s_listbox     = nullptr;
static GtkWidget *s_cap_entry   = nullptr;
static GtkWidget *s_cmd_entry   = nullptr;
static GtkWidget *s_shell_check = nullptr;
static GtkWidget *s_status      = nullptr;

// ─── Config path ──────────────────────────────────────────────────────────────

static std::string conf_path()
{
    const char *h = std::getenv("HOME");
    return std::string(h ? h : "/root") + "/.config/mango/keybindz.conf";
}

static void set_status(const char *msg)
{
    gtk_label_set_text(GTK_LABEL(s_status), msg);
}

// ─── File I/O ─────────────────────────────────────────────────────────────────
// File format:  bind=MODS,KEY,spawn,COMMAND
//               bind=MODS,KEY,spawn_shell,COMMAND
// where MODS is "SUPER+SHIFT", "ALT", "none", etc. (MangoWM native).

static void load_bindings()
{
    s_bindings.clear();
    std::ifstream f(conf_path());
    if (!f) return;

    for (std::string line; std::getline(f, line); ) {
        if (line.empty() || line[0] == '#') continue;
        if (line.compare(0, 5, "bind=") != 0) continue;

        std::string rest = line.substr(5);

        // Split on the first three commas only; the command may itself contain commas.
        size_t c1 = rest.find(',');
        if (c1 == std::string::npos) continue;
        size_t c2 = rest.find(',', c1 + 1);
        if (c2 == std::string::npos) continue;
        size_t c3 = rest.find(',', c2 + 1);
        if (c3 == std::string::npos) continue;

        std::string mods   = rest.substr(0, c1);
        std::string key    = rest.substr(c1 + 1, c2 - c1 - 1);
        std::string action = rest.substr(c2 + 1, c3 - c2 - 1);
        std::string cmd    = rest.substr(c3 + 1);

        if (action != "spawn" && action != "spawn_shell") continue;
        s_bindings.push_back({ mods, key, (action == "spawn_shell"), cmd });
    }
}

static void save_bindings()
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::path(conf_path()).parent_path(), ec);

    std::ofstream f(conf_path());
    if (!f) { set_status("error: cannot write to file"); return; }

    f << "# keybindz.conf — managed by keybindz\n"
      << "# Load this file by adding to ~/.config/mango/config.conf:\n"
      << "#   include ~/.config/mango/keybindz.conf\n\n";

    for (const auto &b : s_bindings) {
        const char *act = b.shell ? "spawn_shell" : "spawn";
        f << "bind=" << b.mods << "," << b.key << "," << act << ","
          << b.command << "\n";
    }
    set_status("saved  ~/config/mango/keybindz.conf");
}

// ─── Key decoding ─────────────────────────────────────────────────────────────

static bool is_modifier_key(guint kv)
{
    switch (kv) {
        case GDK_KEY_Control_L:    case GDK_KEY_Control_R:
        case GDK_KEY_Shift_L:      case GDK_KEY_Shift_R:
        case GDK_KEY_Alt_L:        case GDK_KEY_Alt_R:
        case GDK_KEY_Super_L:      case GDK_KEY_Super_R:
        case GDK_KEY_Hyper_L:      case GDK_KEY_Hyper_R:
        case GDK_KEY_Meta_L:       case GDK_KEY_Meta_R:
        case GDK_KEY_Caps_Lock:    case GDK_KEY_Num_Lock:
        case GDK_KEY_ISO_Level3_Shift:
            return true;
        default:
            return false;
    }
}

// Returns MangoWM-format modifier string ("SUPER+CTRL", "ALT", "none")
// and the unshifted key name ("Return", "a", "1", "F5").
static void decode_event(GdkEventKey *ev,
                          std::string &out_mods,
                          std::string &out_key)
{
    // Build modifier string
    std::string mods;
    guint state = ev->state & gtk_accelerator_get_default_mod_mask();
    auto append = [&](const char *s) {
        if (!mods.empty()) mods += '+';
        mods += s;
    };
    if (state & GDK_MOD4_MASK)    append("SUPER");
    if (state & GDK_CONTROL_MASK) append("CTRL");
    if (state & GDK_MOD1_MASK)    append("ALT");
    if (state & GDK_SHIFT_MASK)   append("SHIFT");
    out_mods = mods.empty() ? "none" : mods;

    // Resolve the base (level-0) keysym for this hardware key.
    // This gives "a" for Shift+A, "1" for Shift+!, etc.
    guint base_kv = 0;
    GdkKeymap *km = gdk_keymap_get_for_display(gdk_display_get_default());
    if (!gdk_keymap_translate_keyboard_state(
            km, ev->hardware_keycode, (GdkModifierType)0, 0,
            &base_kv, nullptr, nullptr, nullptr))
        base_kv = ev->keyval;

    const char *name = gdk_keyval_name(base_kv);
    out_key = name ? name : "unknown";
}

// ─── List refresh ─────────────────────────────────────────────────────────────

static void refresh_list();

static void on_del(GtkButton *, gpointer idx_ptr)
{
    size_t idx = GPOINTER_TO_UINT(idx_ptr);
    if (idx < s_bindings.size()) {
        s_bindings.erase(s_bindings.begin() + idx);
        refresh_list();
        set_status("removed — click save to persist");
    }
}

static void refresh_list()
{
    GList *ch = gtk_container_get_children(GTK_CONTAINER(s_listbox));
    for (GList *l = ch; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(ch);

    for (size_t i = 0; i < s_bindings.size(); i++) {
        const auto &b = s_bindings[i];

        std::string binding_str = b.mods + "," + b.key;
        const char *type_str    = b.shell ? "[sh]" : "[sp]";

        auto *row  = gtk_list_box_row_new();
        auto *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        auto *lkey = gtk_label_new(binding_str.c_str());
        auto *ltyp = gtk_label_new(type_str);
        auto *lcmd = gtk_label_new(b.command.c_str());
        auto *del  = gtk_button_new_with_label("✕");

        gtk_widget_set_name(hbox, "row-box");
        gtk_widget_set_name(lkey, "row-key");
        gtk_widget_set_name(ltyp, "row-type");
        gtk_widget_set_name(lcmd, "row-cmd");
        gtk_widget_set_name(del,  "btn-del");

        gtk_label_set_xalign(GTK_LABEL(lkey), 0.0);
        gtk_label_set_xalign(GTK_LABEL(ltyp), 0.5);
        gtk_label_set_xalign(GTK_LABEL(lcmd), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(lcmd), PANGO_ELLIPSIZE_END);

        gtk_widget_set_size_request(lkey, 220, -1);
        gtk_widget_set_size_request(ltyp, 44,  -1);
        gtk_widget_set_hexpand(lcmd, TRUE);

        gtk_box_pack_start(GTK_BOX(hbox), lkey, FALSE, FALSE, 10);
        gtk_box_pack_start(GTK_BOX(hbox), ltyp, FALSE, FALSE,  4);
        gtk_box_pack_start(GTK_BOX(hbox), lcmd, TRUE,  TRUE,   6);
        gtk_box_pack_end  (GTK_BOX(hbox), del,  FALSE, FALSE,  6);

        g_signal_connect(del, "clicked", G_CALLBACK(on_del), GUINT_TO_POINTER(i));

        gtk_container_add(GTK_CONTAINER(row), hbox);
        gtk_list_box_insert(GTK_LIST_BOX(s_listbox), row, -1);
    }
    gtk_widget_show_all(s_listbox);
}

// ─── Capture entry events ─────────────────────────────────────────────────────

static gboolean on_cap_key(GtkWidget *, GdkEventKey *ev, gpointer)
{
    if (ev->type != GDK_KEY_PRESS) return FALSE;
    if (is_modifier_key(ev->keyval)) return TRUE;  // consume, wait for real key

    decode_event(ev, s_pending_mods, s_pending_key);
    std::string display = s_pending_mods + "," + s_pending_key;
    gtk_entry_set_text(GTK_ENTRY(s_cap_entry), display.c_str());

    // Auto-move focus to command entry so the user can type the command
    gtk_widget_grab_focus(s_cmd_entry);
    set_status("combo captured — type command, then click add");
    return TRUE;  // prevent the character from being inserted into the entry
}

static gboolean on_cap_focus_in(GtkWidget *, GdkEvent *, gpointer)
{
    gtk_entry_set_text(GTK_ENTRY(s_cap_entry), "");
    set_status("press a key combination...");
    return FALSE;
}

// ─── Button callbacks ─────────────────────────────────────────────────────────

static void on_add(GtkButton *, gpointer)
{
    if (s_pending_key.empty()) {
        set_status("click the binding field and press a key combo first");
        return;
    }
    const char *cmd = gtk_entry_get_text(GTK_ENTRY(s_cmd_entry));
    if (!cmd || !*cmd) {
        set_status("enter a command");
        return;
    }
    bool sh = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s_shell_check));
    s_bindings.push_back({ s_pending_mods, s_pending_key, sh, cmd });

    // Reset capture state
    s_pending_mods.clear();
    s_pending_key.clear();
    gtk_entry_set_text(GTK_ENTRY(s_cap_entry), "");
    gtk_entry_set_text(GTK_ENTRY(s_cmd_entry), "");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s_shell_check), FALSE);

    refresh_list();
    set_status("added — click save to persist");
}

static void on_save(GtkButton *, gpointer) { save_bindings(); }

// ─── Window realize ───────────────────────────────────────────────────────────

static void on_realize(GtkWidget *w, gpointer)
{
#ifdef GDK_WINDOWING_WAYLAND
    wayland_inhibit(w);
#endif
}

// ─── CSS ──────────────────────────────────────────────────────────────────────

static const char CSS[] = R"css(
* {
    background-color: #000000;
    color: #686868;
    font-family: monospace;
    font-size: 13px;
}
window {
    background-color: #000000;
}
#hdr {
    background-color: #000000;
    color: #2e2e2e;
    font-size: 11px;
    padding: 6px 10px 4px 10px;
}
#col-bar {
    background-color: #060606;
    padding: 3px 10px;
    border-bottom: 1px solid #161616;
}
#col-bar label {
    color: #2e2e2e;
    font-size: 11px;
    background-color: #060606;
}
list {
    background-color: #000000;
}
row {
    background-color: #000000;
}
row:hover {
    background-color: #090909;
}
row:selected, row:selected:focus {
    background-color: #0d0d0d;
}
#row-box {
    padding: 4px 0;
    border-bottom: 1px solid #0d0d0d;
    background-color: #000000;
}
#row-key {
    color: #787878;
    background-color: #000000;
}
#row-type {
    color: #333333;
    font-size: 11px;
    background-color: #000000;
}
#row-cmd {
    color: #484848;
    background-color: #000000;
}
#btn-del {
    background-color: transparent;
    color: #252525;
    padding: 0 6px;
    min-width: 0;
    min-height: 0;
    font-size: 11px;
    border: none;
}
#btn-del:hover {
    color: #606060;
    background-color: #111111;
}
entry {
    background-color: #080808;
    color: #888888;
    border: 1px solid #1c1c1c;
    padding: 4px 8px;
    caret-color: #555555;
}
entry:focus {
    border-color: #303030;
    background-color: #0c0c0c;
    color: #aaaaaa;
}
checkbutton {
    color: #484848;
    background-color: #000000;
}
checkbutton check {
    background-color: #080808;
    border: 1px solid #1c1c1c;
    min-width: 12px;
    min-height: 12px;
}
checkbutton:checked {
    color: #686868;
}
checkbutton check:checked {
    background-color: #1e1e1e;
}
#btn-add, #btn-save {
    background-color: #080808;
    color: #585858;
    border: 1px solid #1c1c1c;
    padding: 3px 18px;
}
#btn-add:hover, #btn-save:hover {
    background-color: #0e0e0e;
    color: #909090;
}
#status {
    color: #333333;
    font-size: 11px;
    background-color: #000000;
}
scrolledwindow {
    background-color: #000000;
}
scrollbar {
    background-color: #040404;
    min-width: 5px;
}
scrollbar slider {
    background-color: #1a1a1a;
    min-width: 5px;
    border-radius: 2px;
}
scrollbar slider:hover {
    background-color: #242424;
}
separator {
    background-color: #101010;
    min-height: 1px;
    min-width: 1px;
}
)css";

// ─── UI construction ──────────────────────────────────────────────────────────

static void on_activate(GtkApplication *app, gpointer)
{
    // Apply dark CSS
    auto *css_prov = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css_prov, CSS, -1, nullptr);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css_prov),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css_prov);

    load_bindings();

    s_win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(s_win), "keybindz");
    gtk_window_set_default_size(GTK_WINDOW(s_win), 800, 560);
    gtk_window_set_resizable(GTK_WINDOW(s_win), TRUE);

    g_signal_connect(s_win, "realize", G_CALLBACK(on_realize), nullptr);

    // ── Root container ──
    auto *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(s_win), vbox);

    // ── Title ──
    auto *hdr = gtk_label_new("  keybindz  ·  ~/.config/mango/keybindz.conf");
    gtk_label_set_xalign(GTK_LABEL(hdr), 0.0);
    gtk_widget_set_name(hdr, "hdr");
    gtk_box_pack_start(GTK_BOX(vbox), hdr, FALSE, FALSE, 0);

    // ── Column header bar ──
    auto *col_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_name(col_bar, "col-bar");
    auto *ch_key  = gtk_label_new("BINDING");
    auto *ch_type = gtk_label_new("TYPE");
    auto *ch_cmd  = gtk_label_new("COMMAND");
    gtk_label_set_xalign(GTK_LABEL(ch_key),  0.0);
    gtk_label_set_xalign(GTK_LABEL(ch_type), 0.5);
    gtk_label_set_xalign(GTK_LABEL(ch_cmd),  0.0);
    gtk_widget_set_size_request(ch_key,  220, -1);
    gtk_widget_set_size_request(ch_type, 44,  -1);
    gtk_widget_set_hexpand(ch_cmd, TRUE);
    gtk_box_pack_start(GTK_BOX(col_bar), ch_key,  FALSE, FALSE, 10);
    gtk_box_pack_start(GTK_BOX(col_bar), ch_type, FALSE, FALSE,  4);
    gtk_box_pack_start(GTK_BOX(col_bar), ch_cmd,  TRUE,  TRUE,   6);
    gtk_box_pack_start(GTK_BOX(vbox), col_bar, FALSE, FALSE, 0);

    // ── Scrolled binding list ──
    auto *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);

    s_listbox = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(s_listbox), GTK_SELECTION_NONE);
    gtk_container_add(GTK_CONTAINER(scroll), s_listbox);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(vbox),
        gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);

    // ── Input row: [capture entry] [command entry] [shell checkbox] ──
    auto *inp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start (inp, 10);
    gtk_widget_set_margin_end   (inp, 10);
    gtk_widget_set_margin_top   (inp,  7);
    gtk_widget_set_margin_bottom(inp,  3);

    s_cap_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(s_cap_entry), "click → press combo");
    gtk_widget_set_size_request(s_cap_entry, 220, -1);

    s_cmd_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(s_cmd_entry), "command");
    gtk_widget_set_hexpand(s_cmd_entry, TRUE);

    s_shell_check = gtk_check_button_new_with_label("shell");

    gtk_box_pack_start(GTK_BOX(inp), s_cap_entry,   FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(inp), s_cmd_entry,   TRUE,  TRUE,  0);
    gtk_box_pack_start(GTK_BOX(inp), s_shell_check, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), inp, FALSE, FALSE, 0);

    // ── Button row: [add] [save]  status... ──
    auto *brow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start (brow, 10);
    gtk_widget_set_margin_end   (brow, 10);
    gtk_widget_set_margin_top   (brow,  3);
    gtk_widget_set_margin_bottom(brow,  9);

    auto *add_btn  = gtk_button_new_with_label("add");
    auto *save_btn = gtk_button_new_with_label("save");
    s_status = gtk_label_new("");
    gtk_widget_set_name(add_btn,  "btn-add");
    gtk_widget_set_name(save_btn, "btn-save");
    gtk_widget_set_name(s_status, "status");
    gtk_label_set_xalign(GTK_LABEL(s_status), 0.0);
    gtk_widget_set_hexpand(s_status, TRUE);

    gtk_box_pack_start(GTK_BOX(brow), add_btn,  FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(brow), save_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(brow), s_status, TRUE,  TRUE,  6);
    gtk_box_pack_start(GTK_BOX(vbox), brow, FALSE, FALSE, 0);

    // ── Signal connections ──
    g_signal_connect(s_cap_entry, "key-press-event",
                     G_CALLBACK(on_cap_key),      nullptr);
    g_signal_connect(s_cap_entry, "focus-in-event",
                     G_CALLBACK(on_cap_focus_in), nullptr);
    g_signal_connect(add_btn,  "clicked", G_CALLBACK(on_add),  nullptr);
    g_signal_connect(save_btn, "clicked", G_CALLBACK(on_save), nullptr);

    refresh_list();
    gtk_widget_show_all(s_win);
}

// ─── Entry point ──────────────────────────────────────────────────────────────

int main(int argc, char *argv[])
{
    // "org.keybindz" sets the Wayland app_id to "org.keybindz",
    // used in the MangoWM floating windowrule.
    auto *app = gtk_application_new("org.keybindz", (GApplicationFlags)0);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), nullptr);
    int ret = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return ret;
}
