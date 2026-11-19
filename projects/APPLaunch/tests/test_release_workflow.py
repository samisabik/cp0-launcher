#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
# SPDX-License-Identifier: MIT

# Contract for the build and release workflows: what is built, what a release
# publishes, and the guards that keep a release from reusing a version.

from pathlib import Path

repo = Path(__file__).resolve().parents[3]

workflow = (repo / ".github" / "workflows" / "launcher-build.yml").read_text(encoding="utf-8")
assert "branches: [main, ci/**]" in workflow
assert "if: ${{ inputs.publish == true && github.ref == 'refs/heads/main' }}" in workflow
assert "startsWith(github.ref, 'refs/heads/ci/')" not in workflow
assert "${{ steps.version.outputs.version }}" in workflow
# The release tag is pinned to the commit that was actually built.
assert "target_commitish: ${{ github.sha }}" in workflow
# A manual release is a normal GitHub Release; CI runs stay pre-releases.
assert "prerelease: ${{ !inputs.publish }}" in workflow
# The versioned release rejects missing assets.
assert workflow.count("fail_on_unmatched_files: true") == 1
assert "overwrite_files: true" in workflow
# The release ships the install tarball the README downloads, and only that.
assert "files: artifacts/cp0-launcher-arm64.tar.gz" in workflow
assert 'sh scripts/package_release.sh "$GITHUB_WORKSPACE/release"' in workflow
assert ".deb" not in workflow
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
assert "persist-credentials: false" in workflow
assert "if-no-files-found: error" in workflow

release_workflow = (repo / ".github" / "workflows" / "launcher-release.yml").read_text(encoding="utf-8")
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

# The tarball carries everything install.sh installs.
packer = (repo / "scripts" / "package_release.sh").read_text(encoding="utf-8")
installer = (repo / "scripts" / "device_install.sh").read_text(encoding="utf-8")
assert "device_install.sh" in packer and "50-cp0-wifi-scan.rules" in packer
assert "/etc/polkit-1/rules.d/50-cp0-wifi-scan.rules" in installer
rule = (repo / "scripts" / "50-cp0-wifi-scan.rules").read_text(encoding="utf-8")
assert 'subject.isInGroup("netdev")' in rule
for action in ("wifi.scan", "network-control", "enable-disable-wifi", "settings.modify.own", "settings.modify.system"):
    assert f'"org.freedesktop.NetworkManager.{action}"' in rule

print("test_release_workflow: ok")
