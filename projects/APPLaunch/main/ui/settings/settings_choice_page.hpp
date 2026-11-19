/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "hal_lvgl_bsp.h"

#include <cstdint>
#include <functional>
#include <list>
#include <string>

#include "lvgl_components.hpp"

// Value page whose options come from the tree node's children. Reading the
// current index and applying a new one both run on a worker thread, so they
// may block (process calls, privileged helpers).
class LvSettingChoicePage3 : public LvSettingValuePage3Base {
public:
    // Index of the current value, or -1 when it cannot be read.
    using Reader = std::function<int()>;
    // Applies the option at index; returns an empty string or an error message.
    using Writer = std::function<std::string(int)>;
    // Runs on the LVGL thread after a successful write, before the page closes.
    using Applied = std::function<void(int)>;

    LvSettingChoicePage3(lv_obj_t *parent, const NodeIter &parent_node, std::function<void()> back_callback,
                         Reader reader, Writer writer, Applied applied = {});
    ~LvSettingChoicePage3() override;

protected:
    int initial_selection() const override;
    SettingApiResult activate_selected() override;

private:
    void request_back();
    void set_status(const std::string &text, bool error);
    bool request_is_current(std::uint64_t generation) const;
    void begin_load();
    void begin_write();

    Reader reader_;
    Writer writer_;
    Applied applied_;
    std::function<void()> back_callback_;
    lv_obj_t *status_label_ = nullptr;
    bool page_alive_ = true;
    bool back_requested_ = false;
    bool pending_ = false;
    int saved_index_ = -1;
    std::uint64_t generation_ = 0;
};

// Runs a whitelisted privileged command (see cp0_sudo_async.cpp) and waits for
// it. Returns an empty string on success or a short error message.
std::string settings_run_admin(std::list<std::string> arguments, int timeout_ms = 30000);
