#include "tui/menu.h"

namespace motion::tui {

Menu::Menu(const Terminal& term, std::string title, std::string subtitle)
    : m_term(term), m_title(std::move(title)), m_subtitle(std::move(subtitle)) {}

void Menu::buildFrame(Frame& f, int selected) const {
    draw::banner(f);

    if (!m_title.empty())
        draw::title(f, m_title);

    if (!m_subtitle.empty()) {
        f.line(std::string(color::gray) + "  " + m_subtitle + color::reset);
    }
    f.line();

    const int total = (int)m_items.size();
    int titleLines = m_title.empty() ? 0 : 2;
    int subLines = 0;
    if (!m_subtitle.empty()) { subLines = 1; for (char c : m_subtitle) if (c == '\n') ++subLines; }
    int chrome = 8 + titleLines + subLines + 2 + 1;
    int budget = m_term.rows() - chrome - 1;
    if (budget < 4) budget = 4;

    int first = 0, last = total;
    if (total > budget) {
        int vis = budget - 2;
        if (vis < 1) vis = 1;
        first = selected - vis / 2;
        if (first < 0) first = 0;
        if (first > total - vis) first = total - vis;
        last = first + vis;
    }

    if (first > 0)
        f.line(std::string(color::gray) + "  ▲ " + std::to_string(first) + " more" + color::reset);

    for (int i = first; i < last; ++i) {
        const MenuItem& item = m_items[i];
        const bool sel = (i == selected);

        std::string line = "  ";
        if (sel) {
            line += std::string(color::brightCyan) + color::bold + "❯ " + color::reset;
            line += std::string(color::invert) + color::brightCyan + " " + item.label + " " + color::reset;
        } else {
            line += "  ";
            line += item.enabled ? item.label
                                 : std::string(color::gray) + item.label + color::reset;
        }
        if (!item.hint.empty())
            line += std::string("   ") + color::gray + item.hint + color::reset;
        f.line(line);
    }

    if (last < total)
        f.line(std::string(color::gray) + "  ▼ " + std::to_string(total - last) + " more" + color::reset);

    f.line();
    draw::footer(f, m_footer);
}

int Menu::run(int startIndex) {
    if (m_items.empty()) return -1;

    int selected = startIndex;
    if (selected < 0 || selected >= (int)m_items.size()) selected = 0;
    for (int n = 0; n < (int)m_items.size() && !m_items[selected].enabled; ++n)
        selected = (selected + 1) % (int)m_items.size();

    while (true) {
        Frame f;
        buildFrame(f, selected);
        m_term.present(f);

        KeyEvent ev = m_term.readKey();
        m_selected = selected;
        switch (ev.key) {
            case Key::Up:
                do { selected = (selected - 1 + (int)m_items.size()) % (int)m_items.size(); }
                while (!m_items[selected].enabled);
                break;
            case Key::Down:
                do { selected = (selected + 1) % (int)m_items.size(); }
                while (!m_items[selected].enabled);
                break;
            case Key::Enter:
                if (m_items[selected].enabled) return selected;
                break;
            case Key::Escape:
                return -1;
            case Key::Left:
                if (m_hotkeys.find('a') != std::string::npos) { m_hotkey = 'a'; return kHotkey; }
                break;
            case Key::Right:
                if (m_hotkeys.find('d') != std::string::npos) { m_hotkey = 'd'; return kHotkey; }
                break;
            case Key::Char: {
                char c = (char)(ev.ch | 0x20);
                if (c == 'w') {
                    do { selected = (selected - 1 + (int)m_items.size()) % (int)m_items.size(); }
                    while (!m_items[selected].enabled);
                    break;
                }
                if (c == 's') {
                    do { selected = (selected + 1) % (int)m_items.size(); }
                    while (!m_items[selected].enabled);
                    break;
                }
                if (ev.ch && m_hotkeys.find(ev.ch) != std::string::npos) {
                    m_hotkey = ev.ch;
                    return kHotkey;
                }
                break;
            }
            default:
                break;
        }
    }
}

namespace draw {

void banner(Frame& f) {
    static const char* art[] = {
        "  ███╗   ███╗ ██████╗ ████████╗██╗ ██████╗ ███╗   ██╗",
        "  ████╗ ████║██╔═══██╗╚══██╔══╝██║██╔═══██╗████╗  ██║",
        "  ██╔████╔██║██║   ██║   ██║   ██║██║   ██║██╔██╗ ██║",
        "  ██║╚██╔╝██║██║   ██║   ██║   ██║██║   ██║██║╚██╗██║",
        "  ██║ ╚═╝ ██║╚██████╔╝   ██║   ██║╚██████╔╝██║ ╚████║",
        "  ╚═╝     ╚═╝ ╚═════╝    ╚═╝   ╚═╝ ╚═════╝ ╚═╝  ╚═══╝",
    };
    f.raw(color::brightCyan);
    for (const char* l : art) f.line(l);
    f.raw(color::reset);
    f.line(std::string(color::gray) + "        live wallpaper cli  ·  Dev by tenzo" + color::reset);
    f.line();
}

void title(Frame& f, const std::string& t) {
    f.line(std::string(color::bold) + color::brightCyan + "  " + t + color::reset);
    f.line();
}

void footer(Frame& f, const std::string& hint) {
    f.line(std::string(color::gray) + "  " + hint + color::reset);
}

}

}
