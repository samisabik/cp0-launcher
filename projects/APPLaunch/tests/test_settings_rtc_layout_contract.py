# SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
#
# SPDX-License-Identifier: MIT

"""Behaviour contracts for Settings -> time and the submenu that hosts it.

* "set manually" is one editor page: LEFT/RIGHT picks a field, UP/DOWN
  changes it, ENTER writes through the privileged TimeSet path, ESC cancels;
* the editor is refused up front while Network Time owns the clock, and
  re-checks that gate before it writes;
* switching Network Time drops any unwritten edit first;
* a nested roller page comes to rest in the right-hand column, so the
  second-level panel and the third-level page never paint over each other.

The checks are static (regex over the sources) so they need no toolchain and
run with the rest of the Python contracts.
"""

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RTC_CPP = (ROOT / "main/ui/settings/settings_rtc_page.cpp").read_text(encoding="utf-8")
RTC_HPP = (ROOT / "main/ui/settings/settings_rtc_page.hpp").read_text(encoding="utf-8")
SUBMENU_CPP = (ROOT / "main/ui/settings/settings_submenu_page.cpp").read_text(
    encoding="utf-8"
)
SUBMENU_HPP = (ROOT / "main/ui/settings/settings_submenu_page.hpp").read_text(
    encoding="utf-8"
)
PAGE_CPP = (ROOT / "main/ui/settings/settings_page.cpp").read_text(encoding="utf-8")
TREE_HPP = (ROOT / "main/ui/settings/settings_tree_types.hpp").read_text(
    encoding="utf-8"
)
COMPONENTS_HPP = (ROOT / "main/ui/settings/lvgl_components.hpp").read_text(
    encoding="utf-8"
)

def strip_comments(source: str) -> str:
    source = re.sub(r"/\*.*?\*/", "", source, flags=re.S)
    return re.sub(r"//[^\n]*", "", source)


