#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
# SPDX-License-Identifier: MIT

import importlib.util
import os
import sys
import tempfile
import time
from pathlib import Path

repo = Path(__file__).resolve().parents[3]
module_path = repo / "scripts" / "debian_packager.py"
spec = importlib.util.spec_from_file_location("debian_packager_updater", module_path)
packager = importlib.util.module_from_spec(spec)
assert spec.loader
sys.modules[spec.name] = packager
spec.loader.exec_module(packager)


def ar_members(data: bytes) -> dict[str, bytes]:
    assert data.startswith(b"!<arch>\n")
    members: dict[str, bytes] = {}
    offset = 8
    while offset < len(data):
        header = data[offset:offset + 60]
        assert len(header) == 60 and header[58:60] == b"`\n"
        name = header[:16].decode("ascii").rstrip().removesuffix("/")
        size = int(header[48:58])
        offset += 60
        members[name] = data[offset:offset + size]
        offset += size + (size % 2)
    return members


old_source_date_epoch = os.environ.get("SOURCE_DATE_EPOCH")
try:
    os.environ["SOURCE_DATE_EPOCH"] = "1704067200"
    control = packager._control_text(packager.PackageConfig())
    assert "Packaged-Date: 2024-01-01 00:00:00 UTC\n" in control
    assert packager._ar_member_header("data.tar.gz", 1)[16:28].strip() == b"1704067200"

    with tempfile.TemporaryDirectory() as temp_dir:
        root = Path(temp_dir)
        package_root = root / "package"
        control_root = package_root / "DEBIAN"
        data_root = package_root / "usr" / "share" / "APPLaunch"
        control_root.mkdir(parents=True)
        data_root.mkdir(parents=True)
        (control_root / "control").write_text(control, encoding="utf-8")
        (control_root / "postinst").write_text("#!/bin/sh\n", encoding="ascii")
        (control_root / "prerm").write_text("#!/bin/sh\n", encoding="ascii")
        (data_root / "payload.txt").write_text("deterministic payload\n", encoding="ascii")

        first_deb = root / "first.deb"
        second_deb = root / "second.deb"
        packager.build_deb_with_python(package_root, first_deb)
        time.sleep(1.1)
        packager.build_deb_with_python(package_root, second_deb)

        first_bytes = first_deb.read_bytes()
        assert first_bytes == second_deb.read_bytes()
        members = ar_members(first_bytes)
        for name in ("control.tar.gz", "data.tar.gz"):
            compressed_tar = members[name]
            assert compressed_tar[:4] == b"\x1f\x8b\x08\x00"
            assert int.from_bytes(compressed_tar[4:8], "little") == 1704067200
finally:
    if old_source_date_epoch is None:
        os.environ.pop("SOURCE_DATE_EPOCH", None)
    else:
        os.environ["SOURCE_DATE_EPOCH"] = old_source_date_epoch

assert str(packager.POLKIT_LOCAL_RULES_PATH) == "etc/polkit-1/rules.d"
for action in ("wifi.scan", "network-control", "enable-disable-wifi",
               "settings.modify.own", "settings.modify.system"):
    assert f'"org.freedesktop.NetworkManager.{action}"' in packager._wifi_polkit_text()
assert 'subject.isInGroup("netdev")' in packager._wifi_polkit_text()
assert '"50-cp0-wifi-scan.rules"' in Path(packager.__file__).read_text()

workflow = (repo / ".github" / "workflows" / "launcher-build.yml").read_text(
    encoding="utf-8"
)
assert "branches: [main, ci/**]" in workflow
assert "if: ${{ inputs.publish == true && github.ref == 'refs/heads/main' }}" in workflow
assert "startsWith(github.ref, 'refs/heads/ci/')" not in workflow
assert "scripts/build_cardputerzero_release_local.sh" in workflow
assert "python3 scripts/debian_packager.py --version" not in workflow
assert "${{ steps.version.outputs.version }}" in workflow
# The release tag is pinned to the commit that was actually built.
assert "target_commitish: ${{ github.sha }}" in workflow
# A manual release is a normal GitHub Release; CI runs stay pre-releases.
assert "prerelease: ${{ !inputs.publish }}" in workflow
# The versioned release rejects missing assets.
assert workflow.count("fail_on_unmatched_files: true") == 1
assert "overwrite_files: true" in workflow
# Deleted releases and installed versions can still raise the version floor.
assert "LAUNCHER_RELEASE_FLOOR" in workflow
assert "git tag -l 'launcher-v[0-9]*'" in workflow
assert "10#${BASH_REMATCH[3]}" in workflow
assert "Publishing requires an explicit version" in workflow
assert "Verify release preconditions" in workflow
assert "environment: release" in workflow
# CI builds its sysroot from Debian packages; there is no downloaded BSP.
assert "sh scripts/make_sysroot.sh --docker" in workflow
assert "key: sysroot-${{ hashFiles('scripts/make_sysroot.sh') }}" in workflow
assert "sdk_version.txt" not in workflow
assert "persist-credentials: false" in workflow
assert "if-no-files-found: error" in workflow

release_workflow = (repo / ".github" / "workflows" / "launcher-release.yml").read_text(
    encoding="utf-8"
)
assert "workflow_dispatch:" in release_workflow
for trigger in ("push:", "pull_request:", "schedule:", "workflow_run:"):
    assert trigger not in release_workflow
assert "required: true" in release_workflow
assert "uses: ./.github/workflows/launcher-build.yml" in release_workflow
assert "publish: true" in release_workflow
assert "version: ${{ inputs.version }}" in release_workflow
assert "needs: guard" in release_workflow
assert "refs/heads/main" in release_workflow
assert "CONFIRM" in release_workflow
assert "timeout-minutes:" in release_workflow

release_builder = (repo / "scripts" / "build_cardputerzero_release_local.sh").read_text(
    encoding="utf-8"
)
assert 'CLEAN_BUILD=${CLEAN_BUILD:-1}' in release_builder
assert '"$ROOT/projects/$project/main/build"' in release_builder


with tempfile.TemporaryDirectory() as temp_dir:
    root = Path(temp_dir)
    project_dir = root / "projects" / "APPLaunch"
    src_dir = project_dir / "dist"
    images_dir = src_dir / "APPLaunch" / "share" / "images"
    source_images = project_dir / "APPLaunch" / "share" / "images"
    images_dir.mkdir(parents=True)
    source_images.mkdir(parents=True)
    expected_images = {"cli_100.png", "ssh_100.png", "line/settings.png"}
    (images_dir / "line").mkdir()
    for name in expected_images:
        (images_dir / name).write_bytes(b"png")
    (source_images / "source-only.png").write_bytes(b"not a build output")
    (src_dir / "M5CardputerZero-APPLaunch").write_bytes(b"binary")

    config = packager.PackageConfig()
    package_root = root / "staging" / "debian-APPLaunch"
    paths = packager.Paths(
        repo_root=root,
        tool_dir=root / "scripts",
        project_dir=project_dir,
        src_dir=src_dir,
        output_dir=root / "out",
        work_dir=root / "staging",
        package_root=package_root,
        package_file=root / "out" / config.file_name,
    )
    packager.prepare_package_tree(config, paths)

    packaged_images = package_root / "usr" / "share" / "APPLaunch" / "share" / "images"
    assert all((packaged_images / name).is_file() for name in expected_images)
    assert not (packaged_images / "source-only.png").exists()
    assert (package_root / "etc/polkit-1/rules.d/50-cp0-wifi-scan.rules").is_file()
