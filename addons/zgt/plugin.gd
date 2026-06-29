@tool
extends EditorPlugin

const GDE_EXTENSION_PATH := "res://zgt.gdextension"

var terminal_panel: PanelContainer
var tab_bar: TabBar
var term_host: Control
var search_bar: HBoxContainer
var search_edit: LineEdit
var terminals: Array = []

func _register_settings() -> void:
	_ensure_setting("zgt/terminal/font_path", "", TYPE_STRING,
		PROPERTY_HINT_GLOBAL_FILE, "*.ttf,*.otf")
	_ensure_setting("zgt/terminal/font_size", 14, TYPE_INT,
		PROPERTY_HINT_RANGE, "6,48,1")
	_ensure_setting("zgt/terminal/background_color", Color(0.11, 0.11, 0.13), TYPE_COLOR,
		PROPERTY_HINT_NONE, "")
	_ensure_setting("zgt/terminal/foreground_color", Color(0.85, 0.85, 0.85), TYPE_COLOR,
		PROPERTY_HINT_NONE, "")
	_ensure_setting("zgt/terminal/background_opacity", 1.0, TYPE_FLOAT,
		PROPERTY_HINT_RANGE, "0,1,0.01")
	_ensure_setting("zgt/terminal/palette", PackedColorArray(), TYPE_PACKED_COLOR_ARRAY,
		PROPERTY_HINT_NONE, "")

func _ensure_setting(name: String, default_value, type: int, hint: int, hint_string: String) -> void:
	if not ProjectSettings.has_setting(name):
		ProjectSettings.set_setting(name, default_value)
	ProjectSettings.set_initial_value(name, default_value)
	ProjectSettings.add_property_info({
		"name": name, "type": type, "hint": hint, "hint_string": hint_string,
	})

func _enter_tree() -> void:
	_register_settings()
	if not GDExtensionManager.is_extension_loaded(GDE_EXTENSION_PATH):
		var result = GDExtensionManager.load_extension(GDE_EXTENSION_PATH)
		if result != OK:
			push_error("ZGT: Failed to load GDExtension (error ", result, ")")
			return

	terminal_panel = PanelContainer.new()
	terminal_panel.name = "ZGTerminalPanel"
	terminal_panel.custom_minimum_size = Vector2(0, 300)

	var vbox = VBoxContainer.new()
	vbox.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	vbox.size_flags_vertical = Control.SIZE_EXPAND_FILL
	terminal_panel.add_child(vbox)

	# Top bar: tabs + new-tab + restart.
	var top = HBoxContainer.new()
	tab_bar = TabBar.new()
	tab_bar.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	tab_bar.clip_tabs = true
	tab_bar.tab_close_display_policy = TabBar.CLOSE_BUTTON_SHOW_ALWAYS
	tab_bar.tab_changed.connect(_on_tab_changed)
	tab_bar.tab_close_pressed.connect(_on_tab_close)
	top.add_child(tab_bar)
	var add_btn = Button.new()
	add_btn.text = "+"
	add_btn.tooltip_text = "New terminal"
	add_btn.pressed.connect(_add_tab)
	top.add_child(add_btn)
	var restart_btn = Button.new()
	restart_btn.text = "Restart"
	restart_btn.pressed.connect(_restart)
	top.add_child(restart_btn)
	vbox.add_child(top)

	# Search bar (hidden until Ctrl+Shift+F).
	search_bar = HBoxContainer.new()
	search_bar.visible = false
	var find_label = Label.new()
	find_label.text = "Find:"
	search_bar.add_child(find_label)
	search_edit = LineEdit.new()
	search_edit.placeholder_text = "search scrollback…  (Enter: next · Shift+Enter: prev · Esc: close)"
	search_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	search_edit.text_changed.connect(_on_search_changed)
	search_edit.text_submitted.connect(_on_search_submitted)
	search_edit.gui_input.connect(_on_search_gui_input)
	search_bar.add_child(search_edit)
	var close_btn = Button.new()
	close_btn.text = "✕"
	close_btn.pressed.connect(_close_search)
	search_bar.add_child(close_btn)
	vbox.add_child(search_bar)

	# Host that holds the terminals (only the active one is visible; hidden
	# children take no space, so the visible one fills the area).
	term_host = VBoxContainer.new()
	term_host.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	term_host.size_flags_vertical = Control.SIZE_EXPAND_FILL
	vbox.add_child(term_host)

	_add_tab() # start with one terminal

	add_control_to_bottom_panel(terminal_panel, "ZGT")

