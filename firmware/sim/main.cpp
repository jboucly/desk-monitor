// Desk Monitor desktop simulator.
//
// Interactive (default): 480x480 SDL window, mouse = touch, REST API + web editor on http://localhost:8080.
// Headless (--do …): scripted run that renders off-screen and writes PNG screenshots, e.g.
//   desk_monitor_sim --do page=0 --do shot=home.png --do long=300,260 --do wait=300 --do shot=popup.png
#include <SDL2/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "dm/api.h"
#include "dm/app.h"
#include "dm/bus.h"
#include "dm/pc.h"
#include "dm/settings.h"
#include "dm/theme.h"
#include "httplib.h"
#include "lvgl.h"
#include "mock.h"
#include "png.h"
#include "ws_server.h"

namespace {

namespace fs = std::filesystem;

/**
 * @description Storage backed by a folder of the build directory.
 */
class FileStorage : public dm::Storage {
 public:
  explicit FileStorage(fs::path dir) : dir_(std::move(dir)) { fs::create_directories(dir_); }

  std::optional<std::string> read(const std::string &name) override {
    std::ifstream in(dir_ / name, std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
  }

  bool write(const std::string &name, const std::string &data) override {
    const fs::path tmp = dir_ / (name + ".tmp");
    {
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      if (!out.write(data.data(), static_cast<std::streamsize>(data.size()))) return false;
    }
    std::error_code ec;
    fs::rename(tmp, dir_ / name, ec);
    return !ec;
  }

 private:
  fs::path dir_;
};

std::string read_file(const char *path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// ── Headless mode: simulated clock, scripted pointer, off-screen frame buffer ──
uint32_t g_sim_ms = 0;
std::atomic<bool> g_pressed{false};
lv_point_t g_point = {0, 0};
uint16_t g_frame[dm::theme::SCREEN_W * dm::theme::SCREEN_H];

// Screenshot handshake between the HTTP thread and the LVGL loop (--no-window mode)
std::mutex g_shot_mutex;
std::condition_variable g_shot_ready;
bool g_shot_requested = false;
std::vector<uint8_t> g_shot;

/**
 * @description Advances the simulated clock while running LVGL.
 */
void run_for(uint32_t ms) {
  for (uint32_t t = 0; t < ms; t += 5) {
    g_sim_ms += 5;
    lv_timer_handler();
  }
}

/**
 * @description Executes one scripted command: page=N, tap=X,Y, long=X,Y, wait=MS, shot=FILE.png, bench=N.
 */
bool run_command(dm::App &app, const std::string &command) {
  const auto eq = command.find('=');
  const std::string verb = command.substr(0, eq), arg = eq == std::string::npos ? "" : command.substr(eq + 1);
  int x = 0, y = 0;
  if (verb == "page") {
    app.show_page(std::stoi(arg), false);
    run_for(100);
  } else if (verb == "tap" || verb == "long") {
    if (sscanf(arg.c_str(), "%d,%d", &x, &y) != 2) return false;
    g_point = {x, y};
    g_pressed = true;
    run_for(verb == "tap" ? 80 : 900);
    g_pressed = false;
    run_for(100);
  } else if (verb == "wait") {
    run_for(static_cast<uint32_t>(std::stoi(arg)));
  } else if (verb == "shot") {
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(nullptr);
    if (!sim::write_png_rgb565(arg, g_frame, dm::theme::SCREEN_W, dm::theme::SCREEN_H)) return false;
    printf("[sim] wrote %s\n", arg.c_str());
  } else if (verb == "bench") {
    // Full-screen redraws, like every frame of a page swipe (same breakdown as the device logs at boot)
    printf("[sim] bench: %s\n", app.benchmark(std::stoi(arg)).c_str());
  } else {
    return false;
  }
  return true;
}

// Fixed forecast so that headless screenshots are reproducible
const char *CANNED_WEATHER = R"({"name":"Paris","current":{"t":13.4,"feel":11.2,"code":2,"day":true,"rh":72,"wind":14.3},
"daily":[{"wday":3,"code":2,"tmin":8.2,"tmax":15.1},{"wday":4,"code":61,"tmin":9.1,"tmax":13.2},
{"wday":5,"code":3,"tmin":7.4,"tmax":12.8},{"wday":6,"code":0,"tmin":5.9,"tmax":14.9},
{"wday":0,"code":80,"tmin":6.3,"tmax":11.7},{"wday":1,"code":95,"tmin":4.8,"tmax":10.2}]})";

void post_status(dm::Bus &bus) {
  bus.post("sys/wifi", cJSON_Parse(R"({"rssi":-58,"ip":"127.0.0.1"})"));
  bus.post("sys/ha", cJSON_Parse(R"({"state":"ok"})"));
}

/**
 * @description Fake PC companion (headless runs and --mock-pc): connected, with drifting metrics.
 */
void post_mock_pc(dm::Bus &bus, int step) {
  if (step == 0) bus.post("sys/pc", cJSON_Parse(R"({"state":"connected","name":"MacBook Pro"})"));
  const double wave = (step % 20) / 20.0;
  char metrics[320];
  snprintf(metrics, sizeof metrics,
           R"({"cpu":%.1f,"cpu_temp":%.0f,"ram":%.1f,"ram_used":%.0f,"ram_total":17179869184,)"
           R"("gpu":%.1f,"gpu_temp":%.0f,"vram":41,"disk":68,"net_rx":%.0f,"net_tx":%.0f})",
           18 + 30 * wave, 52 + 10 * wave, 61 + 4 * wave, 17179869184 * (0.61 + 0.04 * wave), 9 + 70 * wave,
           48 + 14 * wave, 2.4e6 * wave + 3200, 180000 * wave + 900);
  bus.post("pc", cJSON_Parse(metrics));
}

}  // namespace

