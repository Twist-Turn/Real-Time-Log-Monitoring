#pragma once
/**
 * @file Dashboard.hpp
 * @brief Enhanced 4-panel ncurses terminal dashboard.
 *
 * Layout:
 *   ┌─────────────────────┬──────────────────────┐
 *   │  Connected Apps     │  Live Log Feed       │
 *   │  (with stats)       │  (scrollable)        │
 *   ├─────────────────────┼──────────────────────┤
 *   │  Alert History      │  Throughput Graph    │
 *   │  (last 10)          │  (ASCII bar chart)   │
 *   └─────────────────────┴──────────────────────┘
 *
 * Keyboard controls:
 *   Tab    - Cycle active panel (for scrolling)
 *   ↑/↓    - Scroll active panel
 *   r/R    - Reload alert rules
 *   q/Q    - Quit
 *
 * Refresh rate: 500ms (configurable)
 *
 * SIGWINCH (terminal resize): handled via atomic flag — windows are
 * recreated on the next render tick.
 *
 * Thread safety: All ncurses calls are made only from the thread calling
 * run() (main thread). TSDB, AlertRulesEngine are read via their thread-safe
 * accessor methods.
 */

#include "AlertManager.hpp"
#include "AlertRulesEngine.hpp"
#include "FileWatcher.hpp"
#include "LogPipeline.hpp"
#include "NetworkIngester.hpp"
#include "StatsAggregator.hpp"
#include "TSDB.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <deque>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef __has_include
    #if __has_include(<ncurses.h>)
        #include <ncurses.h>
    #elif __has_include(<curses.h>)
        #include <curses.h>
    #endif
#else
    #include <curses.h>
#endif

// SIGWINCH for terminal resize
#include <csignal>

namespace logmonitor {

// ─── Global resize flag (set by SIGWINCH handler) ───
// Must be file-scope for signal handler access
static std::atomic<bool> g_dashboard_resize{false};

static void dashboard_sigwinch_handler(int) {
    g_dashboard_resize.store(true, std::memory_order_relaxed);
}

// ─── Panel indices ───
enum class Panel : int { APPS = 0, LOG_FEED = 1, ALERT_HISTORY = 2, THROUGHPUT = 3 };

class Dashboard {
public:
    static constexpr int NUM_PANELS       = 4;
    static constexpr int THROUGHPUT_SLOTS = 60;  // 60 seconds of history

    // Color pair IDs
    static constexpr int CP_INFO     = 1;
    static constexpr int CP_WARN     = 2;
    static constexpr int CP_ERROR    = 3;
    static constexpr int CP_CRITICAL = 4;
    static constexpr int CP_HEADER   = 5;
    static constexpr int CP_NETWORK  = 6;
    static constexpr int CP_NORMAL   = 7;
    static constexpr int CP_ACTIVE   = 8;   // Active panel border
    static constexpr int CP_DIMMED   = 9;   // Inactive panel border

    Dashboard(const LogPipeline& pipeline,
              const AlertManager& alert_mgr,
              const StatsAggregator& stats,
              const std::vector<std::shared_ptr<FileWatchState>>& file_states,
              const NetworkIngester* network,
              const TSDB* tsdb,
              const AlertRulesEngine* rules_engine,
              std::atomic<bool>& stop_flag,
              int refresh_ms = 500)
        : pipeline_(pipeline)
        , alert_mgr_(alert_mgr)
        , stats_(stats)
        , file_states_(file_states)
        , network_(network)
        , tsdb_(tsdb)
        , rules_engine_(rules_engine)
        , stop_flag_(stop_flag)
        , refresh_ms_(refresh_ms)
        , active_panel_(static_cast<int>(Panel::LOG_FEED))
    {
        scroll_offsets_.fill(0);
        panels_.fill(nullptr);
    }

    ~Dashboard() { stop(); }

