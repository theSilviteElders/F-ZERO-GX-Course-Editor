// F-Zero GX Course Editor: SDL3 + OpenGL 3.3 + Dear ImGui.
//   fzgx-editor [--workspace DIR]
#include "editor.hpp"

#include <SDL3/SDL.h>
#include <epoxy/gl.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>

#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>
#include <string_view>

using namespace fzgx;

namespace
{
fs::path Home()
{
  const char* h = std::getenv("HOME");
  return h ? fs::path(h) : fs::path(".");
}

// Fills in sensible defaults for anything editor.ini does not set.
void DetectDefaults(Settings& s)
{
  std::error_code ec;
  const fs::path root = fs::path(FZGX_SOURCE_DIR);  // repository root
  // ROM: <repo>/rom/ or a rom/ folder next to the repository.
  for (const fs::path& dir : {root / "rom", root.parent_path() / "rom"})
  {
    if (!s.rom.empty())
      break;
    for (const auto& e : fs::directory_iterator(dir, ec))
    {
      const auto ext = e.path().extension().string();
      if (ext == ".rvz" || ext == ".iso" || ext == ".RVZ" || ext == ".ISO")
      {
        s.rom = e.path();
        break;
      }
    }
  }
  if (s.glinux_appimage.empty())
  {
    for (const fs::path& dir : {Home() / ".var/app/io.github.mabecgames.FZeroGLinux", Home() / "Applications",
                               Home() / "Downloads", root})
      for (const auto& e : fs::directory_iterator(dir, ec))
      {
        const std::string n = e.path().filename().string();
        if (n.find("GLINUX") != std::string::npos && e.path().extension() == ".AppImage")
          s.glinux_appimage = e.path();
      }
  }
  if (s.glinux_user_dir.empty())
  {
    const char* xdg = std::getenv("XDG_DATA_HOME");
    s.glinux_user_dir = (xdg ? fs::path(xdg) : Home() / ".local/share") / "fzero-glinux";
  }
}

void LoadFont(float scale)
{
  ImGuiIO& io = ImGui::GetIO();
  static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2010, 0x2027, 0x00B0, 0x00B0, 0};
  for (const char* path : {"/usr/share/fonts/noto/NotoSans-Regular.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
                           "/usr/share/fonts/dejavu/DejaVuSans.ttf", "/usr/share/fonts/liberation/LiberationSans-Regular.ttf"})
    if (fs::exists(path) && io.Fonts->AddFontFromFileTTF(path, 16.0f * scale, nullptr, ranges))
    {
      io.FontGlobalScale = 1.0f / scale;
      return;
    }
  io.Fonts->AddFontDefault();
}
}  // namespace

int main(int argc, char** argv)
{
  Settings settings;
  settings.workspace = fs::path(FZGX_SOURCE_DIR) / "work";  // git-ignored
  // --screenshot FILE.ppm [--course N] [--select I] [--collision] [--no-textures] [--place S]:
  // render, save, quit (for testing and documentation).
  std::string screenshot;
  int start_course = -1, start_select = -1, start_place = -1;
  bool start_collision = false, no_textures = false;
  for (int i = 1; i < argc; ++i)
  {
    const std::string_view a = argv[i];
    if (a == "--workspace" && i + 1 < argc)
      settings.workspace = fs::absolute(argv[++i]);
    else if (a == "--screenshot" && i + 1 < argc)
      screenshot = argv[++i];
    else if (a == "--course" && i + 1 < argc)
      start_course = std::atoi(argv[++i]);
    else if (a == "--select" && i + 1 < argc)
      start_select = std::atoi(argv[++i]);
    else if (a == "--place" && i + 1 < argc)
      start_place = std::atoi(argv[++i]);  // place structure S next to its original, select it
    else if (a == "--collision")
      start_collision = true;
    else if (a == "--no-textures")
      no_textures = true;
  }
  settings.Load();
  DetectDefaults(settings);
  if (no_textures)
    settings.textures = false;

  if (!SDL_Init(SDL_INIT_VIDEO))
  {
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
  SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);
  SDL_Window* window = SDL_CreateWindow("F-Zero GX Course Editor", 1600, 900,
                                        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!window)
  {
    // Retry without multisampling.
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
    window = SDL_CreateWindow("F-Zero GX Course Editor", 1600, 900,
                              SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  }
  if (!window)
  {
    std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
    return 1;
  }
  SDL_GLContext gl = SDL_GL_CreateContext(window);
  if (!gl)
  {
    std::fprintf(stderr, "OpenGL 3.3: %s\n", SDL_GetError());
    return 1;
  }
  SDL_GL_MakeCurrent(window, gl);
  SDL_GL_SetSwapInterval(1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  static std::string ini = (settings.workspace / "imgui.ini").string();
  std::error_code ec;
  fs::create_directories(settings.workspace, ec);
  io.IniFilename = ini.c_str();
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowRounding = 0;
  style.FrameRounding = 3;
  style.Colors[ImGuiCol_Header] = ImVec4(0.85f, 0.45f, 0.1f, 0.45f);
  style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.9f, 0.5f, 0.15f, 0.6f);
  style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.95f, 0.55f, 0.2f, 0.8f);
  ImGui_ImplSDL3_InitForOpenGL(window, gl);
  ImGui_ImplOpenGL3_Init("#version 330 core");
  LoadFont(SDL_GetWindowDisplayScale(window));

  auto editor_ptr = std::make_unique<Editor>(settings);
  Editor& editor = *editor_ptr;
  if (!editor.Init())
  {
    std::fprintf(stderr, "renderer init failed\n");
    return 1;
  }

  editor.DebugStart(start_course, start_select, start_collision, start_place);
  int frame = 0;
  Uint64 last = SDL_GetTicksNS();
  while (!editor.WantsQuit())
  {
    SDL_Event ev;
    while (SDL_PollEvent(&ev))
    {
      ImGui_ImplSDL3_ProcessEvent(&ev);
      if (ev.type == SDL_EVENT_QUIT ||
          (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && ev.window.windowID == SDL_GetWindowID(window)))
        editor.RequestQuit();
    }
    if (!screenshot.empty())
      io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);  // screenshots: no hover effects from the real mouse
    const Uint64 now = SDL_GetTicksNS();
    const float dt = std::min(0.1f, float(now - last) / 1e9f);
    last = now;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    int fw, fh, ww, wh;
    SDL_GetWindowSizeInPixels(window, &fw, &fh);
    SDL_GetWindowSize(window, &ww, &wh);
    glViewport(0, 0, fw, fh);
    glClearColor(0.06f, 0.06f, 0.08f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    editor.Frame(fw, fh, ww > 0 ? float(fw) / float(ww) : 1.0f, dt);

    ImGui::Render();
    glViewport(0, 0, fw, fh);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    if (!screenshot.empty() && ++frame == 20)
    {
      std::vector<unsigned char> px(std::size_t(fw) * fh * 3);
      glPixelStorei(GL_PACK_ALIGNMENT, 1);
      glReadPixels(0, 0, fw, fh, GL_RGB, GL_UNSIGNED_BYTE, px.data());
      if (FILE* f = std::fopen(screenshot.c_str(), "wb"))
      {
        std::fprintf(f, "P6 %d %d 255\n", fw, fh);
        for (int y = fh - 1; y >= 0; --y)
          std::fwrite(px.data() + std::size_t(y) * fw * 3, 1, std::size_t(fw) * 3, f);
        std::fclose(f);
      }
      break;
    }
    SDL_GL_SwapWindow(window);
  }

  editor_ptr.reset();  // frees GL objects while the context is alive
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  SDL_GL_DestroyContext(gl);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
