# SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
# SPDX-License-Identifier: MIT

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SETTINGS_PAGE = (ROOT / "main/ui/settings/settings_page.cpp").read_text()
ETHERNET_CONTROLLER = (
    ROOT / "main/ui/settings/settings_ethernet_controller.cpp"
).read_text()
SETTINGS_ROLLER = (ROOT / "main/ui/settings/settings_menu_roller.hpp").read_text()
WIFI_PAGE = (ROOT / "main/ui/settings/settings_wifi_page.cpp").read_text()
SETTINGS_ADAPTER = (ROOT / "main/ui/settings/settings_adapter.cpp").read_text()
BUILTIN_REGISTRY = (ROOT / "main/ui/builtin_app_registry.cpp").read_text()
PREINSTALLED_MANIFEST = (ROOT / "APPLaunch/preinstalled-desktop-apps.tsv").read_text()

EXPECTED_ROOT_ORDER = [
    "screen",
    "sound",
    "wi-fi",
    "bluetooth",
    "ethernet",
    "tailscale",
    "ext port",
    "battery",
    "apps",
    "time",
    "system",
]


def settings_tree_builder() -> str:
    start = SETTINGS_PAGE.index("void UISettingTreePage::create_page_detail()")
    end = SETTINGS_PAGE.index("void UISettingTreePage::back_home", start)
    body = SETTINGS_PAGE[start:end]
    return re.sub(r"//.*", "", body)


def test_root_menu_matches_product_order():
    root_labels = re.findall(
        r'mode_tree\.append_child\(\s*root,\s*SettingEntry\{"([^"]+)"',
        settings_tree_builder(),
    )
    assert root_labels == EXPECTED_ROOT_ORDER


def test_screen_remains_the_initial_selection():
    assert "selected_index = 0;" in SETTINGS_ROLLER
    assert "selected_index = 2;" not in SETTINGS_ROLLER


def test_screen_keeps_lock_timeout_options():
    builder = settings_tree_builder()
    screen_entries = re.findall(
        r'append_child\(\s*screen,\s*SettingEntry\{"([^"]+)"', builder
    )
    assert screen_entries == ["brightness", "auto-lock"]
    timeouts = re.findall(
        r'append_child\(\s*dark_time,\s*SettingEntry\{"([^"]+)"', builder
    )
    assert timeouts == ["never", "10 s", "30 s", "1 min", "5 min"]


def test_sound_holds_volume_and_mic_gain():
    builder = settings_tree_builder()
    sound_entries = re.findall(r'append_child\(\s*sound,\s*SettingEntry\{"([^"]+)"', builder)
    assert sound_entries == ["volume", "mic gain", "system sounds"]
    assert re.search(r'sound,\s*SettingEntry\{"volume",\s*volume_page3_factory,\s*PageType::FullCustom\}', builder)
    assert "settings_mic_gain_page_factory" in builder
    assert 'SettingEntry{"system sounds", system_sounds_api, true}' in builder


def test_wifi_menu_joins_hidden_networks_from_the_list():
    wifi_entries = re.findall(
        r'append_child\(\s*wifi,\s*SettingEntry\{"([^"]+)"',
        settings_tree_builder(),
    )
    assert wifi_entries == ["enable", "networks"]
    assert 'SettingEntry{"enable", wifi_power_api, true}' in SETTINGS_PAGE
    assert 'SettingEntry{"networks", wifi_scan_page3_factory, PageType::FullCustom}' in SETTINGS_PAGE
    assert "wifi_add_hidden_page_factory" not in SETTINGS_PAGE
    assert '"+ hidden network"' in WIFI_PAGE
    assert 'cp0::ui::title_row(hidden_panel_, "settings/wi-fi/hidden")' in WIFI_PAGE


def test_bluetooth_menu_has_power_paired_and_scan():
    bluetooth_entries = re.findall(
        r'append_child\(\s*bluetooth,\s*SettingEntry\{"([^"]+)"',
        settings_tree_builder(),
    )
    assert bluetooth_entries == ["power", "paired", "scan"]
    assert "bt_named_only" not in SETTINGS_PAGE
    assert "BtDiscoverable" not in SETTINGS_PAGE