    /// Initialize ncurses and run the UI loop (blocking, main thread only)
    void run() {
        init_ncurses();
        std::signal(SIGWINCH, dashboard_sigwinch_handler);
        create_panels();

        while (!stop_flag_.load(std::memory_order_relaxed)) {
            // Handle resize
            if (g_dashboard_resize.exchange(false)) {
                endwin();
                refresh();
                destroy_panels();
                create_panels();
            }

            // Push throughput sample (one per tick)
            {
                auto lps = stats_.lines_per_second();
                throughput_history_.push_back(static_cast<double>(lps));
                while (static_cast<int>(throughput_history_.size()) > THROUGHPUT_SLOTS) {
                    throughput_history_.pop_front();
                }
            }

            render_all();

            std::this_thread::sleep_for(std::chrono::milliseconds(refresh_ms_));

            handle_input();
        }

        destroy_panels();
        cleanup_ncurses();
    }

    void stop() {
        if (ncurses_active_) {
            destroy_panels();
            cleanup_ncurses();
        }
    }

private:
    // ─── ncurses lifecycle ───

    void init_ncurses() {
        initscr();
        cbreak();
        noecho();
        curs_set(0);
        nodelay(stdscr, TRUE);
        keypad(stdscr, TRUE);

        if (has_colors()) {
            start_color();
            use_default_colors();
            init_pair(CP_INFO,     COLOR_GREEN,   -1);
            init_pair(CP_WARN,     COLOR_YELLOW,  -1);
            init_pair(CP_ERROR,    COLOR_RED,     -1);
            init_pair(CP_CRITICAL, COLOR_RED,     -1);
            init_pair(CP_HEADER,   COLOR_CYAN,    -1);
            init_pair(CP_NETWORK,  COLOR_MAGENTA, -1);
            init_pair(CP_NORMAL,   COLOR_WHITE,   -1);
            init_pair(CP_ACTIVE,   COLOR_GREEN,   -1);  // Active panel border
            init_pair(CP_DIMMED,   COLOR_WHITE,   -1);  // Inactive panel border
        }
        ncurses_active_ = true;
    }

    void cleanup_ncurses() {
        if (ncurses_active_) {
            endwin();
            ncurses_active_ = false;
        }
    }

    void create_panels() {
        int total_y, total_x;
        getmaxyx(stdscr, total_y, total_x);

        // Reserve 2 rows: 1 title + 1 footer
        int panel_area_y = total_y - 2;
        int half_y = panel_area_y / 2;
        int half_x = total_x / 2;

        // [0] Top-left: Connected Apps
        panels_[0] = newwin(half_y, half_x, 1, 0);
        // [1] Top-right: Live Log Feed
        panels_[1] = newwin(half_y, total_x - half_x, 1, half_x);
        // [2] Bottom-left: Alert History
        panels_[2] = newwin(panel_area_y - half_y, half_x, 1 + half_y, 0);
        // [3] Bottom-right: Throughput Graph
        panels_[3] = newwin(panel_area_y - half_y, total_x - half_x, 1 + half_y, half_x);

        if (panels_[0]) keypad(panels_[0], TRUE);
        if (panels_[1]) keypad(panels_[1], TRUE);
        if (panels_[2]) keypad(panels_[2], TRUE);
        if (panels_[3]) keypad(panels_[3], TRUE);
    }

    void destroy_panels() {
        for (auto& p : panels_) {
            if (p) { delwin(p); p = nullptr; }
        }
    }

    // ─── Rendering ───