int main(int argc, char **argv) {
  // Logs appear at once even when redirected to a file
  setvbuf(stdout, nullptr, _IOLBF, 0);
  std::vector<std::string> commands;
  int port = 8080;
  bool no_window = false;
  bool mock_pc = false;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--do") && i + 1 < argc) commands.push_back(argv[++i]);
    else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--no-window")) no_window = true;
    else if (!strcmp(argv[i], "--mock-pc")) mock_pc = true;
    else {
      fprintf(stderr, "usage: %s [--port N] [--no-window] [--mock-pc] [--do page=N|tap=X,Y|long=X,Y|wait=MS|shot=FILE.png|bench=N]...\n",
              argv[0]);
      return 2;
    }
  }
  const bool headless = !commands.empty();

  lv_init();
  lv_display_t *display;
  if (headless || no_window) {
    // Scripted runs use a simulated clock; --no-window runs in real time
    if (headless) lv_tick_set_cb([] { return g_sim_ms; });
    else
      lv_tick_set_cb([] {
        static const auto start = std::chrono::steady_clock::now();
        return static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
      });
    display = lv_display_create(dm::theme::SCREEN_W, dm::theme::SCREEN_H);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, g_frame, nullptr, sizeof g_frame, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(display, [](lv_display_t *d, const lv_area_t *, uint8_t *) { lv_display_flush_ready(d); });
    lv_indev_t *pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, [](lv_indev_t *, lv_indev_data_t *data) {
      data->point = g_point;
      data->state = g_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    });
  } else {
    display = lv_sdl_window_create(dm::theme::SCREEN_W, dm::theme::SCREEN_H);
    lv_sdl_window_set_title(display, "Desk Monitor");
    lv_sdl_mouse_create();
  }

  FileStorage storage(DM_SIM_DATA_PATH);
  dm::Settings settings(storage);
  settings.load();

  dm::Bus bus;
  sim::MockHome home(bus);
  std::mutex home_mutex;  // the HTTP thread reads the mock while LVGL updates it

  dm::Services services;
  services.ha_call = [&](const std::string &domain, const std::string &service, const std::string &entity,
                         cJSON *data) {
    std::lock_guard<std::mutex> lock(home_mutex);
    home.call(domain, service, entity, data);
  };
  // PC companion: the real WebSocket link on port + 1, or a fake PC (headless runs, --mock-pc)
  if (headless) mock_pc = true;
  std::random_device entropy;
  dm::PcSession pc(bus, settings, [&entropy] { return static_cast<uint32_t>(entropy()); });
  services.pc_action = [&pc, mock_pc](const cJSON *action) {
    if (!mock_pc) return pc.send_action(action);
    printf("[mock PC] action %s\n", dm::json::print(action).c_str());
    return true;
  };

  dm::App app(bus, services, storage, read_file(DM_DEFAULT_LAYOUT_PATH));
  app.on_entities_changed = [&](const std::set<std::string> &entities) {
    std::lock_guard<std::mutex> lock(home_mutex);
    home.set_entities(entities);
  };
  app.start();
  post_status(bus);
  if (mock_pc) post_mock_pc(bus, 0);

  const dm::SettingsData s = settings.get();
  if (headless) {
    bus.post("weather", cJSON_Parse(CANNED_WEATHER));
    run_for(200);
    for (const auto &command : commands) {
      if (!run_command(app, command)) {
        fprintf(stderr, "[sim] bad command: %s\n", command.c_str());
        return 1;
      }
    }
    return 0;
  }

  sim::fetch_weather(bus, s.weather_lat, s.weather_lon, s.weather_name);
  settings.on_change = [&](unsigned changes) {
    if (changes & dm::Settings::PC) pc.forget();
    if (changes & dm::Settings::WEATHER) {
      const dm::SettingsData now = settings.get();
      sim::fetch_weather(bus, now.weather_lat, now.weather_lon, now.weather_name);
    }
  };

  // Sensors drift a little so that gauges move
  lv_timer_create(
      [](lv_timer_t *t) {
        auto *ctx = static_cast<std::pair<sim::MockHome *, std::mutex *> *>(lv_timer_get_user_data(t));
        std::lock_guard<std::mutex> lock(*ctx->second);
        ctx->first->tick();
      },
      3000, new std::pair<sim::MockHome *, std::mutex *>(&home, &home_mutex));

  if (mock_pc) {
    lv_timer_create(
        [](lv_timer_t *t) {
          static int step = 0;
          post_mock_pc(*static_cast<dm::Bus *>(lv_timer_get_user_data(t)), ++step);
        },
        1000, &bus);
  } else {
    sim::WsHandlers ws;
    ws.on_open = [&pc](std::function<void(const std::string &)> send, std::function<void()> close) {
      printf("[sim] PC connected\n");
      return pc.on_open(std::move(send), std::move(close));
    };
    ws.on_message = [&pc](uint32_t id, const std::string &text) { pc.on_message(id, text); };
    ws.on_close = [&pc](uint32_t id) {
      printf("[sim] PC disconnected\n");
      pc.on_close(id);
    };
    if (sim::ws_listen(port + 1, "/api/pc", ws)) printf("[sim] PC link on ws://localhost:%d/api/pc\n", port + 1);
    else fprintf(stderr, "[sim] cannot listen on port %d for the PC link\n", port + 1);
    lv_timer_create([](lv_timer_t *t) { static_cast<dm::PcSession *>(lv_timer_get_user_data(t))->poll(); }, 1000, &pc);
  }

  // REST API + web editor (built with `npm run build` in web/)
  dm::Api api(app, settings);
  const auto started = std::chrono::steady_clock::now();
  api.status = [&] {
    const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started);
    return "{\"version\":\"sim\",\"platform\":\"simulator\",\"ip\":\"127.0.0.1\",\"uptime\":" +
           std::to_string(uptime.count()) + "}";
  };
  api.ha_entities = [&] {
    std::lock_guard<std::mutex> lock(home_mutex);
    return home.entity_list_json();
  };
  static httplib::Server server;
  server.set_mount_point("/", DM_WEB_DIST_PATH);
  auto handler = [&api](const httplib::Request &req, httplib::Response &res) {
    dm::HttpResponse r = api.handle(req.method, req.path, req.body, req.get_header_value("X-DM-Pin"));
    res.status = r.status;
    res.set_content(r.body, r.content_type);
  };
  // Live screenshot of the simulated screen, rendered by the LVGL loop below
  server.Get("/sim/screenshot.png", [](const httplib::Request &, httplib::Response &res) {
    std::unique_lock<std::mutex> lock(g_shot_mutex);
    g_shot_requested = true;
    g_shot_ready.wait_for(lock, std::chrono::seconds(2), [] { return !g_shot_requested; });
    res.set_content(reinterpret_cast<const char *>(g_shot.data()), g_shot.size(), "image/png");
  });
  // Scripted tap for end-to-end tests in --no-window mode: /sim/tap?x=414&y=410
  server.Get("/sim/tap", [no_window](const httplib::Request &req, httplib::Response &res) {
    if (!no_window) {
      res.status = 400;
      return;
    }
    g_point = {std::atoi(req.get_param_value("x").c_str()), std::atoi(req.get_param_value("y").c_str())};
    g_pressed = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    g_pressed = false;
    res.set_content("{\"ok\":true}", "application/json");
  });
  server.Get(R"(/api/.*)", handler);
  server.Put(R"(/api/.*)", handler);
  server.Post(R"(/api/.*)", handler);
  std::thread([port] {
    printf("[sim] editor on http://localhost:%d\n", port);
    if (!server.listen("0.0.0.0", port)) fprintf(stderr, "[sim] cannot listen on port %d\n", port);
  }).detach();

  while (true) {
    const uint32_t idle = lv_timer_handler();
    if (no_window) {
      std::lock_guard<std::mutex> lock(g_shot_mutex);
      if (g_shot_requested) {
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(nullptr);
        g_shot = sim::encode_png_rgb565(g_frame, dm::theme::SCREEN_W, dm::theme::SCREEN_H);
        g_shot_requested = false;
        g_shot_ready.notify_all();
      }
    }
    SDL_Delay(idle < 5 ? idle : 5);
  }
}