def test_ethernet_submenu_shows_the_address_read_only():
    ethernet_entries = re.findall(
        r'append_child\(\s*ethernet,\s*SettingEntry\{"?([^"},]+)',
        settings_tree_builder(),
    )
    assert ethernet_entries == ["enable", "std::string(kEthernetIpPrefix) + "]
    assert 'constexpr const char *kEthernetIpPrefix = "ip: ";' in SETTINGS_PAGE
    assert 'settings_interface_ipv4("eth0")' in SETTINGS_PAGE
    assert 'SettingEntry{"enable", ethernet_enabled_api, true}' in SETTINGS_PAGE
    assert '"GENERAL.STATE", "device", "show", "eth0"' in ETHERNET_CONTROLLER
    assert "GENERAL.AUTOCONNECT" not in SETTINGS_PAGE
    assert "GENERAL.AUTOCONNECT" not in ETHERNET_CONTROLLER
    assert 'operation == Operation::Connect ? "connect" : "disconnect"' in ETHERNET_CONTROLLER


def test_time_menu_order():
    builder = settings_tree_builder()
    time_block = builder[builder.index("NodeIter date_time =") : builder.index("NodeIter system =")]
    assert "std::string(kTimeNowPrefix)" in time_block
    labels = re.findall(r'SettingEntry \w+\{"([^"]+)"|SettingEntry\{"([^"]+)"', time_block)
    assert [a or b for a, b in labels] == [
        "time", "network time", "time zone", "clock format", "set manually"]
    assert "settings_rtc_editor_page_factory, PageType::FullCustom" in time_block
    assert "append_clock_format_options" in time_block


def test_ext_port_order_and_hardware_mappings():
    ext_port_entries = re.findall(
        r'append_child\(\s*ext_port,\s*SettingEntry\{"([^"]+)"',
        settings_tree_builder(),
    )
    assert ext_port_entries == ["ext 5v", "grove 5v"]
    assert 'SettingEntry{"ext 5v", std::bind(&ext_port_com, "EXT5V"' in SETTINGS_PAGE
    assert 'SettingEntry{"grove 5v", std::bind(&ext_port_com, "GROVE5V"' in SETTINGS_PAGE


def test_launcher_settings_use_builtin_and_release_preinstalled_apps():
    assert SETTINGS_ADAPTER.count("launcher_app_registry_entries(&count)") == 2
    assert "launcher_builtin_app_registry_entries(&count)" not in SETTINGS_ADAPTER
    assert "launcher::is_launcher_settings_origin(descriptor.origin)" in SETTINGS_ADAPTER
    assert "launcher::is_protected_launcher_app(descriptor.config_key)" in SETTINGS_ADAPTER
    assert '{"Python", "python_100.png", "app_Python", true, false}' in BUILTIN_REGISTRY
    assert '"Snake"' not in BUILTIN_REGISTRY and '"Tank"' not in BUILTIN_REGISTRY
    assert (
        "zclaw.desktop\tZClaw\tshare/images/claw_100.png\t"
        "/usr/share/APPLaunch/bin/ZClaw\tfalse\ttrue"
    ) in PREINSTALLED_MANIFEST


def test_system_entries_and_power_actions():
    builder = settings_tree_builder()
    system = builder[builder.index("NodeIter system =") :]
    system_entries = re.findall(
        r'append_child\(\s*system,\s*SettingEntry\{"([^"]+)"',
        system,
    )
    assert system_entries == ["about", "hardware", "updates"]
    assert "std::string(kDeviceNamePrefix)" in system
    assert 'SettingEntry ssh_entry{"ssh server", ssh_server_api, true};' in system
    assert "settings_t12b_about_page_factory" in system
    assert "settings_hardware_page_factory" in system
    assert "settings_updates_page_factory" in system
    assert "settings_storage_page_factory" not in system
    assert "settings_credit_page_factory" not in system
    assert system.index("ssh_entry") < system.index('"updates"')
    assert system.index("boot_actions::Action::Reboot") < system.index("boot_actions::Action::Shutdown")
    assert "settings_power_request(action)" in SETTINGS_PAGE
    for removed in ('SettingEntry{"OS"', 'SettingEntry{"APPLaunch"',
                    'SettingEntry{"Help"', 'SettingEntry{"Boot"'):
        assert removed not in system


if __name__ == "__main__":
    test_root_menu_matches_product_order()
    test_screen_remains_the_initial_selection()
    test_screen_keeps_lock_timeout_options()
    test_sound_holds_volume_and_mic_gain()
    test_wifi_menu_joins_hidden_networks_from_the_list()
    test_bluetooth_menu_has_power_paired_and_scan()
    test_ethernet_submenu_shows_the_address_read_only()
    test_time_menu_order()
    test_ext_port_order_and_hardware_mappings()
    test_launcher_settings_use_builtin_and_release_preinstalled_apps()
    test_system_entries_and_power_actions()