    void render_all() {
        int total_y, total_x;
        getmaxyx(stdscr, total_y, total_x);

        // Title bar
        attron(A_BOLD | COLOR_PAIR(CP_HEADER));
        std::string title = " LOG MONITOR  [Tab=switch panel] [arrows=scroll] [r=reload rules] [q=quit]";
        std::string tput  = "  " + std::to_string(stats_.lines_per_second()) + " lines/s  ";
        std::string title_padded = title;
        int pad = total_x - static_cast<int>(title.size()) - static_cast<int>(tput.size());
        if (pad > 0) title_padded += std::string(static_cast<std::size_t>(pad), ' ');
        title_padded += tput;
        mvprintw(0, 0, "%.*s", total_x, title_padded.c_str());
        attroff(A_BOLD | COLOR_PAIR(CP_HEADER));

        // Footer
        attron(A_DIM);
        std::string footer = " Total: " + std::to_string(pipeline_.total_lines())
                           + "  Matched: " + std::to_string(pipeline_.total_matched())
                           + "  Latency: " + to_str(pipeline_.avg_latency_us()) + "us"
                           + "  Alerts: I=" + std::to_string(alert_mgr_.info_count())
                           + " W=" + std::to_string(alert_mgr_.warn_count())
                           + " E=" + std::to_string(alert_mgr_.error_count())
                           + " C=" + std::to_string(alert_mgr_.critical_count());
        mvprintw(total_y - 1, 0, "%.*s", total_x, footer.c_str());
        attroff(A_DIM);

        // Panels
        if (panels_[0]) render_apps_panel(panels_[0], active_panel_ == 0);
        if (panels_[1]) render_log_feed_panel(panels_[1], active_panel_ == 1);
        if (panels_[2]) render_alert_history_panel(panels_[2], active_panel_ == 2);
        if (panels_[3]) render_throughput_panel(panels_[3], active_panel_ == 3);

        refresh();
        for (auto& p : panels_) { if (p) wrefresh(p); }
    }

    // ─── Panel 0: Connected Apps ───
    void render_apps_panel(WINDOW* win, bool active) const {
        int h, w;
        getmaxyx(win, h, w);
        werase(win);

        // Border
        wattron(win, COLOR_PAIR(active ? CP_ACTIVE : CP_DIMMED));
        box(win, 0, 0);
        wattroff(win, COLOR_PAIR(active ? CP_ACTIVE : CP_DIMMED));

        // Title
        wattron(win, A_BOLD | COLOR_PAIR(CP_HEADER));
        mvwprintw(win, 0, 2, " Connected Apps ");
        wattroff(win, A_BOLD | COLOR_PAIR(CP_HEADER));

        if (!tsdb_) {
            mvwprintw(win, 2, 2, "TSDB not available");
            return;
        }

        // Header row
        wattron(win, A_BOLD);
        mvwprintw(win, 1, 1, "%-20.20s %6s %6s %6s",
                  "App", "Total", "ERR/m", "Last");
        wattroff(win, A_BOLD);
        mvwhline(win, 2, 1, ACS_HLINE, w - 2);

        auto apps = tsdb_->getApps();
        int row = 3;
        int start = scroll_offsets_[static_cast<int>(Panel::APPS)];
        for (int i = start; i < static_cast<int>(apps.size()) && row < h - 1; ++i, ++row) {
            const auto& app = apps[static_cast<std::size_t>(i)];
            int total  = tsdb_->getCount(app, "", 3600);
            int errors = tsdb_->getCount(app, "ERROR", 60);

            auto last_ns = tsdb_->getLastSeen(app);
            std::string last_str = format_time_ns(last_ns);

            // Highlight apps with recent errors
            bool has_errors = (errors > 0);
            if (has_errors) wattron(win, COLOR_PAIR(CP_ERROR));

            mvwprintw(win, row, 1, "%-20.20s %6d %6d %6.6s",
                      app.c_str(), total, errors, last_str.c_str());

            if (has_errors) wattroff(win, COLOR_PAIR(CP_ERROR));
        }

        // Scroll indicator
        if (static_cast<int>(apps.size()) > (h - 4)) {
            wattron(win, A_DIM);
            mvwprintw(win, h - 1, 1, " ↑↓ scroll (%d/%d) ",
                      scroll_offsets_[0] + 1,
                      static_cast<int>(apps.size()));
            wattroff(win, A_DIM);
        }
    }

