/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <string>

namespace settings_system {

struct UpdateStatusInfo
{
    // succeeded, failed or cancelled: the job is gone and will not report again.
    bool terminal = false;
    // 0..100, or -1 when the status carries no percentage.
    int progress = -1;
    std::string stage;
};

// Parses an `UpdateJobStatus` reply: `running`, `<stage>:<percent>`,
// `succeeded:<detail>`, `failed:<detail>:<code>` or `cancelled`.
UpdateStatusInfo parse_update_status(const std::string &state);

} // namespace settings_system