func _exit_tree() -> void:
	if terminal_panel:
		remove_control_from_bottom_panel(terminal_panel)
		terminal_panel.queue_free()
	terminals.clear()

func _make_terminal() -> Object:
	var t = ClassDB.instantiate("ZGTerminal")
	if not t:
		push_error("ZGT: Failed to instantiate ZGTerminal class")
		return null
	t.name = "ZGTerminal%d" % terminals.size()
	t.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	t.size_flags_vertical = Control.SIZE_EXPAND_FILL
	term_host.add_child(t)
	if t.has_signal("title_changed"):
		t.title_changed.connect(_on_title_changed.bind(t))
	if t.has_signal("search_requested"):
		t.search_requested.connect(_show_search)
	return t

func _add_tab() -> void:
	var t = _make_terminal()
	if not t:
		return
	terminals.append(t)
	tab_bar.add_tab("Terminal")
	tab_bar.current_tab = terminals.size() - 1
	_update_visibility()

func _on_tab_changed(_idx: int) -> void:
	_update_visibility()

func _on_tab_close(idx: int) -> void:
	if idx < 0 or idx >= terminals.size():
		return
	var t = terminals[idx]
	terminals.remove_at(idx)
	tab_bar.remove_tab(idx)
	if is_instance_valid(t):
		t.queue_free()
	if terminals.is_empty():
		_add_tab() # always keep at least one terminal
	else:
		_update_visibility()

func _update_visibility() -> void:
	var cur = tab_bar.current_tab
	for i in terminals.size():
		var t = terminals[i]
		if is_instance_valid(t):
			t.visible = (i == cur)
	var active = _active()
	if active and active.is_inside_tree():
		active.grab_focus()

func _active() -> Object:
	var cur = tab_bar.current_tab
	if cur >= 0 and cur < terminals.size():
		return terminals[cur]
	return null

func _restart() -> void:
	var t = _active()
	if t:
		t.stop_terminal()
		await get_tree().create_timer(0.3).timeout
		if is_instance_valid(t):
			t.start_terminal()

func _on_title_changed(title: String, term: Object) -> void:
	var idx = terminals.find(term)
	if idx >= 0 and idx < tab_bar.tab_count:
		tab_bar.set_tab_title(idx, title if not title.is_empty() else "Terminal")

func _show_search() -> void:
	if not search_bar:
		return
	search_bar.visible = true
	search_edit.grab_focus()
	search_edit.select_all()
	var t = _active()
	if t and not search_edit.text.is_empty():
		t.search(search_edit.text, true)

func _close_search() -> void:
	if not search_bar:
		return
	search_bar.visible = false
	var t = _active()
	if t:
		t.clear_search()
		t.grab_focus()

func _on_search_changed(s: String) -> void:
	var t = _active()
	if t:
		t.search(s, true)

func _on_search_submitted(s: String) -> void:
	var t = _active()
	if t:
		t.search(s, true)

func _on_search_gui_input(ev: InputEvent) -> void:
	if ev is InputEventKey and ev.pressed:
		if ev.keycode == KEY_ESCAPE:
			_close_search()
			search_edit.accept_event()
		elif (ev.keycode == KEY_ENTER or ev.keycode == KEY_KP_ENTER) and ev.shift_pressed:
			var t = _active()
			if t:
				t.search(search_edit.text, false)
			search_edit.accept_event()