    // ─── Panel 1: Live Log Feed ───
    void render_log_feed_panel(WINDOW* win, bool active) const {
        int h, w;
        getmaxyx(win, h, w);
        werase(win);

        wattron(win, COLOR_PAIR(active ? CP_ACTIVE : CP_DIMMED));
        box(win, 0, 0);
        wattroff(win, COLOR_PAIR(active ? CP_ACTIVE : CP_DIMMED));

        wattron(win, A_BOLD | COLOR_PAIR(CP_HEADER));
        mvwprintw(win, 0, 2, " Live Log Feed ");
        wattroff(win, A_BOLD | COLOR_PAIR(CP_HEADER));

        // Collect all recent lines across all sources
        std::vector<std::string> all_lines;
        for (const auto& src : pipeline_.get_sources()) {
            auto lines = pipeline_.get_recent_lines(src);
            for (const auto& l : lines) {
                all_lines.push_back("[" + src + "] " + l);
            }
        }

        int total_lines = static_cast<int>(all_lines.size());
        int display_rows = h - 2;
        int scroll = scroll_offsets_[static_cast<int>(Panel::LOG_FEED)];
        int start  = std::max(0, total_lines - display_rows - scroll);
        int end    = std::min(total_lines, start + display_rows);

        int row = 1;
        for (int i = start; i < end && row < h - 1; ++i, ++row) {
            const auto& line = all_lines[static_cast<std::size_t>(i)];
            int color = log_line_color(line);
            wattron(win, COLOR_PAIR(color));
            mvwprintw(win, row, 1, "%.*s", w - 2, line.c_str());
            wattroff(win, COLOR_PAIR(color));
        }

        if (total_lines > display_rows) {
            wattron(win, A_DIM);
            mvwprintw(win, h - 1, 1, " ↑↓ scroll (%d lines) ", total_lines);
            wattroff(win, A_DIM);
        }
    }

    // ─── Panel 2: Alert History ───
    void render_alert_history_panel(WINDOW* win, bool active) const {
        int h, w;
        getmaxyx(win, h, w);
        werase(win);

        wattron(win, COLOR_PAIR(active ? CP_ACTIVE : CP_DIMMED));
        box(win, 0, 0);
        wattroff(win, COLOR_PAIR(active ? CP_ACTIVE : CP_DIMMED));

        wattron(win, A_BOLD | COLOR_PAIR(CP_HEADER));
        mvwprintw(win, 0, 2, " Alert History (last 10) ");
        wattroff(win, A_BOLD | COLOR_PAIR(CP_HEADER));

        if (!rules_engine_) {
            mvwprintw(win, 2, 2, "AlertRulesEngine not available");
            return;
        }

        // Header
        wattron(win, A_BOLD);
        mvwprintw(win, 1, 1, "%-12.12s %-12.12s %-8.8s %-8.8s",
                  "Time", "Rule", "App", "State");
        wattroff(win, A_BOLD);
        mvwhline(win, 2, 1, ACS_HLINE, w - 2);

        auto history = rules_engine_->get_fired_history();
        int row      = 3;
        int start    = scroll_offsets_[static_cast<int>(Panel::ALERT_HISTORY)];
        for (int i = start; i < static_cast<int>(history.size()) && row < h - 1;
             ++i, ++row) {
            const auto& a = history[static_cast<std::size_t>(i)];
            std::string ts = format_time_ns(a.fired_at_ns);

            wattron(win, COLOR_PAIR(CP_ERROR));
            mvwprintw(win, row, 1, "%-12.12s %-12.12s %-8.8s %-8s",
                      ts.c_str(),
                      a.rule_id.c_str(),
                      a.app.c_str(),
                      (a.delivered ? "SENT" : "FIRED"));
            wattroff(win, COLOR_PAIR(CP_ERROR));
        }

        if (history.empty()) {
            wattron(win, COLOR_PAIR(CP_INFO));
            mvwprintw(win, 3, 2, "No alerts fired yet");
            wattroff(win, COLOR_PAIR(CP_INFO));
        }
    }

