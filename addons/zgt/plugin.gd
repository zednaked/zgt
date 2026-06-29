@tool
extends EditorPlugin

const GDE_EXTENSION_PATH := "res://zgt.gdextension"

var terminal_panel: PanelContainer
var terminal: Object
var title_label: Label

func _enter_tree() -> void:
	if not GDExtensionManager.is_extension_loaded(GDE_EXTENSION_PATH):
		var result = GDExtensionManager.load_extension(GDE_EXTENSION_PATH)
		if result != OK:
			push_error("ZGT: Failed to load GDExtension (error ", result, ")")
			return

	terminal_panel = PanelContainer.new()
	terminal_panel.name = "ZGTerminalPanel"
	# Open at a usable height instead of fully collapsed.
	terminal_panel.custom_minimum_size = Vector2(0, 300)

	var vbox = VBoxContainer.new()
	vbox.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	vbox.size_flags_vertical = Control.SIZE_EXPAND_FILL
	terminal_panel.add_child(vbox)

	terminal = ClassDB.instantiate("ZGTerminal")
	if not terminal:
		push_error("ZGT: Failed to instantiate ZGTerminal class")
		return
	terminal.name = "ZGTerminal"
	terminal.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	terminal.size_flags_vertical = Control.SIZE_EXPAND_FILL
	vbox.add_child(terminal)
	if terminal.has_signal("title_changed"):
		terminal.title_changed.connect(_on_title_changed)

	var hb = HBoxContainer.new()
	var restart_btn = Button.new()
	restart_btn.text = "Restart"
	restart_btn.pressed.connect(_restart)
	hb.add_child(restart_btn)
	title_label = Label.new()
	title_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	title_label.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
	hb.add_child(title_label)
	vbox.add_child(hb)

	add_control_to_bottom_panel(terminal_panel, "ZGT")

func _exit_tree() -> void:
	if terminal_panel:
		remove_control_from_bottom_panel(terminal_panel)
		terminal_panel.queue_free()

func _restart() -> void:
	if terminal:
		terminal.stop_terminal()
		await get_tree().create_timer(0.3).timeout
		terminal.start_terminal()

func _on_title_changed(title: String) -> void:
	if title_label:
		title_label.text = title
		title_label.tooltip_text = title