def function_body(source: str, signature: str) -> str:
    """Return the body of a definition, with comments removed.

    `signature` must match the definition (not a forward declaration); pass the
    trailing newline and opening brace when a declaration exists too.
    """
    start = source.index(signature)
    opening_brace = source.index("{", start)
    depth = 0
    for index in range(opening_brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return strip_comments(source[opening_brace + 1 : index])
    raise AssertionError(f"unterminated function: {signature}")


def normalized(source: str) -> str:
    return re.sub(r"\s+", " ", source).strip()


def metric(source: str, name: str) -> int:
    match = re.search(rf"\b{name}\s*=\s*(\d+)", source)
    assert match, f"missing layout metric: {name}"
    return int(match.group(1))


def test_nested_roller_page_rests_in_the_right_hand_column():
    assert "int panel_x() const override;" in SUBMENU_HPP

    base_body = normalized(
        function_body(COMPONENTS_HPP, "virtual int panel_x() const")
    )
    assert "return 0;" in base_body

    roller_body = normalized(
        function_body(SUBMENU_CPP, "int LvSettingRollerPage2::panel_x() const")
    )
    assert "return metric(LayoutMetric::PanelX);" in roller_body

    animate = normalized(
        function_body(
            SUBMENU_CPP, "void LvSettingRollerPage2::animate_page3_root(bool entering)"
        )
    )
    # Coming in the page still slides from off-screen right; only the resting
    # position moved.  Regressing this would remove the slide-in animation.
    assert "const int start_x = entering ? metric(LayoutMetric::PageWidth) : rest_x;" in animate
    assert "const int end_x = entering ? rest_x : metric(LayoutMetric::PageWidth);" in animate
    assert "const int rest_x = roller3_->panel_x();" in animate

    finish = normalized(
        function_body(
            SUBMENU_CPP, "void LvSettingRollerPage2::finish_page3_transition(bool entering)"
        )
    )
    assert "lv_obj_set_x(roller3_->Get(), roller3_->panel_x());" in finish


def test_ntp_toggle_is_not_blocked_by_unsaved_edits():
    body = normalized(
        function_body(RTC_CPP, "void settings_rtc_ntp_api(int command, void *data) noexcept")
    )
    discard_at = body.index("session.discard_edits();")
    toggle_at = body.index("begin_ntp_toggle")
    assert discard_at < toggle_at


def test_manual_entry_is_gated_while_network_time_is_on():
    """Entering Set Manually must be refused up front, not field by field."""
    assert "struct ActivationBlock" in TREE_HPP
    assert "std::function<const ActivationBlock *()> activation_gate;" in TREE_HPP

    # The gate runs before any navigation or activation is dispatched.
    handler = normalized(
        function_body(
            SUBMENU_CPP, "void LvSettingRollerPage2::handle_key_event(lv_event_t *event)"
        )
    )
    gate_at = handler.index("selected_node->activation_gate")
    assert gate_at < handler.index("LoadNextPage()")
    assert gate_at < handler.index("selected_node->Componens_api(SettingApiActivate")
    assert "show_blocked_warning(block->title, block->message)" in handler

    # Set Manually carries a gate, and the reason is chosen per NTP state.
    assert (
        'SettingEntry manual_entry{"set manually", settings_rtc_editor_page_factory, PageType::FullCustom};'
        in PAGE_CPP
    )
    assert (
        "manual_entry.activation_gate = [] { return settings_rtc_manual_edit_block(); };"
        in PAGE_CPP
    )
    # Only the Network Time status icon from the parent menu is hidden while
    # the manual editor is open.
    submenu = normalized(SUBMENU_CPP)
    assert 'page3_hides_network_time_icon_ = selected_node->label == "set manually";' in submenu
    transition = normalized(
        function_body(
            SUBMENU_CPP,
            "void LvSettingRollerPage2::start_page3_transition(bool entering)",
        )
    )
    assert "page3_hides_network_time_icon_ && entering" in transition
    finish = normalized(
        function_body(
            SUBMENU_CPP,
            "void LvSettingRollerPage2::finish_page3_transition(bool entering)",
        )
    )
    assert "set_network_time_icon_hidden(false)" in finish
    assert "page3_hides_network_time_icon_ = false" in finish
    icon = normalized(
        function_body(
            SUBMENU_CPP,
            "void LvSettingRollerPage2::set_network_time_icon_hidden(bool hidden)",
        )
    )
    assert 'node->label != "network time"' in icon
    assert "lv_obj_get_child(row, 1)" in icon
    assert "lv_obj_add_flag(status_icon, LV_OBJ_FLAG_HIDDEN)" in icon

    block = normalized(
        function_body(
            RTC_CPP, "const ActivationBlock *settings_rtc_manual_edit_block() noexcept"
        )
    )
    assert "workflow.pending()" in block
    assert "state.ntp_available()" in block
    assert "state.ntp_on()" in block
    # Honest copy per case: an unknown status must not claim Network Time is on.
    assert '"network time is on"' in block
    assert '"network time status unavailable"' in block
    assert "return nullptr;" in block

    # The gate's input is refreshed once per visit, so it cannot consult a stale
    # per-process cache while the field pages read a fresh value.
    assert "settings_rtc_refresh_ntp();" in PAGE_CPP
    refresh = normalized(
        function_body(RTC_CPP, "void settings_rtc_refresh_ntp() noexcept")
    )
    # function_body() strips comments, so this reads refresh_ntp_cache(true).
    assert "refresh_ntp_cache(true)" in refresh


def test_blocked_warning_is_text_driven_with_a_fallback():
    modal = normalized(
        function_body(
            SUBMENU_CPP,
            "void LvSettingRollerPage2::show_blocked_warning(const char *title_text, "
            "const char *message_text)",
        )
    )
    assert "title_text ? title_text" in modal
    assert "message_text ? message_text" in modal


def test_gate_precedes_every_activation_branch_and_does_not_fall_through():
    """A refused activation must not still open the page."""
    handler = normalized(
        function_body(
            SUBMENU_CPP, "void LvSettingRollerPage2::handle_key_event(lv_event_t *event)"
        )
    )
    gate = handler.index("if (selected_node->activation_gate)")
    call = handler.index(
        "if (const ActivationBlock *block = selected_node->activation_gate())"
    )
    show = handler.index("show_blocked_warning(block->title, block->message)")
    factory = handler.index("if (selected_node->page_factory)")
    api = handler.index("else if (selected_node->Componens_api)")
    page = handler.index("else if (on_selected_page)")
    assert gate < call < show < factory < api < page, (gate, call, show, factory, api, page)

    # Between the modal and the first activation branch the key must be
    # consumed, otherwise the page opens anyway.
    consumed = handler[show:factory]
    assert "lv_event_stop_processing(event);" in consumed
    assert "return;" in consumed


def test_blocked_modal_swallows_keys_and_does_not_navigate():
    """While the modal is up it owns every key, and closing it must not navigate."""
    handler = normalized(
        function_body(
            SUBMENU_CPP, "void LvSettingRollerPage2::handle_key_event(lv_event_t *event)"
        )
    )
    modal = handler.index("if (power_warning_)")
    navigation = handler.index("if (key == LV_KEY_ESC || key == LV_KEY_LEFT)")
    assert modal < navigation
    segment = handler[modal:navigation]
    assert "close_power_warning();" in segment
    assert "lv_event_stop_processing(event);" in segment
    assert "LeaveSelfPage" not in segment
    assert "LoadNextPage" not in segment


def test_gate_is_attached_before_the_entry_is_moved_into_the_tree():
    """Setting the gate after the move would store an ungated entry."""
    source = strip_comments(PAGE_CPP)
    body = normalized(
        source[
            source.index("void UISettingTreePage::create_page_detail()") : source.index(
                "void UISettingTreePage::back_home"
            )
        ]
    )
    assert (
        "manual_entry.activation_gate = [] { return settings_rtc_manual_edit_block(); };"
        in body
    )
    assert body.index("manual_entry.activation_gate") < body.index("std::move(manual_entry)")
    assert "mode_tree.append_child(date_time, std::move(manual_entry));" in body


def test_block_reasons_are_static_and_cover_all_three_states():
    """The reasons must outlive the entry (static storage) and be exhaustive."""
    block = normalized(
        function_body(
            RTC_CPP, "const ActivationBlock *settings_rtc_manual_edit_block() noexcept"
        )
    )
    assert block.count("static constexpr ActivationBlock") == 3
    assert "if (workflow.pending()) return &kInFlight;" in block
    assert "if (!state.ntp_available()) return &kUnavailable;" in block
    assert "if (state.ntp_on()) return &kNetworkTimeOn;" in block
    assert "return nullptr;" in block


def test_page2_own_offset_still_matches_page_object_base_x():
    """page2_target_x() reuses panel_x(): that identity only holds while the
    page-2 objects are based at PanelX and nothing derives from the class."""
    base = normalized(
        function_body(
            SUBMENU_CPP, "int LvSettingRollerPage2::page_object_base_x(lv_obj_t *object) const"
        )
    )
    assert (
        "if (object == selection_bg_ || object == ComponensObj) "
        "return metric(LayoutMetric::PanelX);" in base
    )
    assert "return entering ? base_x - panel_x() : base_x;" in normalized(
        function_body(
            SUBMENU_CPP,
            "int LvSettingRollerPage2::page2_target_x(lv_obj_t *object, bool entering) const",
        )
    )
    assert "public LvSettingRollerPage2" not in SUBMENU_HPP
    assert "public LvSettingRollerPage2" not in SUBMENU_CPP


def test_status_hint_records_its_target_without_a_stale_read_back():
    """The page-2 return animation targets the hint's resting x.

    set_status_hint() used to read that x back with lv_obj_get_x() right after
    lv_obj_set_pos(), but an object's coords are not refreshed until the next
    layout pass, so it stored the previous x.  Every open/close of a
    third-level page then animated "ok:enter" towards that stale value, walking
    it one frame further left on each visit.
    """
    body = normalized(
        function_body(
            SUBMENU_CPP,
            "void LvSettingRollerPage2::set_status_hint(const char *text, uint32_t color)",
        )
    )
    assert "const int hint_x =" in body
    assert "hint_base_x_ = hint_x;" in body
    assert "hint_base_x_ = lv_obj_get_x(hint_);" not in body


def test_page3_root_animation_is_started_first_so_its_callback_runs_last():
    """The page-3 root owns the status refresh in finish_page3_transition().

    lv_anim_start() inserts at the head of LVGL's animation list and
    anim_timer() walks from the head, so the last-started animation completes
    first.  Starting the root last made its callback (and the corrected
    set_status_hint() it issues) run before the hint animation's final frame,
    which then overwrote the position with the stale target.
    """
    body = normalized(
        function_body(
            SUBMENU_CPP,
            "void LvSettingRollerPage2::start_page3_transition(bool entering)",
        )
    )
    root_at = body.index("animate_page3_root(entering);")
    assert root_at < body.index("animate_first_page_objects(entering);")
    assert root_at < body.index("animate_object_to(hint_,")


def test_status_polling_does_not_move_hint_during_page3_or_lock_transitions():
    body = normalized(
        function_body(
            SUBMENU_CPP,
            "void LvSettingRollerPage2::set_status_hint(const char *text, uint32_t color)",
        )
    )
    assert "if (roller3_ || page3_transitioning_) return;" in body
    assert body.index("hint_base_x_ = hint_x;") < body.index(
        "if (roller3_ || page3_transitioning_) return;"
    )
    assert body.index("lv_obj_set_pos(hint_, hint_x") > body.index(
        "if (roller3_ || page3_transitioning_) return;"
    )


def test_page3_completion_snaps_hint_after_stale_animation_frames():
    body = normalized(
        function_body(
            SUBMENU_CPP,
            "void LvSettingRollerPage2::finish_page3_transition(bool entering)",
        )
    )
    assert "lv_anim_del(object, nullptr);" in body
    assert "snap(hint_, page2_target_x(hint_, entering));" in body
    assert body.index("snap(hint_,") < body.index("if (entering)")


def test_set_manually_is_one_editor_page():
    assert "class LvSettingTimeEditorPage3" in RTC_CPP
    assert "settings_rtc_editor_page_factory(" in RTC_HPP

    keys = normalized(
        function_body(RTC_CPP, "void handle_key_event(lv_event_t *event)")
    )
    assert "if (key == LV_KEY_ESC) { cancel_request();" in keys
    assert "key == LV_KEY_LEFT && focus_ > 0" in keys
    assert "key == LV_KEY_RIGHT && focus_ + 1 < kFieldCount" in keys
    assert "key == LV_KEY_UP) { step(1);" in keys
    assert "key == LV_KEY_DOWN) { step(-1);" in keys
    assert "key == LV_KEY_ENTER) { write();" in keys

    fields = normalized(RTC_CPP)
    for field in ("YEAR", "MONTH", "DAY", "HOUR", "MINUTE"):
        assert f"settings_rtc::RtcField::{field}" in fields
    assert '{"year", "month", "day", "hour", "min"}' in fields


def test_editor_writes_through_the_privileged_time_path():
    write = normalized(function_body(RTC_CPP, "void write()"))
    assert write.index("settings_rtc_manual_edit_block()") < write.index("begin_time_commit()")
    assert "state.edit_field(settings_rtc::RtcField::SECOND, 0);" in write
    assert "settings_rtc::set_time_async(" in write
    assert "SettingsAsync::Dispatch::enqueue_from_callback(token" in write

    submit = normalized(
        function_body(RTC_CPP, "int set_time_async(std::string timestamp, PrivilegedCallback callback, RequestStartedCallback started)")
    )
    assert '{"TimeSet", std::move(timestamp)}' in submit
    assert "cp0_signal_system_admin_async(" in normalized(
        function_body(RTC_CPP, "int submit_privileged(")
    )

    # Leaving cancels the sudo request and abandons the edit.
    destructor = normalized(function_body(RTC_CPP, "~LvSettingTimeEditorPage3() override"))
    assert "cancel_request();" in destructor
    assert "settings_rtc::session().discard_edits();" in destructor


def test_up_and_down_wrap_within_the_field_range():
    step = normalized(function_body(RTC_CPP, "void step(int delta)"))
    assert "if (value > maximum) value = minimum;" in step
    assert "if (value < minimum) value = maximum;" in step
    # The model clamps the day to the month that is actually selected.
    edit = normalized(
        function_body(RTC_CPP, "bool RtcStateModel::edit_field(RtcField field, int value) noexcept")
    )
    assert "const int maximum_day = field_max(RtcField::DAY);" in edit


if __name__ == "__main__":
    # Auto-discover instead of keeping a hand-written list: a list that falls
    # behind the definitions silently skips tests, which is how two of these
    # (including the activation-gate one) once ran nowhere.
    for _name, _test in sorted(globals().items()):
        if _name.startswith("test_") and callable(_test):
            _test()