    // ─── Panel 3: Throughput Graph ───
    void render_throughput_panel(WINDOW* win, bool active) const {
        int h, w;
        getmaxyx(win, h, w);
        werase(win);

        wattron(win, COLOR_PAIR(active ? CP_ACTIVE : CP_DIMMED));
        box(win, 0, 0);
        wattroff(win, COLOR_PAIR(active ? CP_ACTIVE : CP_DIMMED));

        wattron(win, A_BOLD | COLOR_PAIR(CP_HEADER));
        mvwprintw(win, 0, 2, " Throughput (logs/sec, last 60s) ");
        wattroff(win, A_BOLD | COLOR_PAIR(CP_HEADER));

        if (throughput_history_.empty()) {
            mvwprintw(win, 2, 2, "No data yet...");
            return;
        }

        // ASCII bar chart
        int chart_h = h - 3;    // rows available for bars (leave title + bottom border)
        int chart_w = w - 4;    // cols available
        int n_bars  = std::min(static_cast<int>(throughput_history_.size()), chart_w);

        double max_val = 1.0;
        for (const auto& v : throughput_history_) {
            max_val = std::max(max_val, v);
        }

        int start = static_cast<int>(throughput_history_.size()) - n_bars;
        for (int i = 0; i < n_bars && (2 + i) < w - 2; ++i) {
            double v = throughput_history_[static_cast<std::size_t>(start + i)];
            int bar_h = static_cast<int>((v / max_val) * chart_h);

            // Choose color based on height
            int color = CP_INFO;
            if (v / max_val > 0.75)      color = CP_ERROR;
            else if (v / max_val > 0.4)  color = CP_WARN;

            // Draw bar from bottom up
            for (int r = 0; r < bar_h && r < chart_h; ++r) {
                int screen_row = h - 2 - r;
                wattron(win, COLOR_PAIR(color) | A_BOLD);
                mvwaddstr(win, screen_row, 2 + i, "█");
                wattroff(win, COLOR_PAIR(color) | A_BOLD);
            }
        }

        // Y-axis label
        wattron(win, A_DIM);
        mvwprintw(win, 1, 2, "max:%.0f", max_val);
        mvwprintw(win, h - 2, 2, "0");
        wattroff(win, A_DIM);
    }

    // ─── Input handling ───
    void handle_input() {
        int ch = getch();
        if (ch == ERR) return;

        switch (ch) {
            case 'q': case 'Q':
                stop_flag_.store(true, std::memory_order_relaxed);
                break;

            case '\t':
                active_panel_ = (active_panel_ + 1) % NUM_PANELS;
                break;

            case 'r': case 'R':
                if (rules_engine_) {
                    const_cast<AlertRulesEngine*>(rules_engine_)->reload();
                }
                break;

            case KEY_UP:
                if (scroll_offsets_[static_cast<std::size_t>(active_panel_)] > 0) {
                    scroll_offsets_[static_cast<std::size_t>(active_panel_)]--;
                }
                break;

            case KEY_DOWN:
                scroll_offsets_[static_cast<std::size_t>(active_panel_)]++;
                break;

            default:
                break;
        }
    }

    // ─── Helpers ───

    static int log_line_color(const std::string& line) {
        if (line.find("CRITICAL") != std::string::npos) return CP_CRITICAL;
        if (line.find("ERROR")    != std::string::npos) return CP_ERROR;
        if (line.find("WARN")     != std::string::npos) return CP_WARN;
        if (line.find("INFO")     != std::string::npos) return CP_INFO;
        return CP_NORMAL;
    }

    static std::string format_time_ns(int64_t ts_ns) {
        if (ts_ns <= 0) return "--:--:--";
        auto sec = static_cast<std::time_t>(ts_ns / 1'000'000'000LL);
        char buf[10];
        std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&sec));
        return buf;
    }

    static std::string to_str(double v) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(0) << v;
        return oss.str();
    }

    // ─── Members ───
    const LogPipeline& pipeline_;
    const AlertManager& alert_mgr_;
    const StatsAggregator& stats_;
    [[maybe_unused]] const std::vector<std::shared_ptr<FileWatchState>>& file_states_;
    [[maybe_unused]] const NetworkIngester* network_;
    const TSDB* tsdb_;
    const AlertRulesEngine* rules_engine_;
    std::atomic<bool>& stop_flag_;
    int refresh_ms_;
    int active_panel_;
    bool ncurses_active_{false};

    std::array<WINDOW*, NUM_PANELS> panels_;
    std::array<int, NUM_PANELS> scroll_offsets_;
    std::deque<double> throughput_history_;
};

} // namespace logmonitor
