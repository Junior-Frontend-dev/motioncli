#pragma once
#include "tui/terminal.h"

#include <string>
#include <vector>

namespace motion::tui {

struct MenuItem {
    std::string label;
    std::string hint;
    bool enabled = true;
};

class Menu {
public:
    Menu(const Terminal& term, std::string title, std::string subtitle = "");

    void setItems(std::vector<MenuItem> items) { m_items = std::move(items); }
    void setFooter(std::string footer) { m_footer = std::move(footer); }

    void setHotkeys(std::string keys) { m_hotkeys = std::move(keys); }
    char hotkey() const { return m_hotkey; }
    int  selectedIndex() const { return m_selected; }
    static constexpr int kHotkey = -2;

    int run(int startIndex = 0);

private:
    void buildFrame(Frame& f, int selected) const;

    const Terminal& m_term;
    std::string m_title;
    std::string m_subtitle;
    std::string m_footer = "↑/↓ move   ⏎ select   esc back";
    std::vector<MenuItem> m_items;
    std::string m_hotkeys;
    mutable char m_hotkey = 0;
    mutable int m_selected = 0;
};

namespace draw {
    void banner(Frame& f);
    void title(Frame& f, const std::string& t);
    void footer(Frame& f, const std::string& hint);
}

}
